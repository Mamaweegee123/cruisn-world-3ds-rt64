#include "cruisn_rt64_bridge.hpp"
#include "cruisn_rdram_snapshot.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cruisn::rt64_bridge;

namespace {

struct TestMemory final : RdramImage {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kRdramBytes, 0);

    const uint8_t* data() const noexcept override { return bytes.data(); }
    size_t size() const noexcept override { return bytes.size(); }
};

struct TestLease final : ReadLease {
    explicit TestLease(std::shared_ptr<TestMemory> image) : image_(std::move(image)) {}
    std::shared_ptr<TestMemory> image_;
    uint64_t memory_epoch = 1;
    bool complete = true;
    bool stable = true;

    std::shared_ptr<const RdramImage> image() const noexcept override { return image_; }
    uint64_t epoch() const noexcept override { return memory_epoch; }
    bool complete_rdram_image() const noexcept override { return complete; }
    bool stable_until_release() const noexcept override { return stable; }
};

int failures = 0;
int checks = 0;

void check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

void put32(TestMemory& memory, uint32_t address, uint32_t value) {
    size_t offset = 0;
    if (!translate_guest_span(address, sizeof(uint32_t), memory.size(), &offset)) {
        std::abort();
    }
    memory.bytes[offset + 0] = static_cast<uint8_t>(value);
    memory.bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    memory.bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    memory.bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get32(const RdramImage& image, uint32_t address) {
    size_t offset = 0;
    if (!translate_guest_span(address, sizeof(uint32_t), image.size(), &offset)) {
        std::abort();
    }
    const uint8_t* bytes = image.data() + offset;
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

constexpr uint32_t kTask = 0x80360888u;
constexpr uint32_t kRoot = 0x80399338u;

void make_synthetic_task(TestMemory& memory, uint32_t root = kRoot,
                         uint32_t root_size = 16) {
    const std::array<uint32_t, 16> words = {
        1, 0, 0x803311D0u, 0xD0u, 0x803312A0u, 0,
        0x80352CA0u, 0x800u, 0, 0, 0x80353380u, 0x80359300u,
        root, root_size, 0x80359380u, 0xC00u,
    };
    for (size_t i = 0; i < words.size(); ++i) {
        put32(memory, kTask + static_cast<uint32_t>(i * 4), words[i]);
    }
    put32(memory, 0x80359300u, 0); // output-size word required by task shape

    // Synthetic command stream: FullSync then EndDL. This is not a game task.
    put32(memory, root + 0, 0xE9000000u);
    put32(memory, root + 4, 0);
    put32(memory, root + 8, 0xDF000000u);
    put32(memory, root + 12, 0);
}

SubmitRequest request_for(const std::shared_ptr<TestMemory>& memory,
                          uint64_t sequence = 1, bool complete = true,
                          bool stable = true, uint64_t epoch = 1) {
    SubmitRequest request;
    request.ostask_guest_address = kTask;
    request.task_sequence = sequence;
    request.execution_state = ExecutionState::not_started;
    request.gbi = { rt64_3ds::Gbi::F3DEX2, true, true };
    auto lease = std::make_shared<TestLease>(memory);
    lease->complete = complete;
    lease->stable = stable;
    lease->memory_epoch = epoch;
    request.rdram = std::move(lease);
    return request;
}

void test_guest_address_translation() {
    size_t offset = 0;
    check(translate_guest_span(0x80399338u, 24, kRdramBytes, &offset) &&
          offset == 0x00399338u, "KSEG0 guest address maps to RDRAM offset");
    check(translate_guest_span(0x003E7B10u, 8, kRdramBytes, &offset) &&
          offset == 0x003E7B10u, "physical guest address maps directly");
    check(!translate_guest_span(0x807FFFFCu, 8, kRdramBytes, &offset),
          "span crossing the 8 MiB boundary is rejected");
    check(!translate_guest_span(0xA0399338u, 8, kRdramBytes, &offset),
          "KSEG1 address is rejected");
    check(!translate_guest_span(0x01000000u, 4, kRdramBytes, &offset),
          "out-of-range physical address is rejected");
}

void test_real_interpreter_invocation_and_lifetime() {
    auto memory = std::make_shared<TestMemory>();
    make_synthetic_task(*memory);
    const std::vector<uint8_t> original_guest_memory = memory->bytes;
    auto request = request_for(memory);
    std::weak_ptr<const ReadLease> weak_lease = request.rdram;
    std::weak_ptr<TestMemory> weak_memory = memory;
    Adapter adapter;
    SubmitResult result = adapter.submit(request);
    check(result.status == Status::ok, "valid synthetic task reaches real interpreter");
    check(result.submission && result.submission->frame &&
          result.submission->frame->has_fullsync,
          "interpreter produces a FullSync FrameRecord");
    check(adapter.interpreted_commands() == 2 && adapter.interpreter_invocations() == 1,
          "real interpreter consumes synthetic FullSync/EndDL list");
    check(memory->bytes == original_guest_memory,
          "adapter and interpreter leave synthetic guest RDRAM and OSTask unchanged");
    check(result.submission && result.submission->completion_owner ==
          GuestCompletionOwner::runtime,
          "guest completion remains owned by runtime");
    memory.reset();
    check(!weak_memory.expired(), "adapter owns the RDRAM allocation");
    result.submission.reset();
    request.rdram.reset();
    check(weak_lease.expired(), "task read lease releases after frame retirement");
    check(!weak_memory.expired(), "adapter keeps RDRAM allocation alive for Interpreter");
}

void test_task_metadata_rejection() {
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        put32(*memory, kTask, 2); // OSTask type is not graphics
        Adapter adapter;
        const auto result = adapter.submit(request_for(memory));
        check(result.status == Status::invalid_task_metadata &&
              adapter.interpreter_invocations() == 0,
              "non-graphics OSTask is rejected before interpreter invocation");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        put32(*memory, kTask + 13u * 4u, 25); // not 8-byte sized
        Adapter adapter;
        const auto result = adapter.submit(request_for(memory));
        check(result.status == Status::invalid_task_metadata &&
              adapter.interpreter_invocations() == 0,
              "misaligned display-list size is rejected");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        put32(*memory, 0x80359300u, 8); // output buffer already modified
        Adapter adapter;
        const auto result = adapter.submit(request_for(memory));
        check(result.status == Status::invalid_task_metadata,
              "inconsistent output metadata is rejected");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        Adapter adapter;
        auto request = request_for(memory);
        request.ostask_guest_address = 0x807FFFF8u;
        const auto result = adapter.submit(request);
        check(result.status == Status::invalid_task_address,
              "OSTask descriptor crossing RDRAM boundary is rejected");
    }
}

void test_lease_and_gbi_gates() {
    for (const int rejected_case : { 0, 1, 2 }) {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        auto request = request_for(memory, 1, rejected_case != 0,
                                   rejected_case != 1,
                                   rejected_case == 2 ? 0 : 1);
        Adapter adapter;
        const auto result = adapter.submit(request);
        check(result.status == Status::invalid_rdram_lease,
              "incomplete/unstable/unidentified memory lease is rejected");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        auto request = request_for(memory);
        request.gbi.microcode_identity_verified = false;
        Adapter adapter;
        const auto result = adapter.submit(request);
        check(result.status == Status::invalid_gbi_evidence,
              "unverified GBI identity is rejected");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        auto request = request_for(memory);
        request.gbi.task_command_semantics_verified = false;
        Adapter adapter;
        const auto result = adapter.submit(request);
        check(result.status == Status::invalid_gbi_evidence,
              "unreviewed task command semantics are rejected");
    }
    {
        auto memory = std::make_shared<TestMemory>();
        make_synthetic_task(*memory);
        auto request = request_for(memory);
        request.gbi.gbi = static_cast<rt64_3ds::Gbi>(0xFF);
        Adapter adapter;
        const auto result = adapter.submit(request);
        check(result.status == Status::invalid_gbi_evidence,
              "unsupported GBI selector is rejected");
    }
}

void test_upstream_perspnorm_dispatch_is_not_bridge_approval() {
    auto memory = std::make_shared<TestMemory>();
    constexpr uint32_t list_with_perspnorm = kRoot;
    constexpr uint32_t list_without_perspnorm = kRoot + 0x40;
    put32(*memory, list_with_perspnorm + 0, 0xDB0E0000u);
    put32(*memory, list_with_perspnorm + 4, 0x00000017u);
    put32(*memory, list_with_perspnorm + 8, 0xE9000000u);
    put32(*memory, list_with_perspnorm + 12, 0);
    put32(*memory, list_with_perspnorm + 16, 0xDF000000u);
    put32(*memory, list_with_perspnorm + 20, 0);
    put32(*memory, list_without_perspnorm + 0, 0xE9000000u);
    put32(*memory, list_without_perspnorm + 4, 0);
    put32(*memory, list_without_perspnorm + 8, 0xDF000000u);
    put32(*memory, list_without_perspnorm + 12, 0);

    rt64_3ds::Interpreter with_command(
        const_cast<uint8_t*>(memory->data()));
    rt64_3ds::FrameRecord frame_with_command;
    with_command.set_gbi(rt64_3ds::Gbi::F3DEX2);
    const bool fullsync_with = with_command.run(list_with_perspnorm,
                                                frame_with_command);

    rt64_3ds::Interpreter without_command(
        const_cast<uint8_t*>(memory->data()));
    rt64_3ds::FrameRecord frame_without_command;
    without_command.set_gbi(rt64_3ds::Gbi::F3DEX2);
    const bool fullsync_without = without_command.run(
        list_without_perspnorm, frame_without_command);

    check(fullsync_with && fullsync_without && frame_with_command.has_fullsync &&
          frame_without_command.has_fullsync,
          "synthetic lists with and without PERSPNORM reach FullSync");
    check(with_command.stats().commands == 3 &&
          with_command.stats().unknown == 0,
          "upstream consumes MOVEWORD index 0x0E without marking it unknown");
    check(frame_with_command.draws.size() == frame_without_command.draws.size() &&
          frame_with_command.verts.size() == frame_without_command.verts.size() &&
          frame_with_command.color_image == frame_without_command.color_image,
          "no-geometry synthetic records match; not a projection semantics test");

    make_synthetic_task(*memory);
    put32(*memory, kRoot + 0, 0xDB0E0000u);
    put32(*memory, kRoot + 4, 0x00000017u);
    put32(*memory, kRoot + 8, 0xE9000000u);
    put32(*memory, kRoot + 12, 0);
    put32(*memory, kRoot + 16, 0xDF000000u);
    put32(*memory, kRoot + 20, 0);
    put32(*memory, kTask + 13u * 4u, 24);
    auto request = request_for(memory);
    request.gbi.task_command_semantics_verified = false;
    Adapter adapter;
    const auto rejected = adapter.submit(request);
    check(rejected.status == Status::invalid_gbi_evidence &&
          adapter.interpreter_invocations() == 0,
          "task containing unvalidated PERSPNORM cannot enter adapter");
}

void test_order_and_duplicate_execution_gates() {
    auto memory = std::make_shared<TestMemory>();
    make_synthetic_task(*memory);
    Adapter adapter;
    auto first = adapter.submit(request_for(memory, 10));
    check(first.status == Status::ok, "first ordered task accepted");

    auto duplicate = adapter.submit(request_for(memory, 10));
    check(duplicate.status == Status::invalid_task_order &&
          adapter.interpreter_invocations() == 1,
          "duplicate task sequence is rejected before second interpreter run");

    auto prior = adapter.submit(request_for(memory, 9));
    check(prior.status == Status::invalid_task_order &&
          adapter.interpreter_invocations() == 1,
          "out-of-order task is rejected");

    auto already_request = request_for(memory, 11);
    already_request.execution_state = ExecutionState::already_started;
    auto already = adapter.submit(already_request);
    check(already.status == Status::task_already_executed &&
          adapter.interpreter_invocations() == 1,
          "previously executed graphics task is rejected");

    auto unknown_request = request_for(memory, 11);
    unknown_request.execution_state = ExecutionState::unknown;
    auto unknown = adapter.submit(unknown_request);
    check(unknown.status == Status::task_already_executed &&
          adapter.interpreter_invocations() == 1,
          "unknown execution ownership is rejected");

    // Cruis'n reuses OSTask slots. A later sequence at the same guest address
    // is valid and must not be rejected as a duplicate task.
    auto next = adapter.submit(request_for(memory, 11));
    check(next.status == Status::ok,
          "reused OSTask address is accepted at a later sequence");
}

void test_fullsync_failure_consumes_sequence() {
    auto memory = std::make_shared<TestMemory>();
    make_synthetic_task(*memory, kRoot, 8);
    put32(*memory, kRoot, 0xDF000000u);
    put32(*memory, kRoot + 4, 0);
    Adapter adapter;
    const auto result = adapter.submit(request_for(memory));
    check(result.status == Status::interpreter_did_not_reach_fullsync,
          "list without FullSync is explicitly rejected");
    const auto retry = adapter.submit(request_for(memory));
    check(retry.status == Status::invalid_task_order,
          "failed interpreter task cannot be silently replayed");
}

void test_interpreter_rejects_rdram_rebinding() {
    auto first_memory = std::make_shared<TestMemory>();
    make_synthetic_task(*first_memory);
    Adapter adapter;
    const auto first = adapter.submit(request_for(first_memory, 1));
    check(first.status == Status::ok, "interpreter binds to first RDRAM allocation");

    auto second_memory = std::make_shared<TestMemory>();
    make_synthetic_task(*second_memory);
    const auto second = adapter.submit(request_for(second_memory, 2));
    check(second.status == Status::invalid_rdram_lease &&
          adapter.interpreter_invocations() == 1,
          "interpreter cannot be rebound to a different RDRAM allocation");
}

void test_synchronized_snapshot_contract() {
    auto source = std::make_shared<TestMemory>();
    make_synthetic_task(*source);
    std::shared_mutex gate;

    // Hold the writer gate while a snapshot attempts to capture. The capture
    // must wait for the writer to finish, so its copied view is one serialized
    // post-write state rather than a torn read.
    std::promise<void> writer_has_gate;
    std::promise<void> release_writer;
    auto release = release_writer.get_future();
    std::thread writer([&] {
        std::unique_lock lock(gate);
        put32(*source, 0x100u, 0x12345678u);
        writer_has_gate.set_value();
        release.wait();
    });
    writer_has_gate.get_future().wait();

    std::promise<void> capture_started;
    auto capture_future = std::async(std::launch::async, [&] {
        capture_started.set_value();
        return SnapshotLease::capture(source, gate, 2);
    });
    capture_started.get_future().wait();
    check(capture_future.wait_for(std::chrono::milliseconds(25)) ==
          std::future_status::timeout,
          "snapshot waits while a concurrent guest-memory writer owns the gate");
    release_writer.set_value();
    writer.join();
    auto snapshot = capture_future.get();
    check(snapshot && snapshot->complete_rdram_image() && snapshot->epoch() == 2,
          "snapshot owns a complete task-time RDRAM image and epoch");
    auto snapshot_image = snapshot ? snapshot->image() : nullptr;
    check(snapshot_image && snapshot_image->data() != source->data() &&
          snapshot_image->size() == kRdramBytes,
          "snapshot has independent storage for the interpreter");
    check(snapshot_image && get32(*snapshot_image, 0x100u) == 0x12345678u,
          "snapshot includes the completed synthetic timer write");

    // Repeat with a synthetic message-queue worker to show this writer uses
    // the same gate and that the next epoch includes its completed writes.
    std::promise<void> message_writer_has_gate;
    std::promise<void> release_message_writer;
    auto release_message = release_message_writer.get_future();
    std::thread message_writer([&] {
        std::unique_lock lock(gate);
        put32(*source, 0x108u, 0xAABBCCDDu);
        put32(*source, 0x10Cu, 0x11223344u);
        message_writer_has_gate.set_value();
        release_message.wait();
    });
    message_writer_has_gate.get_future().wait();
    std::promise<void> message_capture_started;
    auto message_capture_future = std::async(std::launch::async, [&] {
        message_capture_started.set_value();
        return SnapshotLease::capture(source, gate, 3);
    });
    message_capture_started.get_future().wait();
    check(message_capture_future.wait_for(std::chrono::milliseconds(25)) ==
          std::future_status::timeout,
          "snapshot waits while synthetic message writer owns the gate");
    release_message_writer.set_value();
    message_writer.join();
    auto message_snapshot = message_capture_future.get();
    auto message_image = message_snapshot ? message_snapshot->image() : nullptr;
    check(message_image && get32(*message_image, 0x108u) == 0xAABBCCDDu &&
          get32(*message_image, 0x10Cu) == 0x11223344u &&
          get32(*snapshot_image, 0x108u) == 0 &&
          get32(*snapshot_image, 0x10Cu) == 0,
          "message writes appear atomically in next snapshot, not prior snapshot");

    // Source writes after capture do not alter the snapshot interpreted by RT64.
    {
        std::unique_lock lock(gate);
        put32(*source, kRoot, 0xDF000000u);
    }
    SubmitRequest request;
    request.ostask_guest_address = kTask;
    request.task_sequence = 1;
    request.execution_state = ExecutionState::not_started;
    request.gbi = { rt64_3ds::Gbi::F3DEX2, true, true };
    request.rdram = message_snapshot;
    std::weak_ptr<const ReadLease> weak_lease = message_snapshot;
    std::weak_ptr<const RdramImage> weak_image = message_image;
    {
        Adapter adapter;
        auto result = adapter.submit(request);
        check(result.status == Status::ok && result.submission &&
              result.submission->frame->has_fullsync,
              "interpreter reads the immutable snapshot after source mutation");
        check(adapter.interpreted_commands() == 2,
              "interpreter command boundary follows snapshot, not live source");
        request.rdram.reset();
        message_snapshot.reset();
        check(!weak_lease.expired(),
              "frame submission retains snapshot lease through retirement");
        result.submission.reset();
        check(weak_lease.expired(),
              "snapshot lease releases after frame retirement");
    }
    request.rdram.reset();
    snapshot.reset();
    message_snapshot.reset();
    snapshot_image.reset();
    message_image.reset();
    check(weak_image.expired(),
          "snapshot allocation releases after interpreter and frame owners retire");
}

} // namespace

int main() {
    test_guest_address_translation();
    test_real_interpreter_invocation_and_lifetime();
    test_task_metadata_rejection();
    test_lease_and_gbi_gates();
    test_order_and_duplicate_execution_gates();
    test_upstream_perspnorm_dispatch_is_not_bridge_approval();
    test_fullsync_failure_consumes_sequence();
    test_interpreter_rejects_rdram_rebinding();
    test_synchronized_snapshot_contract();
    std::printf("checks=%d failures=%d evidence=synthetic_host_contract_only\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}

#include "cruisn_rt64_bridge.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace cruisn::rt64_bridge {
namespace {

enum TaskWord : size_t {
    type = 0,
    flags,
    ucode_boot,
    ucode_boot_size,
    ucode,
    ucode_size,
    ucode_data,
    ucode_data_size,
    dram_stack,
    dram_stack_size,
    output,
    output_size_pointer,
    data_pointer,
    data_size,
    yield_pointer,
    yield_size,
};

constexpr uint32_t kGraphicsTask = 1;
constexpr uint32_t kUcodeBoot = 0x803311D0u;
constexpr uint32_t kUcodeBootSize = 0xD0u;
constexpr uint32_t kUcode = 0x803312A0u;
constexpr uint32_t kUcodeSize = 0;
constexpr uint32_t kUcodeData = 0x80352CA0u;
constexpr uint32_t kUcodeDataSize = 0x800u;
constexpr uint32_t kOutput = 0x80353380u;
constexpr uint32_t kOutputSizePointer = 0x80359300u;
constexpr uint32_t kYield = 0x80359380u;
constexpr uint32_t kYieldSize = 0xC00u;
constexpr uint32_t kUcodeReadExtent = 0xF80u;
constexpr uint32_t kMaxDisplayList = 0x10000u;

uint32_t read_word_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool span_ok(const uint8_t* rdram, size_t rdram_size, uint32_t address,
             size_t length, size_t alignment = 1) noexcept {
    if (length == 0) return true;
    if (alignment > 1 && (address % alignment) != 0) return false;
    size_t offset = 0;
    return translate_guest_span(address, length, rdram_size, &offset) &&
           rdram != nullptr && offset <= rdram_size &&
           length <= rdram_size - offset;
}

bool descriptor_valid(const RdramImage& image, uint32_t task_address,
                      std::array<uint32_t, 16>& words) noexcept {
    const uint8_t* const rdram = image.data();
    const size_t rdram_size = image.size();
    size_t task_offset = 0;
    if (rdram == nullptr ||
        !translate_guest_span(task_address, kOstaskBytes, rdram_size,
                              &task_offset) ||
        (task_address & 7u) != 0 ||
        task_offset > rdram_size ||
        kOstaskBytes > rdram_size - task_offset) {
        return false;
    }

    for (size_t i = 0; i < words.size(); ++i) {
        words[i] = read_word_le(rdram + task_offset + i * sizeof(uint32_t));
    }

    const uint32_t data = words[data_pointer];
    const uint32_t length = words[data_size];
    if (words[type] != kGraphicsTask || words[flags] != 0 ||
        words[ucode_boot] != kUcodeBoot ||
        words[ucode_boot_size] != kUcodeBootSize ||
        words[ucode] != kUcode || words[ucode_size] != kUcodeSize ||
        words[ucode_data] != kUcodeData ||
        words[ucode_data_size] != kUcodeDataSize ||
        words[dram_stack] != 0 || words[dram_stack_size] != 0 ||
        words[output] != kOutput ||
        words[output_size_pointer] != kOutputSizePointer ||
        words[yield_pointer] != kYield || words[yield_size] != kYieldSize ||
        (data & 7u) != 0 || length < 8 || length > kMaxDisplayList ||
        (length & 7u) != 0) {
        return false;
    }

    const uint32_t output_capacity = words[output_size_pointer] - words[output];
    if (words[output_size_pointer] <= words[output] ||
        output_capacity > 0x10000u || (output_capacity & 7u) != 0 ||
        !span_ok(rdram, rdram_size, words[ucode_boot],
                 words[ucode_boot_size], 8) ||
        !span_ok(rdram, rdram_size, words[ucode], kUcodeReadExtent, 8) ||
        !span_ok(rdram, rdram_size, words[ucode_data],
                 words[ucode_data_size], 8) ||
        !span_ok(rdram, rdram_size, data, length, 8) ||
        !span_ok(rdram, rdram_size, words[output], output_capacity, 8) ||
        !span_ok(rdram, rdram_size, words[output_size_pointer],
                 sizeof(uint32_t), 4) ||
        !span_ok(rdram, rdram_size, words[yield_pointer],
                 words[yield_size], 8)) {
        return false;
    }

    size_t output_size_offset = 0;
    if (!translate_guest_span(words[output_size_pointer], sizeof(uint32_t),
                              rdram_size, &output_size_offset) ||
        read_word_le(rdram + output_size_offset) != 0) {
        return false;
    }
    return true;
}

} // namespace

bool translate_guest_span(uint32_t guest_address, size_t length,
                          size_t rdram_size, size_t* offset) noexcept {
    if (offset == nullptr || rdram_size > kRdramBytes) return false;

    uint64_t physical = guest_address;
    if (guest_address >= 0x80000000u && guest_address < 0x80800000u) {
        physical = static_cast<uint64_t>(guest_address & 0x007FFFFFu);
    } else if (guest_address >= kRdramBytes) {
        return false;
    }

    if (physical > rdram_size || length > rdram_size - physical) return false;
    *offset = static_cast<size_t>(physical);
    return true;
}

SubmitResult Adapter::submit(const SubmitRequest& request) {
    SubmitResult result;
    if (!request.rdram) {
        result.status = Status::invalid_rdram_lease;
        return result;
    }
    const ReadLease& lease = *request.rdram;
    const auto image = lease.image();
    if (!image || image->data() == nullptr || image->size() != kRdramBytes ||
        lease.epoch() == 0 || !lease.complete_rdram_image() ||
        !lease.stable_until_release()) {
        result.status = Status::invalid_rdram_lease;
        return result;
    }
    if ((request.ostask_guest_address & 7u) != 0 ||
        !span_ok(image->data(), image->size(), request.ostask_guest_address,
                 kOstaskBytes, 8)) {
        result.status = Status::invalid_task_address;
        return result;
    }
    if (request.execution_state != ExecutionState::not_started) {
        result.status = Status::task_already_executed;
        return result;
    }
    if (!request.gbi.microcode_identity_verified ||
        !request.gbi.task_command_semantics_verified) {
        result.status = Status::invalid_gbi_evidence;
        return result;
    }
    if (request.gbi.gbi != rt64_3ds::Gbi::F3DEX2 &&
        request.gbi.gbi != rt64_3ds::Gbi::F3DEX) {
        result.status = Status::invalid_gbi_evidence;
        return result;
    }
    if (request.task_sequence == 0 ||
        request.task_sequence <= last_sequence_) {
        result.status = Status::invalid_task_order;
        return result;
    }

    std::array<uint32_t, 16> words{};
    if (!descriptor_valid(*image, request.ostask_guest_address, words)) {
        result.status = Status::invalid_task_metadata;
        return result;
    }

    if (interpreter_ && rdram_identity_ != image->data()) {
        result.status = Status::invalid_rdram_lease;
        return result;
    }

    // Reserve before calling the interpreter. Even a failed run must not be
    // retried under the same sequence: the runtime has no rollback contract.
    last_sequence_ = request.task_sequence;
    auto frame = std::make_unique<rt64_3ds::FrameRecord>();
    if (!interpreter_) {
        rdram_identity_ = image->data();
        rdram_image_ = image;
        interpreter_ = std::make_unique<rt64_3ds::Interpreter>(
            const_cast<uint8_t*>(image->data()));
    }
    interpreter_->set_gbi(request.gbi.gbi);
    ++interpreter_invocations_;
    if (!interpreter_->run(words[data_pointer], *frame) || !frame->has_fullsync) {
        result.status = Status::interpreter_did_not_reach_fullsync;
        return result;
    }

    auto submission = std::make_unique<FrameSubmission>();
    submission->task_sequence = request.task_sequence;
    submission->ostask_guest_address = request.ostask_guest_address;
    submission->completion_owner = GuestCompletionOwner::runtime;
    submission->rdram_lifetime = request.rdram;
    submission->frame = std::move(frame);
    result.status = Status::ok;
    result.submission = std::move(submission);
    return result;
}

uint32_t Adapter::interpreted_commands() const noexcept {
    return interpreter_ ? interpreter_->stats().commands : 0;
}

const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::invalid_argument: return "invalid_argument";
    case Status::invalid_rdram_lease: return "invalid_rdram_lease";
    case Status::invalid_task_address: return "invalid_task_address";
    case Status::invalid_task_metadata: return "invalid_task_metadata";
    case Status::invalid_gbi_evidence: return "invalid_gbi_evidence";
    case Status::invalid_task_order: return "invalid_task_order";
    case Status::task_already_executed: return "task_already_executed";
    case Status::interpreter_did_not_reach_fullsync:
        return "interpreter_did_not_reach_fullsync";
    }
    return "unknown";
}

} // namespace cruisn::rt64_bridge

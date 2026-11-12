#pragma once

#include "rt64_3ds_dl.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cruisn::rt64_bridge {

constexpr size_t kRdramBytes = 8u * 1024u * 1024u;
constexpr size_t kOstaskBytes = 64;

enum class Status {
    ok,
    invalid_argument,
    invalid_rdram_lease,
    invalid_task_address,
    invalid_task_metadata,
    invalid_gbi_evidence,
    invalid_task_order,
    task_already_executed,
    interpreter_did_not_reach_fullsync,
};

enum class ExecutionState {
    unknown,
    not_started,
    already_started,
};

enum class GuestCompletionOwner {
    runtime,
};

// Owns the RDRAM allocation for the lifetime of the Adapter. Its address must
// not move, though contents may change between task epochs.
class RdramImage {
public:
    virtual ~RdramImage() = default;
    virtual const uint8_t* data() const noexcept = 0;
    virtual size_t size() const noexcept = 0;
};

// This per-task lease promises a coherent, immutable view of all 8 MiB until
// its last owner is released by the backend. The current runtime does not
// provide this contract yet.
class ReadLease {
public:
    virtual ~ReadLease() = default;
    virtual std::shared_ptr<const RdramImage> image() const noexcept = 0;
    virtual uint64_t epoch() const noexcept = 0;
    virtual bool complete_rdram_image() const noexcept = 0;
    virtual bool stable_until_release() const noexcept = 0;
};

struct GbiEvidence {
    rt64_3ds::Gbi gbi = rt64_3ds::Gbi::F3DEX2;
    bool microcode_identity_verified = false;
    bool task_command_semantics_verified = false;
};

struct SubmitRequest {
    uint32_t ostask_guest_address = 0;
    uint64_t task_sequence = 0;
    ExecutionState execution_state = ExecutionState::unknown;
    GbiEvidence gbi{};
    std::shared_ptr<const ReadLease> rdram;
};

struct FrameSubmission {
    uint64_t task_sequence = 0;
    uint32_t ostask_guest_address = 0;
    GuestCompletionOwner completion_owner = GuestCompletionOwner::runtime;
    std::shared_ptr<const ReadLease> rdram_lifetime;
    std::unique_ptr<rt64_3ds::FrameRecord> frame;
};

struct SubmitResult {
    Status status = Status::invalid_argument;
    std::unique_ptr<FrameSubmission> submission;
};

// Maps either a physical RDRAM address [0, 8 MiB) or KSEG0 [0x80000000,
// 0x80800000) into the 8 MiB RDRAM image. Other segments are rejected.
bool translate_guest_span(uint32_t guest_address, size_t length,
                          size_t rdram_size, size_t* offset) noexcept;

// A deliberately narrow adapter for the authenticated Cruis'n task layout
// identified in the Phase 0 audit. It is disconnected from the guest runtime.
// A caller must prove that the generated RSP has not already run, supply a
// verified microcode/GBI identity, and hold a stable full-RDRAM lease through
// Citro3D/backend consumption. The class does not produce guest completion,
// DPC output, SP/DP status, or interrupts.
class Adapter {
public:
    SubmitResult submit(const SubmitRequest& request);

    uint64_t last_reserved_sequence() const noexcept { return last_sequence_; }
    uint64_t interpreter_invocations() const noexcept { return interpreter_invocations_; }
    uint32_t interpreted_commands() const noexcept;

private:
    uint64_t last_sequence_ = 0;
    uint64_t interpreter_invocations_ = 0;
    const uint8_t* rdram_identity_ = nullptr;
    std::shared_ptr<const RdramImage> rdram_image_;
    std::unique_ptr<rt64_3ds::Interpreter> interpreter_;
};

const char* status_name(Status status) noexcept;

} // namespace cruisn::rt64_bridge

#pragma once

#include "cruisn_rt64_bridge.hpp"

#include <memory>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace cruisn::rt64_bridge {

// Host-side prototype for a task-time full-RDRAM snapshot. Every runtime
// writer must hold the same gate while it accesses mutable guest memory.
// The gate is held only while copying; RT64 then reads the independent image.
class SnapshotLease final : public ReadLease {
public:
    static std::shared_ptr<SnapshotLease> capture(
        std::shared_ptr<const RdramImage> source,
        std::shared_mutex& runtime_rdram_gate,
        uint64_t epoch);

    std::shared_ptr<const RdramImage> image() const noexcept override;
    uint64_t epoch() const noexcept override { return epoch_; }
    bool complete_rdram_image() const noexcept override;
    bool stable_until_release() const noexcept override { return true; }

private:
    class Image final : public RdramImage {
    public:
        explicit Image(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
        const uint8_t* data() const noexcept override { return bytes_.data(); }
        size_t size() const noexcept override { return bytes_.size(); }

    private:
        std::vector<uint8_t> bytes_;
    };

    SnapshotLease(std::shared_ptr<const Image> image, uint64_t epoch)
        : image_(std::move(image)), epoch_(epoch) {}

    std::shared_ptr<const Image> image_;
    uint64_t epoch_ = 0;
};

} // namespace cruisn::rt64_bridge

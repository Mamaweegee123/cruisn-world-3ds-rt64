#include "cruisn_rdram_snapshot.hpp"

#include <mutex>

namespace cruisn::rt64_bridge {

std::shared_ptr<SnapshotLease> SnapshotLease::capture(
    std::shared_ptr<const RdramImage> source,
    std::shared_mutex& runtime_rdram_gate,
    uint64_t epoch) {
    if (!source || epoch == 0) return {};

    std::unique_lock lock(runtime_rdram_gate);
    const uint8_t* const data = source->data();
    if (data == nullptr || source->size() != kRdramBytes) return {};
    std::vector<uint8_t> copy(data, data + kRdramBytes);
    auto image = std::make_shared<const Image>(std::move(copy));
    return std::shared_ptr<SnapshotLease>(
        new SnapshotLease(std::move(image), epoch));
}

std::shared_ptr<const RdramImage> SnapshotLease::image() const noexcept {
    return image_;
}

bool SnapshotLease::complete_rdram_image() const noexcept {
    return image_ && image_->data() != nullptr && image_->size() == kRdramBytes;
}

} // namespace cruisn::rt64_bridge

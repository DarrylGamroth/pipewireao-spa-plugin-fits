/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <optional>

namespace egrabber_pipewire {

struct FrameSequenceResult {
    std::uint64_t sequence = 0;
    std::uint64_t lost = 0;
    bool discontinuity = false;
};

class FrameSequence {
public:
    FrameSequenceResult next(std::optional<std::uint64_t> frame_id);
    void reset() noexcept;

private:
    std::optional<std::uint64_t> last_frame_id_;
    std::uint64_t sequence_ = 0;
    bool have_sequence_ = false;
};

} // namespace egrabber_pipewire

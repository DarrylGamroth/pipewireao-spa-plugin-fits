/* SPDX-License-Identifier: MIT */

#include "frame_sequence.hpp"

#include <limits>

namespace egrabber_pipewire {

namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

} // namespace

FrameSequenceResult FrameSequence::next(std::optional<std::uint64_t> frame_id) {
    if (!have_sequence_) {
        have_sequence_ = true;
        last_frame_id_ = frame_id;
        return {sequence_, 0, false};
    }

    std::uint64_t advance = 1;
    bool discontinuity = false;
    if (frame_id && last_frame_id_) {
        const auto current = *frame_id;
        const auto previous = *last_frame_id_;
        if (current > previous) {
            advance = current - previous;
            discontinuity = advance != 1;
        } else if (previous >= 0xff00 && current > 0 && current <= 0x100) {
            // GigE Vision's 16-bit block ID wraps from 65535 to 1.
            advance = (0xffff - previous) + current;
            discontinuity = advance != 1;
        } else {
            discontinuity = true;
        }
    }

    sequence_ = saturating_add(sequence_, advance);
    last_frame_id_ = frame_id;
    return {sequence_, advance > 1 ? advance - 1 : 0, discontinuity};
}

void FrameSequence::reset() noexcept { last_frame_id_.reset(); }

} // namespace egrabber_pipewire

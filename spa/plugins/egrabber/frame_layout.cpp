/* SPDX-License-Identifier: MIT */

#include "frame_layout.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace egrabber_pipewire {

std::size_t committed_prefix(std::size_t size_filled, std::size_t payload_size,
                             std::size_t granularity) noexcept {
    const auto bounded = std::min(size_filled, payload_size);
    if (bounded == payload_size || granularity == 0) return bounded;
    return bounded - bounded % granularity;
}

ResolvedFrameLayout resolve_frame_layout(
        const NegotiatedFrameLayout &negotiated,
        const DeliveredFrameLayout &delivered) {
    if (delivered.width != negotiated.width ||
        delivered.delivered_height > negotiated.height ||
        delivered.pixel_format != negotiated.pixel_format ||
        (delivered.offset_x && *delivered.offset_x != negotiated.offset_x) ||
        (delivered.offset_y && *delivered.offset_y != negotiated.offset_y))
        throw std::runtime_error("delivered image layout differs from the negotiated layout");

    ResolvedFrameLayout result;
    result.line_pitch = delivered.line_pitch
        ? delivered.line_pitch : negotiated.natural_line_pitch;
    if (delivered.x_padding &&
        (*delivered.x_padding > std::numeric_limits<std::size_t>::max() -
                negotiated.natural_line_pitch ||
         negotiated.natural_line_pitch + *delivered.x_padding != result.line_pitch))
        throw std::runtime_error("delivered line pitch and X padding disagree");
    if (delivered.delivered_height != 0 &&
        result.line_pitch > std::numeric_limits<std::size_t>::max() /
            delivered.delivered_height)
        throw std::runtime_error("delivered image size overflows size_t");

    result.image_offset = delivered.image_offset.value_or(0);
    if (result.image_offset > negotiated.buffer_capacity)
        throw std::runtime_error("image offset exceeds the announced buffer");
    const auto image_size = result.line_pitch * delivered.delivered_height;
    result.data_size = std::min(
        image_size, negotiated.buffer_capacity - result.image_offset);
    result.incomplete = delivered.transport_incomplete ||
                        delivered.delivered_height != negotiated.height;
    result.corrupted = result.incomplete || result.data_size != image_size;

    if (delivered.image_present && !*delivered.image_present) {
        result.corrupted = true;
        result.data_size = 0;
    }
    if (!delivered.payload_type_supported) {
        result.corrupted = true;
        result.data_size = 0;
    }
    if (delivered.data_larger_than_buffer && *delivered.data_larger_than_buffer)
        result.corrupted = true;
    if (delivered.data_size && result.image_offset + result.data_size > *delivered.data_size) {
        result.corrupted = true;
        result.data_size = *delivered.data_size > result.image_offset
            ? std::min(result.data_size, *delivered.data_size - result.image_offset) : 0;
    }
    if (delivered.size_filled && result.image_offset + result.data_size > *delivered.size_filled) {
        result.corrupted = true;
        result.data_size = *delivered.size_filled > result.image_offset
            ? std::min(result.data_size, *delivered.size_filled - result.image_offset) : 0;
    }
    return result;
}

} // namespace egrabber_pipewire

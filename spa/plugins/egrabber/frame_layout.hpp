/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace egrabber_pipewire {

struct NegotiatedFrameLayout {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t offset_x = 0;
    std::size_t offset_y = 0;
    std::size_t natural_line_pitch = 0;
    std::size_t buffer_capacity = 0;
    std::string pixel_format;
};

struct DeliveredFrameLayout {
    std::size_t width = 0;
    std::size_t delivered_height = 0;
    std::size_t line_pitch = 0;
    std::string pixel_format;
    std::optional<std::size_t> offset_x;
    std::optional<std::size_t> offset_y;
    std::optional<std::size_t> image_offset;
    std::optional<std::size_t> size_filled;
    std::optional<std::size_t> data_size;
    std::optional<std::size_t> x_padding;
    std::optional<bool> image_present;
    std::optional<bool> data_larger_than_buffer;
    bool payload_type_supported = true;
    bool transport_incomplete = false;
};

struct ResolvedFrameLayout {
    std::size_t image_offset = 0;
    std::size_t data_size = 0;
    std::size_t line_pitch = 0;
    bool incomplete = false;
    bool corrupted = false;
};

ResolvedFrameLayout resolve_frame_layout(
    const NegotiatedFrameLayout &negotiated,
    const DeliveredFrameLayout &delivered);

std::size_t committed_prefix(std::size_t size_filled, std::size_t payload_size,
                             std::size_t granularity) noexcept;

} // namespace egrabber_pipewire

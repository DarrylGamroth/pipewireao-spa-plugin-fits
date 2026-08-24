/* SPDX-License-Identifier: MIT */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spa/utils/dict.h>

namespace egrabber_pipewire {

enum class ProgressivePolicy { disabled, offer, require };

struct Options {
    std::string producer = "gigelink";
    std::optional<std::string> serial;
    std::optional<std::string> user_id;
    std::string control = "auto";
    std::vector<std::string> clprotocol_libraries;
    std::optional<std::string> clprotocol_device_template;
    std::optional<std::string> camera_serial;
    std::optional<std::string> genapi_runtime;
    std::uint32_t control_timeout_ms = 1000;
    ProgressivePolicy progressive = ProgressivePolicy::disabled;
    std::optional<std::array<std::uint8_t, 16>> acquisition_domain;
    std::uint64_t acquisition_generation = 0;
    std::uint32_t acquisition_sequence_context = 0;
    int interface_index = 0;
    int device_index = 0;
    int stream_index = 0;
    std::size_t buffer_count = 8;
};

void read_options(Options &options, const struct spa_dict *info);
const char *progressive_policy_name(ProgressivePolicy policy) noexcept;
std::string format_acquisition_domain(
		const std::array<std::uint8_t, 16> &domain);

} // namespace egrabber_pipewire

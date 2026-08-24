/* SPDX-License-Identifier: MIT */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace egrabber_pipewire {

enum class FeatureKind { boolean, integer, floating, enumeration, string, command, unsupported };

struct Feature {
    std::string name;
    std::string property_name;
    std::string description;
    FeatureKind kind = FeatureKind::unsupported;
    bool readable = false;
    bool writeable = false;
    std::vector<std::string> enum_entries;
};

using FeatureValue = std::variant<bool, std::int64_t, double,
		std::int32_t, std::string>;

bool changes_payload_layout(const Feature &feature);
bool is_scalar_feature(const Feature &feature) noexcept;

} // namespace egrabber_pipewire

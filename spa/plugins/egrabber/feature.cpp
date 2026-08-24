/* SPDX-License-Identifier: MIT */

#include "feature.hpp"

#include <algorithm>
#include <string_view>

namespace egrabber_pipewire {

bool changes_payload_layout(const Feature &feature) {
    const auto &name = feature.name;
    static constexpr std::string_view exact[] = {
        "PixelFormat", "Width", "Height", "OffsetX", "OffsetY", "PayloadSize",
        "ChunkModeActive",
    };
    if (std::find(std::begin(exact), std::end(exact), name) != std::end(exact)) return true;
    static constexpr std::string_view fragments[] = {
        "Binning", "Decimation", "Resolution", "Region", "ComponentEnable",
        "ChunkEnable",
    };
    return std::any_of(std::begin(fragments), std::end(fragments), [&](std::string_view fragment) {
        return name.find(fragment) != std::string::npos;
    });
}

bool is_scalar_feature(const Feature &feature) noexcept
{
	return feature.kind == FeatureKind::boolean ||
		feature.kind == FeatureKind::integer ||
		feature.kind == FeatureKind::floating ||
		feature.kind == FeatureKind::enumeration ||
		feature.kind == FeatureKind::string;
}

} // namespace egrabber_pipewire

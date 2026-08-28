/* SPDX-License-Identifier: MIT */

#include "feature.hpp"

#include "feature.h"

namespace egrabber_pipewire {

bool changes_payload_layout(const Feature &feature) {
	return pwao_genicam_feature_changes_payload_layout(feature.name.c_str());
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

/* SPDX-License-Identifier: MIT */

#pragma once

#include "camera.hpp"

#include <cstdint>

struct spa_pod;
struct spa_pod_builder;

namespace egrabber_pipewire {

const Feature *scalar_feature_at(const Camera &camera, std::uint32_t index);
spa_pod *build_feature_prop_info(Camera &camera, const Feature &feature,
		spa_pod_builder *builder);
spa_pod *build_feature_props(Camera &camera, spa_pod_builder *builder);

} // namespace egrabber_pipewire

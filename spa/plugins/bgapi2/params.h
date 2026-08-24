/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_PARAMS_H
#define SPA_BGAPI2_PARAMS_H

#include <stdint.h>

#include <spa/pod/builder.h>

struct bgapi2_camera;

struct spa_pod *bgapi2_build_feature_prop_info(struct bgapi2_camera *camera,
		uint32_t scalar_index, struct spa_pod_builder *builder);
struct spa_pod *bgapi2_build_feature_props(struct bgapi2_camera *camera,
		struct spa_pod_builder *builder);

#endif

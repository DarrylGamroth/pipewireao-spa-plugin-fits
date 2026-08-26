/* SPDX-License-Identifier: MIT */
#ifndef SPA_ANDOR3_PARAMS_H
#define SPA_ANDOR3_PARAMS_H

#include <stdint.h>

#include <spa/pod/builder.h>

struct andor3_camera;

struct spa_pod *andor3_build_feature_prop_info(struct andor3_camera *camera,
		uint32_t feature_index, struct spa_pod_builder *builder);
struct spa_pod *andor3_build_feature_props(struct andor3_camera *camera,
		struct spa_pod_builder *builder);

#endif

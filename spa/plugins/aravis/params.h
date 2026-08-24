/* SPDX-License-Identifier: MIT */
#ifndef SPA_ARAVIS_PARAMS_H
#define SPA_ARAVIS_PARAMS_H

#include <stdint.h>

#include <spa/pod/builder.h>

struct aravis_camera;

struct spa_pod *aravis_build_feature_prop_info(struct aravis_camera *camera,
		uint32_t scalar_index, struct spa_pod_builder *builder);
struct spa_pod *aravis_build_feature_props(struct aravis_camera *camera,
		struct spa_pod_builder *builder);

#endif

/* SPDX-License-Identifier: MIT */
#ifndef SPA_HAMAMATSU_PARAMS_H
#define SPA_HAMAMATSU_PARAMS_H

#include <stdint.h>

#include <spa/pod/builder.h>

struct hamamatsu_camera;

struct spa_pod *hamamatsu_build_feature_prop_info(struct hamamatsu_camera *camera,
		uint32_t feature_index, struct spa_pod_builder *builder);
struct spa_pod *hamamatsu_build_feature_props(struct hamamatsu_camera *camera,
		struct spa_pod_builder *builder);

#endif

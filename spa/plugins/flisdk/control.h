/* SPDX-License-Identifier: MIT */
#ifndef SPA_FLISDK_CONTROL_H
#define SPA_FLISDK_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

struct spa_pod;
struct spa_pod_builder;
struct flisdk_camera;
struct flisdk_control;

#ifdef __cplusplus
extern "C" {
#endif

struct flisdk_control_options {
	const char *libraries;
	const char *device_template;
	const char *camera_serial;
	const char *genapi_runtime;
	uint32_t timeout_ms;
};

int flisdk_control_open(struct flisdk_control **control,
		struct flisdk_camera *camera,
		const struct flisdk_control_options *options);
void flisdk_control_close(struct flisdk_control *control);

int flisdk_control_build_prop_info(struct flisdk_control *control,
		uint32_t index, struct spa_pod_builder *builder,
		struct spa_pod **param);
int flisdk_control_build_props(struct flisdk_control *control,
		struct spa_pod_builder *builder, struct spa_pod **param);
int flisdk_control_set(struct flisdk_control *control, const char *name,
		const struct spa_pod *value, bool allow_layout_change,
		bool *layout_changed);

#ifdef __cplusplus
}
#endif

#endif

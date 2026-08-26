/* SPDX-License-Identifier: MIT */
#ifndef SPA_FLISDK_CAMERA_H
#define SPA_FLISDK_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define FLISDK_CAMERA_MAX_BUFFERS 64u

#ifdef __cplusplus
extern "C" {
#endif

struct flisdk_camera_options {
	const char *camera_name;
	const char *grabber_name;
};

struct flisdk_camera_info {
	uint64_t payload_size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t depth;
	bool unsigned_pixels;
	char model[128];
	char camera_name[256];
};

struct flisdk_camera_buffer;

struct flisdk_camera_completion {
	struct flisdk_camera_buffer *buffer;
	void *user_data;
	uint64_t frame_id;
	uint64_t size_filled;
	bool incomplete;
};

struct flisdk_camera;

int flisdk_camera_open(struct flisdk_camera **camera,
		const struct flisdk_camera_options *options);
void flisdk_camera_close(struct flisdk_camera *camera);
const struct flisdk_camera_info *flisdk_camera_get_info(
		const struct flisdk_camera *camera);

int flisdk_camera_announce(struct flisdk_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct flisdk_camera_buffer **buffer);
int flisdk_camera_revoke(struct flisdk_camera *camera,
		struct flisdk_camera_buffer **buffer);
int flisdk_camera_queue(struct flisdk_camera *camera,
		struct flisdk_camera_buffer *buffer);

int flisdk_camera_start(struct flisdk_camera *camera);
int flisdk_camera_stop(struct flisdk_camera *camera);
int flisdk_camera_try_get_completion(struct flisdk_camera *camera,
		struct flisdk_camera_completion *completion);

int flisdk_camera_send_command(struct flisdk_camera *camera,
		const char *command, char *response, uint64_t response_size);
int flisdk_camera_apply_layout(struct flisdk_camera *camera,
		uint32_t width, uint32_t height);
int flisdk_camera_refresh_info(struct flisdk_camera *camera);

#ifdef __cplusplus
}
#endif

#endif

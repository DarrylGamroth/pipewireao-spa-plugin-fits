/* SPDX-License-Identifier: MIT */
#ifndef SPA_EDTPDV_CAMERA_H
#define SPA_EDTPDV_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define EDTPDV_CAMERA_MAX_BUFFERS 64u

struct edtpdv_camera_options {
	const char *device;
	uint32_t unit;
	uint32_t channel;
	uint32_t ring_buffers;
};

struct edtpdv_camera_info {
	uint64_t payload_size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t depth;
	char model[128];
};

struct edtpdv_camera_buffer;

struct edtpdv_camera_completion {
	struct edtpdv_camera_buffer *buffer;
	void *user_data;
	uint64_t frame_id;
	uint64_t size_filled;
	bool incomplete;
};

struct edtpdv_camera;

int edtpdv_camera_open(struct edtpdv_camera **camera,
		const struct edtpdv_camera_options *options);
void edtpdv_camera_close(struct edtpdv_camera *camera);
const struct edtpdv_camera_info *edtpdv_camera_get_info(
		const struct edtpdv_camera *camera);

int edtpdv_camera_announce(struct edtpdv_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct edtpdv_camera_buffer **buffer);
int edtpdv_camera_revoke(struct edtpdv_camera *camera,
		struct edtpdv_camera_buffer **buffer);
int edtpdv_camera_queue(struct edtpdv_camera *camera,
		struct edtpdv_camera_buffer *buffer);

int edtpdv_camera_start(struct edtpdv_camera *camera);
int edtpdv_camera_stop(struct edtpdv_camera *camera);
int edtpdv_camera_try_get_completion(struct edtpdv_camera *camera,
		struct edtpdv_camera_completion *completion);

#endif

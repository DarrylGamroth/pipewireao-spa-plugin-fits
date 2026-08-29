/* SPDX-License-Identifier: MIT */
#ifndef SPA_HERMES_CAMERA_H
#define SPA_HERMES_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define HERMES_CAMERA_MAX_BUFFERS 64u
#define HERMES_FRONT_PANEL_BLOCK_SIZE 1024u

struct hermes_camera_options {
	const char *device_id;
	uint32_t exposure_clocks;
	uint32_t integrated_frames;
	uint32_t counters;
	uint32_t frames_per_buffer;
	uint32_t bits_per_pixel;
	bool advanced_mode;
	bool force_8bit;
	bool half_array;
	bool signed_data;
};

struct hermes_camera_info {
	uint64_t payload_size;
	uint32_t width;
	uint32_t height;
	uint32_t counters;
	uint32_t frames_per_buffer;
	uint32_t bits_per_pixel;
	uint32_t raw_plane_bytes;
	char model[64];
	char serial[64];
	char profile[64];
};

struct hermes_camera_buffer;
struct hermes_camera;

struct hermes_camera_completion {
	struct hermes_camera_buffer *buffer;
	void *user_data;
	uint64_t batch_id;
	uint64_t size_filled;
	bool incomplete;
};

int hermes_camera_open(struct hermes_camera **camera,
		const struct hermes_camera_options *options);
void hermes_camera_close(struct hermes_camera *camera);
const struct hermes_camera_info *hermes_camera_get_info(
		const struct hermes_camera *camera);

int hermes_camera_announce(struct hermes_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct hermes_camera_buffer **buffer);
int hermes_camera_revoke(struct hermes_camera *camera,
		struct hermes_camera_buffer **buffer);
int hermes_camera_queue(struct hermes_camera *camera,
		struct hermes_camera_buffer *buffer);

int hermes_camera_start(struct hermes_camera *camera);
int hermes_camera_stop(struct hermes_camera *camera);
int hermes_camera_get_completion(struct hermes_camera *camera,
		struct hermes_camera_completion *completion);

#endif

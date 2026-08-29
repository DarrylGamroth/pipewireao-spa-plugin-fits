/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hermes.h"

struct hermes_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct hermes_camera {
	struct hermes_camera_info info;
	struct hermes_camera_buffer *buffers[HERMES_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint64_t batch_id;
	bool started;
};

static const char *profile_for(const struct hermes_camera_options *options)
{
	if (options->half_array)
		return options->bits_per_pixel == 8 ? SPA_HERMES_HALF_U8_PROFILE :
				SPA_HERMES_HALF_U16_PROFILE;
	return options->bits_per_pixel == 8 ? SPA_HERMES_FULL_U8_PROFILE :
			SPA_HERMES_FULL_U16_PROFILE;
}

int hermes_camera_open(struct hermes_camera **camera_ptr,
		const struct hermes_camera_options *options)
{
	struct hermes_camera *camera;
	uint64_t payload_size;
	uint32_t pixels;

	if (camera_ptr == NULL || options == NULL || options->device_id == NULL ||
			strcmp(options->device_id, "mock") != 0 ||
			options->counters < 1 || options->counters > 3 ||
			(options->bits_per_pixel != 8 && options->bits_per_pixel != 16) ||
			options->frames_per_buffer == 0)
		return -ENODEV;
	pixels = options->half_array ? 32u * 32u : 64u * 32u;
	payload_size = (uint64_t)pixels * (options->bits_per_pixel / 8u) *
			options->counters * options->frames_per_buffer;
	if (payload_size == 0 || payload_size > UINT32_MAX ||
			payload_size % HERMES_FRONT_PANEL_BLOCK_SIZE != 0)
		return -EINVAL;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	camera->info = (struct hermes_camera_info) {
		.payload_size = payload_size,
		.width = options->half_array ? 32u : 64u,
		.height = 32u,
		.counters = options->counters,
		.frames_per_buffer = options->frames_per_buffer,
		.bits_per_pixel = options->bits_per_pixel,
		.raw_plane_bytes = pixels * (options->bits_per_pixel / 8u),
	};
	memcpy(camera->info.model, "Synthetic MPD HERMES", 21);
	memcpy(camera->info.serial, "mock", 5);
	(void)snprintf(camera->info.profile, sizeof(camera->info.profile), "%s",
			profile_for(options));
	*camera_ptr = camera;
	return 0;
}

void hermes_camera_close(struct hermes_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	free(camera);
}

const struct hermes_camera_info *hermes_camera_get_info(
		const struct hermes_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int hermes_camera_announce(struct hermes_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct hermes_camera_buffer **buffer_ptr)
{
	struct hermes_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			size < camera->info.payload_size ||
			camera->n_buffers >= HERMES_CAMERA_MAX_BUFFERS)
		return -EINVAL;
	buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return -errno;
	buffer->memory = memory;
	buffer->size = size;
	buffer->user_data = user_data;
	camera->buffers[camera->n_buffers++] = buffer;
	*buffer_ptr = buffer;
	return 0;
}

int hermes_camera_revoke(struct hermes_camera *camera,
		struct hermes_camera_buffer **buffer_ptr)
{
	uint32_t i;

	if (camera == NULL || buffer_ptr == NULL || *buffer_ptr == NULL ||
			camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != *buffer_ptr)
			continue;
		free(*buffer_ptr);
		*buffer_ptr = NULL;
		memmove(&camera->buffers[i], &camera->buffers[i + 1],
				(camera->n_buffers - i - 1u) * sizeof(camera->buffers[0]));
		camera->n_buffers--;
		return 0;
	}
	return -ENOENT;
}

int hermes_camera_queue(struct hermes_camera *camera,
		struct hermes_camera_buffer *buffer)
{
	uint32_t i;

	if (camera == NULL || buffer == NULL || buffer->queued)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] == buffer) {
			buffer->queued = true;
			return 0;
		}
	}
	return -ENOENT;
}

int hermes_camera_start(struct hermes_camera *camera)
{
	if (camera == NULL || camera->n_buffers < 2)
		return -EINVAL;
	camera->batch_id = 0;
	camera->started = true;
	return 0;
}

int hermes_camera_stop(struct hermes_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	camera->started = false;
	return 0;
}

int hermes_camera_get_completion(struct hermes_camera *camera,
		struct hermes_camera_completion *completion)
{
	uint32_t i;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct hermes_camera_buffer *buffer = camera->buffers[index];
		uint8_t *bytes;
		uint64_t j;

		if (!buffer->queued)
			continue;
		buffer->queued = false;
		camera->scan_hint = (index + 1u) % camera->n_buffers;
		camera->batch_id++;
		bytes = buffer->memory;
		for (j = 0; j < camera->info.payload_size; j++)
			bytes[j] = (uint8_t)(camera->batch_id + j);
		*completion = (struct hermes_camera_completion) {
			.buffer = buffer,
			.user_data = buffer->user_data,
			.batch_id = camera->batch_id,
			.size_filled = camera->info.payload_size,
		};
		return 1;
	}
	return 0;
}

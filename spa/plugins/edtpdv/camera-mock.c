/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct edtpdv_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct edtpdv_camera {
	struct edtpdv_camera_info info;
	struct edtpdv_camera_buffer *buffers[EDTPDV_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint64_t frame_id;
	bool started;
};

int edtpdv_camera_open(struct edtpdv_camera **camera_ptr,
		const struct edtpdv_camera_options *options)
{
	struct edtpdv_camera *camera;

	if (camera_ptr == NULL || options == NULL || options->device == NULL ||
			strcmp(options->device, "mock") != 0 || options->unit != 0 ||
			options->channel != 0 || options->ring_buffers < 2 ||
			options->ring_buffers > EDTPDV_CAMERA_MAX_BUFFERS)
		return -ENODEV;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	camera->info.payload_size = 12;
	camera->info.width = 4;
	camera->info.height = 3;
	camera->info.pitch = 4;
	camera->info.depth = 8;
	memcpy(camera->info.model, "Synthetic EDT PDV camera", 25);
	*camera_ptr = camera;
	return 0;
}

void edtpdv_camera_close(struct edtpdv_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	free(camera);
}

const struct edtpdv_camera_info *edtpdv_camera_get_info(
		const struct edtpdv_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int edtpdv_camera_announce(struct edtpdv_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct edtpdv_camera_buffer **buffer_ptr)
{
	struct edtpdv_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			size < camera->info.payload_size ||
			camera->n_buffers >= EDTPDV_CAMERA_MAX_BUFFERS)
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

int edtpdv_camera_revoke(struct edtpdv_camera *camera,
		struct edtpdv_camera_buffer **buffer_ptr)
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

int edtpdv_camera_queue(struct edtpdv_camera *camera,
		struct edtpdv_camera_buffer *buffer)
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

int edtpdv_camera_start(struct edtpdv_camera *camera)
{
	if (camera == NULL || camera->n_buffers < 2)
		return -EINVAL;
	camera->frame_id = 0;
	camera->started = true;
	return 0;
}

int edtpdv_camera_stop(struct edtpdv_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	camera->started = false;
	return 0;
}

int edtpdv_camera_try_get_completion(struct edtpdv_camera *camera,
		struct edtpdv_camera_completion *completion)
{
	uint32_t i;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct edtpdv_camera_buffer *buffer = camera->buffers[index];
		uint8_t *bytes;
		uint32_t j;

		if (!buffer->queued)
			continue;
		buffer->queued = false;
		camera->scan_hint = (index + 1u) % camera->n_buffers;
		camera->frame_id++;
		bytes = buffer->memory;
		for (j = 0; j < camera->info.payload_size; j++)
			bytes[j] = (uint8_t)(camera->frame_id + j);
		*completion = (struct edtpdv_camera_completion) {
			.buffer = buffer,
			.user_data = buffer->user_data,
			.frame_id = camera->frame_id,
			.size_filled = camera->info.payload_size,
		};
		return 1;
	}
	return 0;
}

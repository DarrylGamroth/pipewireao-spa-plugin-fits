/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct flisdk_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct flisdk_camera {
	struct flisdk_camera_info info;
	struct flisdk_camera_buffer *buffers[FLISDK_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint64_t frame_id;
	bool started;
	double fps;
	uint32_t offset_x;
	bool crop_enabled;
	bool unsigned_pixels;
};

int flisdk_camera_open(struct flisdk_camera **camera_ptr,
		const struct flisdk_camera_options *options)
{
	struct flisdk_camera *camera;

	if (camera_ptr == NULL || options == NULL || options->camera_name == NULL ||
			strcmp(options->camera_name, "mock") != 0 ||
			(options->grabber_name != NULL &&
			 strcmp(options->grabber_name, "mock") != 0))
		return -ENODEV;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	camera->info.payload_size = 12;
	camera->info.width = 4;
	camera->info.height = 3;
	camera->info.pitch = 4;
	camera->info.depth = 8;
	camera->info.unsigned_pixels = true;
	camera->unsigned_pixels = true;
	camera->fps = 120.0;
	camera->crop_enabled = true;
	camera->offset_x = 64;
	memcpy(camera->info.model, "Synthetic FliSdk camera", 24);
	memcpy(camera->info.camera_name, "mock", 5);
	*camera_ptr = camera;
	return 0;
}

void flisdk_camera_close(struct flisdk_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	free(camera);
}

const struct flisdk_camera_info *flisdk_camera_get_info(
		const struct flisdk_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int flisdk_camera_announce(struct flisdk_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct flisdk_camera_buffer **buffer_ptr)
{
	struct flisdk_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			size < camera->info.payload_size ||
			camera->n_buffers >= FLISDK_CAMERA_MAX_BUFFERS)
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

int flisdk_camera_revoke(struct flisdk_camera *camera,
		struct flisdk_camera_buffer **buffer_ptr)
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

int flisdk_camera_queue(struct flisdk_camera *camera,
		struct flisdk_camera_buffer *buffer)
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

int flisdk_camera_start(struct flisdk_camera *camera)
{
	if (camera == NULL || camera->n_buffers < 2)
		return -EINVAL;
	camera->frame_id = 0;
	camera->started = true;
	return 0;
}

int flisdk_camera_stop(struct flisdk_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	camera->started = false;
	return 0;
}

int flisdk_camera_try_get_completion(struct flisdk_camera *camera,
		struct flisdk_camera_completion *completion)
{
	uint32_t i;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct flisdk_camera_buffer *buffer = camera->buffers[index];
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
		*completion = (struct flisdk_camera_completion) {
			.buffer = buffer,
			.user_data = buffer->user_data,
			.frame_id = camera->frame_id,
			.size_filled = camera->info.payload_size,
		};
		return 1;
	}
	return 0;
}

int flisdk_camera_send_command(struct flisdk_camera *camera,
		const char *command, char *response, uint64_t response_size)
{
	unsigned int first, last;
	int written;

	if (camera == NULL || command == NULL || response == NULL || response_size == 0)
		return -EINVAL;
	if (strcmp(command, "cameratype raw") == 0)
		written = snprintf(response, response_size, "C-RED 2");
	else if (strcmp(command, "hwuid raw") == 0)
		written = snprintf(response, response_size, "CRED2-MOCK-001");
	else if (strcmp(command, "version firmware raw") == 0)
		written = snprintf(response, response_size, "3.0");
	else if (strcmp(command, "fps raw") == 0)
		written = snprintf(response, response_size, "%.12g", camera->fps);
	else if (strcmp(command, "minfps raw") == 0)
		written = snprintf(response, response_size, "0.001");
	else if (strcmp(command, "maxfps raw") == 0)
		written = snprintf(response, response_size, "600");
	else if (strcmp(command, "tint raw") == 0)
		written = snprintf(response, response_size, "0.00125");
	else if (strcmp(command, "mintint raw") == 0)
		written = snprintf(response, response_size, "0.00005");
	else if (strcmp(command, "maxtint raw") == 0 ||
			strcmp(command, "maxtintitr raw") == 0)
		written = snprintf(response, response_size, "1.0");
	else if (strcmp(command, "cropping raw") == 0)
		written = snprintf(response, response_size, "%s",
				camera->crop_enabled ? "on" : "off");
	else if (strcmp(command, "cropping columns raw") == 0)
		written = snprintf(response, response_size, "%u %u", camera->offset_x,
				camera->offset_x + camera->info.width - 1u);
	else if (strcmp(command, "cropping rows raw") == 0)
		written = snprintf(response, response_size, "0 %u",
				camera->info.height - 1u);
	else if (strcmp(command, "unsigned raw") == 0)
		written = snprintf(response, response_size, "%s",
				camera->unsigned_pixels ? "on" : "off");
	else if (sscanf(command, "set cropping columns %u %u", &first, &last) == 2 &&
			last >= first) {
		camera->offset_x = first;
		camera->info.width = last - first + 1u;
		written = snprintf(response, response_size, "Result: OK");
	} else if (sscanf(command, "set cropping rows %u %u", &first, &last) == 2 &&
			last >= first) {
		camera->info.height = last - first + 1u;
		written = snprintf(response, response_size, "Result: OK");
	} else if (sscanf(command, "set fps %lf", &camera->fps) == 1)
		written = snprintf(response, response_size, "Result: OK");
	else if (strncmp(command, "set unsigned ", 13) == 0) {
		camera->unsigned_pixels = strcmp(command + 13, "on") == 0;
		camera->info.unsigned_pixels = camera->unsigned_pixels;
		written = snprintf(response, response_size, "Result: OK");
	} else if (strncmp(command, "set ", 4) == 0 ||
			strcmp(command, "swtrig") == 0 || strcmp(command, "continue") == 0 ||
			strcmp(command, "save") == 0)
		written = snprintf(response, response_size, "Result: OK");
	else if (strlen(command) >= 4 &&
			strcmp(command + strlen(command) - 4u, " raw") == 0)
		written = snprintf(response, response_size, "0");
	else
		written = snprintf(response, response_size, "Result: OK");
	return written < 0 || (uint64_t)written >= response_size ? -ENOSPC : 0;
}

int flisdk_camera_apply_layout(struct flisdk_camera *camera,
		uint32_t width, uint32_t height)
{
	if (camera == NULL || width == 0 || height == 0 || camera->started ||
			camera->n_buffers != 0)
		return -EINVAL;
	camera->info.width = width;
	camera->info.height = height;
	camera->info.pitch = width * (camera->info.depth / 8u);
	camera->info.payload_size = (uint64_t)camera->info.pitch * height;
	return 0;
}

int flisdk_camera_refresh_info(struct flisdk_camera *camera)
{
	return camera == NULL ? -EINVAL : 0;
}

/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libedt.h>
#include <libpdv.h>

struct edtpdv_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct edtpdv_camera {
	PdvDev device;
	struct edtpdv_camera_info info;
	struct edtpdv_camera_buffer *buffers[EDTPDV_CAMERA_MAX_BUFFERS];
	uint32_t ring_buffers;
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint32_t last_done;
	int last_timeouts;
	uint64_t frame_id;
	bool started;
};

static int copy_text(char *destination, size_t capacity, const char *source)
{
	int written;

	if (source == NULL || source[0] == '\0')
		source = "EDT PDV camera";
	written = snprintf(destination, capacity, "%s", source);
	return written >= 0 && (size_t)written < capacity ? 0 : -ENAMETOOLONG;
}

int edtpdv_camera_open(struct edtpdv_camera **camera_ptr,
		const struct edtpdv_camera_options *options)
{
	struct edtpdv_camera *camera;
	int width, height, pitch, depth, image_size;

	if (camera_ptr == NULL || options == NULL || options->device == NULL ||
			options->device[0] == '\0' || options->unit > INT_MAX ||
			options->channel > INT_MAX || options->ring_buffers < 2 ||
			options->ring_buffers > EDTPDV_CAMERA_MAX_BUFFERS)
		return -EINVAL;
	*camera_ptr = NULL;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	camera->device = pdv_open_device(options->device, (int)options->unit,
			(int)options->channel, 0);
	if (camera->device == NULL) {
		free(camera);
		return -ENODEV;
	}
	width = pdv_get_width(camera->device);
	height = pdv_get_height(camera->device);
	pitch = pdv_get_pitch(camera->device);
	depth = pdv_get_depth(camera->device);
	image_size = pdv_get_image_size(camera->device);
	if (width <= 0 || height <= 0 || pitch <= 0 || depth <= 0 ||
			image_size <= 0 || (uint64_t)pitch * (uint64_t)height !=
			(uint64_t)image_size || copy_text(camera->info.model,
				sizeof(camera->info.model),
				pdv_get_camera_type(camera->device)) < 0) {
		pdv_close(camera->device);
		free(camera);
		return -ENOTSUP;
	}
	camera->info.width = (uint32_t)width;
	camera->info.height = (uint32_t)height;
	camera->info.pitch = (uint32_t)pitch;
	camera->info.depth = (uint32_t)depth;
	camera->info.payload_size = (uint64_t)image_size;
	camera->ring_buffers = options->ring_buffers;
	*camera_ptr = camera;
	return 0;
}

void edtpdv_camera_close(struct edtpdv_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	(void)edtpdv_camera_stop(camera);
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	if (camera->device != NULL)
		pdv_close(camera->device);
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
	struct edtpdv_camera_buffer *buffer;
	uint32_t i;

	if (camera == NULL || buffer_ptr == NULL ||
			(buffer = *buffer_ptr) == NULL || camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != buffer)
			continue;
		memmove(&camera->buffers[i], &camera->buffers[i + 1],
				(camera->n_buffers - i - 1u) * sizeof(camera->buffers[0]));
		camera->n_buffers--;
		free(buffer);
		*buffer_ptr = NULL;
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
	int timeouts;

	if (camera == NULL || camera->device == NULL || camera->n_buffers < 2)
		return -EINVAL;
	if (camera->started)
		return 0;
	pdv_flush_fifo(camera->device);
	if (pdv_multibuf(camera->device, (int)camera->ring_buffers) != 0)
		return -EIO;
	camera->last_done = edt_done_count(camera->device);
	timeouts = pdv_timeouts(camera->device);
	if (timeouts < 0)
		return -EIO;
	camera->last_timeouts = timeouts;
	camera->frame_id = 0;
	pdv_start_images(camera->device, 0);
	camera->started = true;
	return 0;
}

int edtpdv_camera_stop(struct edtpdv_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return -EINVAL;
	if (!camera->started)
		return 0;
	(void)edt_stop_buffers(camera->device);
	pdv_stop_continuous(camera->device);
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	camera->started = false;
	return 0;
}

static struct edtpdv_camera_buffer *take_queued_buffer(
		struct edtpdv_camera *camera)
{
	uint32_t i;

	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct edtpdv_camera_buffer *buffer = camera->buffers[index];

		if (!buffer->queued)
			continue;
		buffer->queued = false;
		camera->scan_hint = (index + 1u) % camera->n_buffers;
		return buffer;
	}
	return NULL;
}

int edtpdv_camera_try_get_completion(struct edtpdv_camera *camera,
		struct edtpdv_camera_completion *completion)
{
	struct edtpdv_camera_buffer *buffer;
	uint8_t *image;
	uint32_t done, delta;
	int timeouts;
	bool incomplete, recover;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	done = edt_done_count(camera->device);
	delta = done - camera->last_done;
	if (delta == 0)
		return 0;
	camera->last_done = done;
	camera->frame_id += delta;
	timeouts = pdv_timeouts(camera->device);
	if (timeouts < 0)
		return -EIO;
	incomplete = timeouts != camera->last_timeouts ||
			pdv_overrun(camera->device) != 0;
	recover = timeouts != camera->last_timeouts;
	camera->last_timeouts = timeouts;
	buffer = take_queued_buffer(camera);
	if (buffer == NULL) {
		if (recover)
			(void)pdv_timeout_restart(camera->device, 1);
		return 0;
	}
	image = pdv_get_last_image_raw(camera->device);
	if (image == NULL) {
		buffer->queued = true;
		if (recover)
			(void)pdv_timeout_restart(camera->device, 1);
		return -EIO;
	}
	memcpy(buffer->memory, image, (size_t)camera->info.payload_size);
	if (recover)
		(void)pdv_timeout_restart(camera->device, 1);
	*completion = (struct edtpdv_camera_completion) {
		.buffer = buffer,
		.user_data = buffer->user_data,
		.frame_id = camera->frame_id,
		.size_filled = camera->info.payload_size,
		.incomplete = incomplete,
	};
	return 1;
}

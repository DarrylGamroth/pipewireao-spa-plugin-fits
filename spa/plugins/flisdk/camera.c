/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FliSdk_C_V2.h>
#include <FliSerialCamera_C.h>

enum buffer_state {
	BUFFER_IDLE,
	BUFFER_QUEUED,
	BUFFER_FILLING,
	BUFFER_COMPLETE,
};

struct flisdk_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	uint64_t frame_id;
	enum buffer_state state;
};

struct flisdk_camera {
	FliContext context;
	callbackHandler callback;
	struct flisdk_camera_info info;
	struct flisdk_camera_buffer *buffers[FLISDK_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t queue_hint;
	uint64_t frame_id;
	pthread_mutex_t lock;
	pthread_mutex_t sdk_lock;
	pthread_cond_t callbacks_idle;
	uint32_t active_callbacks;
	bool started;
};

static int copy_text(char *destination, size_t capacity, const char *source,
		const char *fallback)
{
	int written;

	if (source == NULL || source[0] == '\0')
		source = fallback;
	written = snprintf(destination, capacity, "%s", source);
	return written >= 0 && (size_t)written < capacity ? 0 : -ENAMETOOLONG;
}

static struct flisdk_camera_buffer *take_queued_buffer(
		struct flisdk_camera *camera)
{
	uint32_t i;

	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->queue_hint + i) % camera->n_buffers;
		struct flisdk_camera_buffer *buffer = camera->buffers[index];

		if (buffer->state != BUFFER_QUEUED)
			continue;
		buffer->state = BUFFER_FILLING;
		camera->queue_hint = (index + 1u) % camera->n_buffers;
		return buffer;
	}
	return NULL;
}

static void image_received(const uint8_t *image, void *data)
{
	struct flisdk_camera *camera = data;
	struct flisdk_camera_buffer *buffer;
	uint64_t frame_id;

	if (pthread_mutex_lock(&camera->lock) != 0)
		return;
	camera->active_callbacks++;
	frame_id = ++camera->frame_id;
	buffer = camera->started && image != NULL ? take_queued_buffer(camera) : NULL;
	if (buffer != NULL)
		buffer->frame_id = frame_id;
	pthread_mutex_unlock(&camera->lock);

	if (buffer != NULL)
		memcpy(buffer->memory, image, (size_t)camera->info.payload_size);

	if (pthread_mutex_lock(&camera->lock) != 0)
		return;
	if (buffer != NULL)
		buffer->state = BUFFER_COMPLETE;
	if (--camera->active_callbacks == 0)
		pthread_cond_broadcast(&camera->callbacks_idle);
	pthread_mutex_unlock(&camera->lock);
}

static void destroy_camera(struct flisdk_camera *camera)
{
	if (camera->context != NULL)
		FliSdk_exit_V2(camera->context);
	pthread_cond_destroy(&camera->callbacks_idle);
	pthread_mutex_destroy(&camera->sdk_lock);
	pthread_mutex_destroy(&camera->lock);
	free(camera);
}

static int refresh_info(struct flisdk_camera *camera)
{
	uint16_t width = 0, height = 0;

	FliSdk_getCurrentImageDimension_V2(camera->context, &width, &height);
	if (width == 0 || height == 0)
		return -ENOTSUP;
	camera->info.width = width;
	camera->info.height = height;
	camera->info.depth = FliSdk_isMono8Pixel_V2(camera->context) ? 8u : 16u;
	camera->info.unsigned_pixels = FliSdk_isUnsignedPixel_V2(camera->context);
	camera->info.pitch = (uint32_t)width * (camera->info.depth / 8u);
	camera->info.payload_size =
		(uint64_t)camera->info.pitch * (uint64_t)height;
	return 0;
}

int flisdk_camera_open(struct flisdk_camera **camera_ptr,
		const struct flisdk_camera_options *options)
{
	struct flisdk_camera *camera;
	char detected[4096];
	int res;

	if (camera_ptr == NULL || options == NULL || options->camera_name == NULL ||
			options->camera_name[0] == '\0')
		return -EINVAL;
	*camera_ptr = NULL;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	if ((res = pthread_mutex_init(&camera->lock, NULL)) != 0) {
		free(camera);
		return -res;
	}
	if ((res = pthread_mutex_init(&camera->sdk_lock, NULL)) != 0) {
		pthread_mutex_destroy(&camera->lock);
		free(camera);
		return -res;
	}
	if ((res = pthread_cond_init(&camera->callbacks_idle, NULL)) != 0) {
		pthread_mutex_destroy(&camera->sdk_lock);
		pthread_mutex_destroy(&camera->lock);
		free(camera);
		return -res;
	}
	camera->context = FliSdk_init_V2();
	if (camera->context == NULL) {
		destroy_camera(camera);
		return -ENODEV;
	}

	detected[0] = '\0';
	FliSdk_detectGrabbers_V2(camera->context, detected, sizeof(detected));
	if (options->grabber_name != NULL && options->grabber_name[0] != '\0' &&
			!FliSdk_setGrabber_V2(camera->context, options->grabber_name)) {
		res = -ENODEV;
		goto error;
	}
	detected[0] = '\0';
	FliSdk_detectCameras_V2(camera->context, detected, sizeof(detected));
	if (!FliSdk_setCamera_V2(camera->context, options->camera_name)) {
		res = -ENODEV;
		goto error;
	}
	FliSdk_setMode_V2(camera->context, Full);
	if (!FliSdk_update_V2(camera->context)) {
		res = -EIO;
		goto error;
	}
	FliSdk_enableRingBuffer_V2(camera->context, false);
	if ((res = refresh_info(camera)) < 0)
		goto error;
	FliSdk_getCameraModelAsString_V2(camera->context, camera->info.model,
			sizeof(camera->info.model));
	camera->info.model[sizeof(camera->info.model) - 1u] = '\0';
	if ((camera->info.model[0] == '\0' &&
			copy_text(camera->info.model, sizeof(camera->info.model), NULL,
				"First Light Imaging camera") < 0) ||
			copy_text(camera->info.camera_name,
				sizeof(camera->info.camera_name), options->camera_name,
				"First Light Imaging camera") < 0) {
		res = -ENAMETOOLONG;
		goto error;
	}
	*camera_ptr = camera;
	return 0;

error:
	destroy_camera(camera);
	return res;
}

void flisdk_camera_close(struct flisdk_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	(void)flisdk_camera_stop(camera);
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	destroy_camera(camera);
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
			camera->n_buffers >= FLISDK_CAMERA_MAX_BUFFERS || camera->started)
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
	struct flisdk_camera_buffer *buffer;
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

int flisdk_camera_queue(struct flisdk_camera *camera,
		struct flisdk_camera_buffer *buffer)
{
	uint32_t i;
	int res;

	if (camera == NULL || buffer == NULL)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->lock)) != 0)
		return -res;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != buffer)
			continue;
		if (buffer->state != BUFFER_IDLE) {
			pthread_mutex_unlock(&camera->lock);
			return -EINVAL;
		}
		buffer->state = BUFFER_QUEUED;
		pthread_mutex_unlock(&camera->lock);
		return 0;
	}
	pthread_mutex_unlock(&camera->lock);
	return -ENOENT;
}

int flisdk_camera_start(struct flisdk_camera *camera)
{
	callbackHandler callback;
	int res;

	if (camera == NULL || camera->context == NULL || camera->n_buffers < 2)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->lock)) != 0)
		return -res;
	if (camera->started) {
		pthread_mutex_unlock(&camera->lock);
		return 0;
	}
	pthread_mutex_unlock(&camera->lock);
	callback = FliSdk_addCallbackNewImage_V2(camera->context,
			image_received, 0, true, camera);
	if (callback == NULL)
		return -EIO;
	camera->callback = callback;
	if ((res = pthread_mutex_lock(&camera->lock)) != 0) {
		FliSdk_removeCallbackNewImage_V2(camera->context, camera->callback);
		camera->callback = NULL;
		return -res;
	}
	camera->frame_id = 0;
	camera->started = true;
	pthread_mutex_unlock(&camera->lock);
	if (!FliSdk_start_V2(camera->context)) {
		uint32_t i;

		pthread_mutex_lock(&camera->lock);
		camera->started = false;
		pthread_mutex_unlock(&camera->lock);
		(void)FliSdk_stop_V2(camera->context);
		FliSdk_removeCallbackNewImage_V2(camera->context, camera->callback);
		camera->callback = NULL;
		pthread_mutex_lock(&camera->lock);
		while (camera->active_callbacks != 0)
			pthread_cond_wait(&camera->callbacks_idle, &camera->lock);
		for (i = 0; i < camera->n_buffers; i++)
			camera->buffers[i]->state = BUFFER_QUEUED;
		pthread_mutex_unlock(&camera->lock);
		return -EIO;
	}
	return 0;
}

int flisdk_camera_stop(struct flisdk_camera *camera)
{
	uint32_t i;
	int first_error = 0, res;

	if (camera == NULL)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->lock)) != 0)
		return -res;
	if (!camera->started) {
		pthread_mutex_unlock(&camera->lock);
		return 0;
	}
	camera->started = false;
	pthread_mutex_unlock(&camera->lock);
	if (!FliSdk_stop_V2(camera->context))
		first_error = -EIO;
	if (camera->callback != NULL) {
		FliSdk_removeCallbackNewImage_V2(camera->context, camera->callback);
		camera->callback = NULL;
	}
	if ((res = pthread_mutex_lock(&camera->lock)) != 0)
		return first_error != 0 ? first_error : -res;
	while (camera->active_callbacks != 0)
		pthread_cond_wait(&camera->callbacks_idle, &camera->lock);
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->state = BUFFER_IDLE;
	pthread_mutex_unlock(&camera->lock);
	return first_error;
}

int flisdk_camera_try_get_completion(struct flisdk_camera *camera,
		struct flisdk_camera_completion *completion)
{
	struct flisdk_camera_buffer *selected = NULL;
	uint64_t oldest_filling = UINT64_MAX;
	uint32_t i;
	int res;

	if (camera == NULL || completion == NULL)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->lock)) != 0)
		return -res;
	if (!camera->started) {
		pthread_mutex_unlock(&camera->lock);
		return -EINVAL;
	}
	for (i = 0; i < camera->n_buffers; i++) {
		struct flisdk_camera_buffer *buffer = camera->buffers[i];

		if (buffer->state == BUFFER_FILLING &&
				buffer->frame_id < oldest_filling)
			oldest_filling = buffer->frame_id;
		else if (buffer->state == BUFFER_COMPLETE &&
				(selected == NULL || buffer->frame_id < selected->frame_id))
			selected = buffer;
	}
	if (selected != NULL && selected->frame_id < oldest_filling) {
		selected->state = BUFFER_IDLE;
		*completion = (struct flisdk_camera_completion) {
			.buffer = selected,
			.user_data = selected->user_data,
			.frame_id = selected->frame_id,
			.size_filled = camera->info.payload_size,
		};
		pthread_mutex_unlock(&camera->lock);
		return 1;
	}
	pthread_mutex_unlock(&camera->lock);
	return 0;
}

int flisdk_camera_send_command(struct flisdk_camera *camera,
		const char *command, char *response, uint64_t response_size)
{
	int res;

	if (camera == NULL || camera->context == NULL || command == NULL ||
			response == NULL || response_size == 0 || response_size > SIZE_MAX)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->sdk_lock)) != 0)
		return -res;
	response[0] = '\0';
	if (!FliSerialCamera_sendCommand_V2(camera->context, command, response,
			(size_t)response_size))
		res = -EIO;
	else
		res = 0;
	response[response_size - 1u] = '\0';
	pthread_mutex_unlock(&camera->sdk_lock);
	return res;
}

int flisdk_camera_apply_layout(struct flisdk_camera *camera,
		uint32_t width, uint32_t height)
{
	int res;

	if (camera == NULL || camera->context == NULL || width == 0 || height == 0 ||
			width > UINT16_MAX || height > UINT16_MAX || camera->started ||
			camera->n_buffers != 0)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->sdk_lock)) != 0)
		return -res;
	FliSdk_setImageDimension_V2(camera->context, (uint16_t)width,
			(uint16_t)height);
	res = refresh_info(camera);
	if (res == 0 && (camera->info.width != width || camera->info.height != height))
		res = -EIO;
	pthread_mutex_unlock(&camera->sdk_lock);
	return res;
}

int flisdk_camera_refresh_info(struct flisdk_camera *camera)
{
	int res;

	if (camera == NULL || camera->context == NULL)
		return -EINVAL;
	if ((res = pthread_mutex_lock(&camera->sdk_lock)) != 0)
		return -res;
	res = refresh_info(camera);
	pthread_mutex_unlock(&camera->sdk_lock);
	return res;
}

/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct andor3_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct mock_feature {
	const char *name;
	const char *property_name;
	const char *description;
	enum andor3_feature_kind kind;
	bool writable;
	bool changes_layout;
};

static const struct mock_feature features[] = {
	{ "AOIWidth", "andor3.AOIWidth", "AOIWidth", ANDOR3_FEATURE_INTEGER, true, true },
	{ "AOIHeight", "andor3.AOIHeight", "AOIHeight", ANDOR3_FEATURE_INTEGER, true, true },
	{ "AOIStride", "andor3.AOIStride", "AOIStride", ANDOR3_FEATURE_INTEGER, false, true },
	{ "ImageSizeBytes", "andor3.ImageSizeBytes", "ImageSizeBytes",
		ANDOR3_FEATURE_INTEGER, false, true },
	{ "PixelEncoding", "andor3.PixelEncoding", "PixelEncoding",
		ANDOR3_FEATURE_ENUMERATION, true, true },
	{ "ExposureTime", "andor3.ExposureTime", "ExposureTime", ANDOR3_FEATURE_FLOATING, true, false },
	{ "SensorCooling", "andor3.SensorCooling", "SensorCooling", ANDOR3_FEATURE_BOOLEAN, true, false },
	{ "CameraModel", "andor3.CameraModel", "CameraModel", ANDOR3_FEATURE_STRING, false, false },
	{ "SerialNumber", "andor3.SerialNumber", "SerialNumber", ANDOR3_FEATURE_STRING, false, false },
	{ "SoftwareTrigger", "andor3-command.SoftwareTrigger", "SoftwareTrigger",
		ANDOR3_FEATURE_COMMAND, true, false },
};

static const char *pixel_encodings[] = { "Mono8", "Mono12", "Mono16" };

struct andor3_camera {
	struct andor3_camera_info info;
	struct andor3_camera_buffer *buffers[ANDOR3_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint64_t frame_id;
	int32_t pixel_encoding;
	double exposure_time;
	bool sensor_cooling;
	bool started;
};

static void refresh_layout(struct andor3_camera *camera)
{
	uint32_t bytes_per_pixel = camera->pixel_encoding == 0 ? 1u : 2u;

	camera->info.stride = camera->info.width * bytes_per_pixel;
	camera->info.image_size = (uint64_t)camera->info.stride * camera->info.height;
	camera->info.payload_size = camera->info.image_size;
	strcpy(camera->info.pixel_encoding,
			pixel_encodings[camera->pixel_encoding]);
}

int andor3_camera_open(struct andor3_camera **camera_ptr,
		const struct andor3_camera_options *options)
{
	struct andor3_camera *camera;

	if (camera_ptr == NULL || options == NULL || options->device_index != 0)
		return -ENODEV;
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	camera->info.width = 4;
	camera->info.height = 3;
	camera->pixel_encoding = 1;
	camera->exposure_time = 0.001;
	camera->sensor_cooling = true;
	strcpy(camera->info.model, "Synthetic Andor SDK3 camera");
	strcpy(camera->info.serial, "SIM-001");
	refresh_layout(camera);
	*camera_ptr = camera;
	return 0;
}

void andor3_camera_close(struct andor3_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	free(camera);
}

const struct andor3_camera_info *andor3_camera_get_info(
		const struct andor3_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

uint32_t andor3_camera_get_feature_count(const struct andor3_camera *camera)
{
	return camera == NULL ? 0 : (uint32_t)(sizeof(features) / sizeof(features[0]));
}

int andor3_camera_get_feature_info(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_info *info)
{
	const struct mock_feature *feature;

	if (camera == NULL || info == NULL ||
			index >= sizeof(features) / sizeof(features[0]))
		return -EINVAL;
	feature = &features[index];
	*info = (struct andor3_feature_info) {
		.name = feature->name,
		.property_name = feature->property_name,
		.description = feature->description,
		.kind = feature->kind,
		.n_enum_entries = feature->kind == ANDOR3_FEATURE_ENUMERATION ? 3u : 0u,
		.available = true,
		.readable = feature->kind != ANDOR3_FEATURE_COMMAND,
		.writable = feature->writable,
		.changes_layout = feature->changes_layout,
	};
	return 0;
}

const char *andor3_camera_get_feature_enum_entry(
		const struct andor3_camera *camera, uint32_t index, uint32_t entry_index)
{
	if (camera == NULL || index != 4 || entry_index >= 3)
		return NULL;
	return pixel_encodings[entry_index];
}

int andor3_camera_get_feature_value(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_value *value)
{
	if (camera == NULL || value == NULL ||
			index >= sizeof(features) / sizeof(features[0]))
		return -EINVAL;
	value->kind = features[index].kind;
	switch (index) {
	case 0: value->integer = camera->info.width; break;
	case 1: value->integer = camera->info.height; break;
	case 2: value->integer = camera->info.stride; break;
	case 3: value->integer = camera->info.payload_size; break;
	case 4: value->enumeration = camera->pixel_encoding; break;
	case 5: value->floating = camera->exposure_time; break;
	case 6: value->boolean = camera->sensor_cooling; break;
	case 7: value->string = camera->info.model; break;
	case 8: value->string = camera->info.serial; break;
	default: return -ENODATA;
	}
	return 0;
}

int andor3_camera_get_feature_integer_range(struct andor3_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum)
{
	if (camera == NULL || minimum == NULL || maximum == NULL || index > 3)
		return -EINVAL;
	*minimum = index < 2 ? 1 : 0;
	*maximum = index < 2 ? 4096 : INT32_MAX;
	return 0;
}

int andor3_camera_get_feature_float_range(struct andor3_camera *camera,
		uint32_t index, double *minimum, double *maximum)
{
	if (camera == NULL || minimum == NULL || maximum == NULL || index != 5)
		return -EINVAL;
	*minimum = 0.000001;
	*maximum = 10.0;
	return 0;
}

int andor3_camera_find_feature(const struct andor3_camera *camera,
		const char *property_name, uint32_t *index)
{
	uint32_t i;

	if (camera == NULL || property_name == NULL || index == NULL)
		return -EINVAL;
	for (i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
		if (strcmp(features[i].property_name, property_name) == 0) {
			*index = i;
			return 0;
		}
	}
	return -ENOENT;
}

int andor3_camera_set_feature_value(struct andor3_camera *camera,
		uint32_t index, const struct andor3_feature_value *value)
{
	if (camera == NULL || value == NULL ||
			index >= sizeof(features) / sizeof(features[0]) ||
			value->kind != features[index].kind || !features[index].writable)
		return -EINVAL;
	switch (index) {
	case 0:
		if (value->integer < 1 || value->integer > 4096)
			return -ERANGE;
		camera->info.width = (uint32_t)value->integer;
		refresh_layout(camera);
		break;
	case 1:
		if (value->integer < 1 || value->integer > 4096)
			return -ERANGE;
		camera->info.height = (uint32_t)value->integer;
		refresh_layout(camera);
		break;
	case 4:
		if (value->enumeration < 0 || value->enumeration >= 3)
			return -ERANGE;
		camera->pixel_encoding = value->enumeration;
		refresh_layout(camera);
		break;
	case 5: camera->exposure_time = value->floating; break;
	case 6: camera->sensor_cooling = value->boolean; break;
	case 9: return 0;
	default: return -EACCES;
	}
	return 0;
}

int andor3_camera_refresh_info(struct andor3_camera *camera)
{
	return camera == NULL || camera->started || camera->n_buffers != 0 ? -EBUSY : 0;
}

int andor3_camera_announce(struct andor3_camera *camera, void *memory,
		uint64_t size, void *user_data, struct andor3_camera_buffer **buffer_ptr)
{
	struct andor3_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			((uintptr_t)memory & 7u) != 0 || size < camera->info.payload_size ||
			camera->n_buffers >= ANDOR3_CAMERA_MAX_BUFFERS)
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

int andor3_camera_revoke(struct andor3_camera *camera,
		struct andor3_camera_buffer **buffer_ptr)
{
	uint32_t i;

	if (camera == NULL || buffer_ptr == NULL || *buffer_ptr == NULL || camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != *buffer_ptr) continue;
		free(*buffer_ptr);
		*buffer_ptr = NULL;
		memmove(&camera->buffers[i], &camera->buffers[i + 1],
				(camera->n_buffers - i - 1u) * sizeof(camera->buffers[0]));
		camera->n_buffers--;
		return 0;
	}
	return -ENOENT;
}

int andor3_camera_queue(struct andor3_camera *camera,
		struct andor3_camera_buffer *buffer)
{
	uint32_t i;

	if (camera == NULL || buffer == NULL || buffer->queued) return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] == buffer) {
			buffer->queued = true;
			return 0;
		}
	}
	return -ENOENT;
}

int andor3_camera_start(struct andor3_camera *camera)
{
	if (camera == NULL || camera->n_buffers < 2) return -EINVAL;
	camera->frame_id = 0;
	camera->started = true;
	return 0;
}

int andor3_camera_stop(struct andor3_camera *camera)
{
	uint32_t i;
	if (camera == NULL) return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) camera->buffers[i]->queued = false;
	camera->started = false;
	return 0;
}

int andor3_camera_try_get_completion(struct andor3_camera *camera,
		struct andor3_camera_completion *completion)
{
	uint32_t i;

	if (camera == NULL || completion == NULL || !camera->started) return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct andor3_camera_buffer *buffer = camera->buffers[index];
		uint8_t *bytes;
		uint32_t j;

		if (!buffer->queued) continue;
		buffer->queued = false;
		camera->scan_hint = (index + 1u) % camera->n_buffers;
		camera->frame_id++;
		bytes = buffer->memory;
		for (j = 0; j < camera->info.image_size; j++)
			bytes[j] = (uint8_t)(camera->frame_id + j);
		*completion = (struct andor3_camera_completion) {
			.buffer = buffer,
			.user_data = buffer->user_data,
			.frame_id = camera->frame_id,
			.size_filled = camera->info.image_size,
		};
		return 1;
	}
	return 0;
}

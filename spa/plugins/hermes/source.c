/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/pod.h>

#include "camera.h"
#include "hermes.h"
#include "../image-frame.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS HERMES_CAMERA_MAX_BUFFERS

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	struct spa_ndarray_info format;
	struct spa_io_buffers *io;
	bool have_format;
	uint32_t n_buffers;
};

struct buffer_slot {
	struct spa_buffer *buffer;
	struct hermes_camera_buffer *camera_buffer;
	uint32_t id;
	bool camera_queued;
	bool published;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_log *log;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_dict props;
	struct spa_dict_item prop_items[24];
	char node_name[192];
	char node_description[256];
	char device_id[64];
	char frames_per_buffer[16];
	char counters[8];
	char bits_per_pixel[8];
	char batch_rate[32];
	char exposure_clocks[16];
	char integrated_frames[16];
	struct port port;
	struct hermes_camera *camera;
	struct hermes_camera_info camera_info;
	struct hermes_camera_options options;
	struct spa_fraction rate;
	struct buffer_slot slots[MAX_BUFFERS];
	bool started;
	bool discontinuity;
};

static int64_t monotonic_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return SPA_TIME_INVALID;
	return (int64_t)((uint64_t)now.tv_sec * SPA_NSEC_PER_SEC +
			(uint64_t)now.tv_nsec);
}

static const char *lookup(const struct spa_dict *info, const char *key)
{
	return info == NULL ? NULL : spa_dict_lookup(info, key);
}

static int parse_u32_default(const struct spa_dict *info, const char *key,
		uint32_t minimum, uint32_t maximum, uint32_t default_value,
		uint32_t *value)
{
	const char *text = lookup(info, key);

	if (text == NULL) {
		*value = default_value;
		return 0;
	}
	if (!spa_atou32(text, value, 10) || *value < minimum || *value > maximum)
		return -EINVAL;
	return 0;
}

static int parse_bool_default(const struct spa_dict *info, const char *key,
		bool default_value, bool *value)
{
	const char *text = lookup(info, key);

	if (text == NULL) {
		*value = default_value;
		return 0;
	}
	if (spa_streq(text, "true") || spa_streq(text, "1"))
		*value = true;
	else if (spa_streq(text, "false") || spa_streq(text, "0"))
		*value = false;
	else
		return -EINVAL;
	return 0;
}

static int parse_rate(const char *text, struct spa_fraction *rate)
{
	char *end;
	unsigned long numerator, denominator;

	if (text == NULL)
		text = "2000/1";
	errno = 0;
	numerator = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '/' || numerator == 0 ||
			numerator > UINT32_MAX)
		return -EINVAL;
	text = end + 1;
	errno = 0;
	denominator = strtoul(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' || denominator == 0 ||
			denominator > UINT32_MAX)
		return -EINVAL;
	*rate = SPA_FRACTION((uint32_t)numerator, (uint32_t)denominator);
	return 0;
}

static int parse_options(struct impl *self, const struct spa_dict *info)
{
	const char *device_id = lookup(info, SPA_KEY_API_HERMES_DEVICE_ID);
	const char *mode = lookup(info, SPA_KEY_API_HERMES_CAMERA_MODE);
	uint32_t force_8bit, half_array, signed_data;
	int res;

	if (device_id == NULL)
		device_id = "";
	if (strlen(device_id) >= sizeof(self->device_id))
		return -ENAMETOOLONG;
	memcpy(self->device_id, device_id, strlen(device_id) + 1u);
	self->options.device_id = self->device_id;
	if (mode == NULL || spa_streq(mode, "normal"))
		self->options.advanced_mode = false;
	else if (spa_streq(mode, "advanced"))
		self->options.advanced_mode = true;
	else
		return -EINVAL;
	if ((res = parse_u32_default(info, SPA_KEY_API_HERMES_EXPOSURE_CLOCKS,
			1, UINT16_MAX - 1u, 1040, &self->options.exposure_clocks)) < 0 ||
			(res = parse_u32_default(info,
					SPA_KEY_API_HERMES_INTEGRATED_FRAMES,
					1, UINT16_MAX - 1u, 1,
					&self->options.integrated_frames)) < 0 ||
			(res = parse_u32_default(info, SPA_KEY_API_HERMES_COUNTERS,
					1, 3, 1, &self->options.counters)) < 0 ||
			(res = parse_u32_default(info,
					SPA_KEY_API_HERMES_FRAMES_PER_BUFFER,
					1, UINT16_MAX, 50,
					&self->options.frames_per_buffer)) < 0 ||
			(res = parse_u32_default(info, SPA_KEY_API_HERMES_BITS_PER_PIXEL,
					8, 16, 8, &self->options.bits_per_pixel)) < 0 ||
			(res = parse_bool_default(info, SPA_KEY_API_HERMES_FORCE_8BIT,
					true, &self->options.force_8bit)) < 0 ||
			(res = parse_bool_default(info, SPA_KEY_API_HERMES_HALF_ARRAY,
					false, &self->options.half_array)) < 0 ||
			(res = parse_bool_default(info, SPA_KEY_API_HERMES_SIGNED_DATA,
					false, &self->options.signed_data)) < 0 ||
			(res = parse_rate(lookup(info, SPA_KEY_API_HERMES_BATCH_RATE),
					&self->rate)) < 0)
		return res;
	force_8bit = self->options.force_8bit;
	half_array = self->options.half_array;
	signed_data = self->options.signed_data;
	if ((self->options.bits_per_pixel != 8 &&
			self->options.bits_per_pixel != 16) ||
			(force_8bit && self->options.bits_per_pixel != 8) ||
			(half_array && self->options.counters != 1) || signed_data)
		return -EINVAL;
	return 0;
}

static void emit_node_info(struct impl *self, bool full)
{
	uint64_t old = full ? self->info.change_mask : 0;

	if (full)
		self->info.change_mask = self->info_all;
	if (self->info.change_mask != 0) {
		spa_node_emit_info(&self->hooks, &self->info);
		self->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *self, bool full)
{
	uint64_t old = full ? self->port.info.change_mask : 0;

	if (full)
		self->port.info.change_mask = self->port.info_all;
	if (self->port.info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_OUTPUT, 0,
				&self->port.info);
		self->port.info.change_mask = old;
	}
}

static int node_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *self = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_node_info(self, true);
	emit_port_info(self, true);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

static int node_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int node_enum_params(void *object SPA_UNUSED, int seq SPA_UNUSED,
		uint32_t id SPA_UNUSED, uint32_t start SPA_UNUSED,
		uint32_t num SPA_UNUSED, const struct spa_pod *filter SPA_UNUSED)
{
	return -ENOENT;
}

static int node_set_param(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		uint32_t flags SPA_UNUSED, const struct spa_pod *param SPA_UNUSED)
{
	return -ENOENT;
}

static int node_set_io(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		void *data SPA_UNUSED, size_t size SPA_UNUSED)
{
	return -ENOTSUP;
}

static int queue_slot(struct impl *self, struct buffer_slot *slot)
{
	int res = hermes_camera_queue(self->camera, slot->camera_buffer);

	if (res < 0)
		return res;
	slot->camera_queued = true;
	return 0;
}

static int queue_producer_buffers(struct impl *self)
{
	uint32_t i;

	for (i = 0; i < self->port.n_buffers; i++) {
		struct buffer_slot *slot = &self->slots[i];

		if (!slot->camera_queued && !slot->published && slot->buffer != NULL &&
				queue_slot(self, slot) < 0)
			return -EIO;
	}
	return 0;
}

static int stop_source(struct impl *self)
{
	uint32_t i;
	int res;

	if (!self->started)
		return 0;
	res = hermes_camera_stop(self->camera);
	for (i = 0; i < self->port.n_buffers; i++)
		self->slots[i].camera_queued = false;
	self->started = false;
	return res;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;
	int res;

	spa_return_val_if_fail(self != NULL && command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!self->port.have_format || self->port.n_buffers == 0 ||
				self->port.io == NULL)
			return -EIO;
		if (self->started)
			return 0;
		if ((res = queue_producer_buffers(self)) < 0 ||
				(res = hermes_camera_start(self->camera)) < 0)
			return res;
		self->started = true;
		self->discontinuity = true;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		return stop_source(self);
	default:
		return -ENOTSUP;
	}
}

static int node_add_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED,
		const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static struct spa_pod *build_format(struct impl *self,
		struct spa_pod_builder *builder, uint32_t id)
{
	int32_t shape[3] = {
		(int32_t)self->camera_info.frames_per_buffer,
		(int32_t)self->camera_info.counters,
		(int32_t)self->camera_info.raw_plane_bytes,
	};

	return spa_pod_builder_add_object(builder, SPA_TYPE_OBJECT_Format, id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String(SPA_HERMES_RAW_BATCH_SCHEMA),
			SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id(SPA_ELEMENT_TYPE_U8),
			SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int, 3, shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&self->rate));
}

static int build_port_param(struct impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format:
		if (index > 0 || (id == SPA_PARAM_Format && !self->port.have_format))
			return 0;
		*param = build_format(self, builder, id);
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Buffers:
		if (index > 0 || !self->port.have_format)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int((int32_t)self->camera_info.payload_size),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int((int32_t)self->camera_info.raw_plane_bytes),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Meta:
		if (index == 0)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
		else if (index == 1)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
		else
			return 0;
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers)));
		return *param == NULL ? -ENOSPC : 1;
	default:
		return -ENOENT;
	}
}

static int port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *self = object;
	struct spa_result_node_params result = { .id = id, .next = start };
	uint8_t storage[1536];
	uint32_t count = 0;

	spa_return_val_if_fail(self != NULL && direction == SPA_DIRECTION_OUTPUT &&
			port_id == 0 && num > 0, -EINVAL);
	while (count < num) {
		struct spa_pod_builder builder;
		struct spa_pod *param = NULL;
		int res;

		result.index = result.next++;
		spa_pod_builder_init(&builder, storage, sizeof(storage));
		res = build_port_param(self, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int string_property(const struct spa_pod *format, uint32_t key,
		const char **value)
{
	const struct spa_pod_prop *property = spa_pod_find_prop(format, NULL, key);

	if (property == NULL || spa_pod_get_string(&property->value, value) < 0)
		return -EINVAL;
	return 0;
}

static int validate_format(struct impl *self, const struct spa_pod *param,
		struct spa_ndarray_info *format)
{
	uint8_t storage[1536];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			sizeof(storage));
	const struct spa_pod *fixed;
	const char *schema;

	fixed = pipewireao_pod_unwrap_fixed_choices(&builder, param);
	if (fixed == NULL || spa_format_ndarray_parse(fixed, format) < 0 ||
			format->element_type != SPA_ELEMENT_TYPE_U8 ||
			format->layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
			format->n_dimensions != 3 ||
			format->shape[0] != self->camera_info.frames_per_buffer ||
			format->shape[1] != self->camera_info.counters ||
			format->shape[2] != self->camera_info.raw_plane_bytes ||
			format->rate.num != self->rate.num ||
			format->rate.denom != self->rate.denom ||
			spa_ndarray_format_key_count(fixed,
					SPA_FORMAT_NDARRAY_schema) != 1 ||
			string_property(fixed, SPA_FORMAT_NDARRAY_schema, &schema) < 0 ||
			!spa_streq(schema, SPA_HERMES_RAW_BATCH_SCHEMA))
		return -EINVAL;
	return 0;
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();

	spa_return_val_if_fail(self != NULL && direction == SPA_DIRECTION_OUTPUT &&
			port_id == 0, -EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (self->started || self->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY))
			self->port.have_format = false;
		return 0;
	}
	if (validate_format(self, param, &format) < 0)
		return -EINVAL;
	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		self->port.format = format;
		self->port.have_format = true;
		self->port.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
		self->port.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
		self->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		emit_port_info(self, false);
	}
	return 0;
}

static int release_buffers(struct impl *self)
{
	uint32_t i;
	int first_error = 0, res;

	if (self->started)
		return -EBUSY;
	for (i = 0; i < self->port.n_buffers; i++) {
		struct buffer_slot *slot = &self->slots[i];

		if (slot->camera_buffer != NULL &&
				(res = hermes_camera_revoke(self->camera,
						&slot->camera_buffer)) < 0 && first_error == 0)
			first_error = res;
		memset(slot, 0, sizeof(*slot));
	}
	self->port.n_buffers = 0;
	return first_error;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t flags SPA_UNUSED, uint32_t port_id,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *self = object;
	uint32_t announced = 0, i;
	int res;

	spa_return_val_if_fail(self != NULL && direction == SPA_DIRECTION_OUTPUT &&
			port_id == 0, -EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return self->port.n_buffers == 0 ? 0 : release_buffers(self);
	if (!self->port.have_format || self->port.n_buffers != 0 ||
			buffers == NULL || n_buffers < MIN_BUFFERS || n_buffers > MAX_BUFFERS)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				(data->type != SPA_DATA_MemPtr &&
				 data->type != SPA_DATA_MemFd) ||
				data->maxsize < self->camera_info.payload_size ||
				data->chunk == NULL ||
				spa_buffer_find_meta_data(buffers[i], SPA_META_Header,
					sizeof(struct spa_meta_header)) == NULL ||
				spa_buffer_find_meta_data(buffers[i], SPA_META_Acquisition,
					sizeof(struct spa_meta_acquisition)) == NULL)
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer_slot *slot = &self->slots[i];
		struct spa_data *data = &buffers[i]->datas[0];

		slot->buffer = buffers[i];
		slot->id = i;
		if ((res = hermes_camera_announce(self->camera, data->data,
				data->maxsize, slot, &slot->camera_buffer)) < 0)
			goto error;
		announced++;
	}
	self->port.n_buffers = n_buffers;
	return 0;

error:
	for (i = 0; i < announced; i++)
		if (self->slots[i].camera_buffer != NULL)
			(void)hermes_camera_revoke(self->camera,
					&self->slots[i].camera_buffer);
	for (i = 0; i < n_buffers; i++)
		memset(&self->slots[i], 0, sizeof(self->slots[i]));
	return res;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL && direction == SPA_DIRECTION_OUTPUT &&
			port_id == 0, -EINVAL);
	if (id != SPA_IO_Buffers)
		return -ENOENT;
	if (data != NULL && size < sizeof(struct spa_io_buffers))
		return -EINVAL;
	self->port.io = data;
	return 0;
}

static int port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int recycle_buffer(struct impl *self)
{
	struct spa_io_buffers *io = self->port.io;
	struct buffer_slot *slot;
	uint32_t id;

	if (io == NULL || io->status == SPA_STATUS_HAVE_DATA ||
			io->buffer_id == SPA_ID_INVALID)
		return 0;
	id = io->buffer_id;
	if (id >= self->port.n_buffers)
		return -EPROTO;
	slot = &self->slots[id];
	if (!slot->published || slot->camera_queued || slot->buffer == NULL)
		return -EPROTO;
	io->buffer_id = SPA_ID_INVALID;
	slot->published = false;
	return queue_slot(self, slot) < 0 ? -EIO : 1;
}

static int publish_completion(struct impl *self,
		const struct hermes_camera_completion *completion)
{
	struct spa_meta_acquisition acquisition;
	struct pwao_image_frame frame;
	struct buffer_slot *slot = completion->user_data;
	uint32_t header_flags = self->discontinuity ?
			SPA_META_HEADER_FLAG_DISCONT : 0;
	int res;

	if (slot == NULL || slot->camera_buffer != completion->buffer ||
			!slot->camera_queued || slot->buffer == NULL || slot->published)
		return -EPROTO;
	slot->camera_queued = false;
	if (completion->size_filled != self->camera_info.payload_size ||
			completion->size_filled > UINT32_MAX) {
		res = -EMSGSIZE;
		goto recycle;
	}
	spa_meta_acquisition_init(&acquisition);
	frame = (struct pwao_image_frame) {
		.data_index = 0,
		.header_flags = header_flags,
		.chunk_flags = completion->incomplete ? SPA_CHUNK_FLAG_CORRUPTED : 0,
		.offset = 0,
		.size = (uint32_t)completion->size_filled,
		.stride = (int32_t)self->camera_info.raw_plane_bytes,
		.sequence = completion->batch_id,
		.pts = monotonic_nsec(),
		.acquisition = &acquisition,
	};
	if (self->port.io == NULL || self->port.io->status == SPA_STATUS_HAVE_DATA)
		res = -EBUSY;
	else if ((res = pwao_image_frame_write(slot->buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER |
			PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION)) >= 0) {
		self->port.io->buffer_id = slot->id;
		self->port.io->status = SPA_STATUS_HAVE_DATA;
		slot->published = true;
	}
	if (res >= 0) {
		self->discontinuity = false;
		return 1;
	}
	if (res == -EBUSY) {
		self->discontinuity = true;
		res = 0;
	}

recycle:
	(void)queue_slot(self, slot);
	return res;
}

static int node_process(void *object)
{
	struct impl *self = object;
	struct hermes_camera_completion completion;
	int res;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	if (!self->started)
		return SPA_STATUS_OK;
	if ((res = recycle_buffer(self)) < 0)
		return res;
	if ((res = hermes_camera_get_completion(self->camera, &completion)) < 0)
		return res;
	if (res == 0)
		return SPA_STATUS_OK;
	res = publish_completion(self, &completion);
	if (res < 0)
		return res;
	return res > 0 ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
}

static const struct spa_node_methods node_methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = node_add_listener,
	.set_callbacks = node_set_callbacks,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.set_io = node_set_io,
	.send_command = node_send_command,
	.add_port = node_add_port,
	.remove_port = node_remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = port_reuse_buffer,
	.process = node_process,
};

static int get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *self = (struct impl *)handle;

	spa_return_val_if_fail(handle != NULL && interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct impl *self = (struct impl *)handle;
	int first_error = 0, res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	if ((res = stop_source(self)) < 0)
		first_error = res;
	if (self->port.n_buffers != 0 &&
			(res = release_buffers(self)) < 0 && first_error == 0)
		first_error = res;
	hermes_camera_close(self->camera);
	self->camera = NULL;
	return first_error;
}

static size_t get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static void configure_props(struct impl *self)
{
	uint32_t n = 0;

	(void)snprintf(self->node_name, sizeof(self->node_name),
			"hermes_source.%s", self->camera_info.serial[0] != '\0' ?
			self->camera_info.serial : "default");
	(void)snprintf(self->node_description, sizeof(self->node_description),
			"%s FrontPanel raw batch source", self->camera_info.model);
	(void)snprintf(self->frames_per_buffer, sizeof(self->frames_per_buffer),
			"%u", self->camera_info.frames_per_buffer);
	(void)snprintf(self->counters, sizeof(self->counters), "%u",
			self->camera_info.counters);
	(void)snprintf(self->bits_per_pixel, sizeof(self->bits_per_pixel), "%u",
			self->camera_info.bits_per_pixel);
	(void)snprintf(self->batch_rate, sizeof(self->batch_rate), "%u/%u",
			self->rate.num, self->rate.denom);
	(void)snprintf(self->exposure_clocks, sizeof(self->exposure_clocks), "%u",
			self->options.exposure_clocks);
	(void)snprintf(self->integrated_frames, sizeof(self->integrated_frames),
			"%u", self->options.integrated_frames);
#define ADD_ITEM(key, value) self->prop_items[n++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_API, "hermes-frontpanel");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->node_description);
	ADD_ITEM(SPA_KEY_NODE_DRIVER, "true");
	ADD_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, "Micro Photon Devices");
	ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, self->camera_info.model);
	ADD_ITEM(SPA_KEY_DEVICE_SERIAL, self->camera_info.serial);
	ADD_ITEM(SPA_KEY_API_HERMES_DEVICE_ID, self->device_id);
	ADD_ITEM(SPA_KEY_API_HERMES_FRAMES_PER_BUFFER, self->frames_per_buffer);
	ADD_ITEM(SPA_KEY_API_HERMES_COUNTERS, self->counters);
	ADD_ITEM(SPA_KEY_API_HERMES_BITS_PER_PIXEL, self->bits_per_pixel);
	ADD_ITEM(SPA_KEY_API_HERMES_BATCH_RATE, self->batch_rate);
	ADD_ITEM(SPA_KEY_API_HERMES_CAMERA_MODE,
			self->options.advanced_mode ? "advanced" : "normal");
	ADD_ITEM(SPA_KEY_API_HERMES_EXPOSURE_CLOCKS, self->exposure_clocks);
	ADD_ITEM(SPA_KEY_API_HERMES_INTEGRATED_FRAMES, self->integrated_frames);
	ADD_ITEM(SPA_KEY_API_HERMES_FORCE_8BIT,
			self->options.force_8bit ? "true" : "false");
	ADD_ITEM(SPA_KEY_API_HERMES_HALF_ARRAY,
			self->options.half_array ? "true" : "false");
	ADD_ITEM(SPA_KEY_API_HERMES_SIGNED_DATA, "false");
	ADD_ITEM(SPA_KEY_API_HERMES_READINESS, "poll");
#undef ADD_ITEM
	self->props = SPA_DICT_INIT(self->prop_items, n);
}

static int init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *self = (struct impl *)handle;
	const struct hermes_camera_info *camera_info;
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(self, 0, sizeof(*self));
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	if ((res = parse_options(self, info)) < 0)
		return res;
	spa_hook_list_init(&self->hooks);
	if ((res = hermes_camera_open(&self->camera, &self->options)) < 0)
		return res;
	camera_info = hermes_camera_get_info(self->camera);
	if (camera_info == NULL || camera_info->payload_size > INT32_MAX ||
			camera_info->raw_plane_bytes > INT32_MAX) {
		res = -ENOTSUP;
		goto error;
	}
	self->camera_info = *camera_info;
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_POLL_DRIVER;
	configure_props(self);
	self->info.props = &self->props;
	self->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->port.info = SPA_PORT_INFO_INIT();
	self->port.info.flags = SPA_PORT_FLAG_LIVE;
	self->port.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	self->port.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	self->port.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	self->port.params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta,
			SPA_PARAM_INFO_READ);
	self->port.params[4] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	self->port.info.params = self->port.params;
	self->port.info.n_params = SPA_N_ELEMENTS(self->port.params);
	return 0;

error:
	hermes_camera_close(self->camera);
	self->camera = NULL;
	return res;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_hermes_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_HERMES_SOURCE,
	NULL,
	get_size,
	init,
	enum_interface_info,
};

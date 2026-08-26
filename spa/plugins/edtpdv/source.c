/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
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
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include "camera.h"
#include "edtpdv.h"
#include "../image-frame.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS EDTPDV_CAMERA_MAX_BUFFERS

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	struct spa_video_info_raw format;
	struct spa_io_buffers *io;
	bool have_format;
	uint32_t n_buffers;
};

struct buffer_slot {
	struct spa_buffer *buffer;
	struct edtpdv_camera_buffer *camera_buffer;
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
	struct spa_dict_item prop_items[13];
	char node_name[192];
	char node_description[256];
	char device[64];
	char unit[16];
	char channel[16];
	char ring_buffers[16];
	struct port port;
	struct edtpdv_camera *camera;
	struct edtpdv_camera_info camera_info;
	struct buffer_slot slots[MAX_BUFFERS];
	uint32_t video_format;
	bool started;
	bool discontinuity;
	bool have_sequence;
	uint64_t last_sequence;
};

static int64_t monotonic_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return SPA_TIME_INVALID;
	return (int64_t)((uint64_t)now.tv_sec * SPA_NSEC_PER_SEC +
			(uint64_t)now.tv_nsec);
}

static int copy_text(char *destination, size_t capacity, const char *source)
{
	if (source == NULL || source[0] == '\0' || strlen(source) >= capacity)
		return -EINVAL;
	memcpy(destination, source, strlen(source) + 1u);
	return 0;
}

static int parse_u32(const struct spa_dict *info, const char *key,
		uint32_t minimum, uint32_t maximum, uint32_t *value)
{
	const char *text = info == NULL ? NULL : spa_dict_lookup(info, key);

	if (text == NULL || !spa_atou32(text, value, 10) || *value < minimum ||
			*value > maximum)
		return -EINVAL;
	return 0;
}

static int map_format(const struct edtpdv_camera_info *info,
		uint32_t *format)
{
	if (info->depth == 8 && info->pitch >= info->width) {
		*format = SPA_VIDEO_FORMAT_GRAY8;
		return 0;
	}
	if (info->depth >= 10 && info->depth <= 16 &&
			info->width <= UINT32_MAX / 2u &&
			info->pitch >= info->width * 2u) {
		*format = SPA_VIDEO_FORMAT_GRAY16_LE;
		return 0;
	}
	return -ENOTSUP;
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
	int res;

	if ((res = edtpdv_camera_queue(self->camera, slot->camera_buffer)) < 0)
		return res;
	slot->camera_queued = true;
	return 0;
}

static int queue_producer_buffers(struct impl *self)
{
	uint32_t i;

	for (i = 0; i < self->port.n_buffers; i++) {
		struct buffer_slot *slot = &self->slots[i];

		if (slot->camera_queued || slot->published || slot->buffer == NULL)
			continue;
		if (queue_slot(self, slot) < 0)
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
	res = edtpdv_camera_stop(self->camera);
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
		if ((res = queue_producer_buffers(self)) < 0)
			return res;
		if ((res = edtpdv_camera_start(self->camera)) < 0)
			return res;
		self->started = true;
		self->have_sequence = false;
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

static int build_port_param(struct impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	struct port *port = &self->port;

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(self->video_format),
				SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(
						self->camera_info.width, self->camera_info.height)),
				SPA_FORMAT_VIDEO_framerate,
				SPA_POD_Fraction(&SPA_FRACTION(0, 1)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Format:
		if (index > 0 || !port->have_format)
			return 0;
		*param = spa_format_video_raw_build(builder, id, &port->format);
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Buffers:
		if (index > 0 || !port->have_format)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int((int32_t)self->camera_info.payload_size),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int((int32_t)self->camera_info.pitch),
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
	uint8_t storage[1024];
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

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	struct spa_video_info_raw format = { 0 };

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
	if (spa_format_video_raw_parse(param, &format) < 0 ||
			format.format != self->video_format ||
			format.size.width != self->camera_info.width ||
			format.size.height != self->camera_info.height)
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
				(res = edtpdv_camera_revoke(self->camera,
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
		if ((res = edtpdv_camera_announce(self->camera, data->data,
				data->maxsize, slot, &slot->camera_buffer)) < 0)
			goto error;
		announced++;
	}
	self->port.n_buffers = n_buffers;
	return 0;

error:
	for (i = 0; i < announced; i++)
		if (self->slots[i].camera_buffer != NULL)
			(void)edtpdv_camera_revoke(self->camera,
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
		const struct edtpdv_camera_completion *completion)
{
	struct spa_meta_acquisition acquisition;
	struct pwao_image_frame frame;
	struct buffer_slot *slot = completion->user_data;
	uint32_t header_flags = self->discontinuity ?
			SPA_META_HEADER_FLAG_DISCONT : 0;
	uint32_t chunk_flags = completion->incomplete ?
			SPA_CHUNK_FLAG_CORRUPTED : 0;
	int res;

	if (slot == NULL || slot->camera_buffer != completion->buffer ||
			!slot->camera_queued || slot->buffer == NULL || slot->published)
		return -EPROTO;
	slot->camera_queued = false;
	if (self->have_sequence && completion->frame_id != self->last_sequence + 1u)
		header_flags |= SPA_META_HEADER_FLAG_DISCONT;
	self->have_sequence = true;
	self->last_sequence = completion->frame_id;
	if (completion->size_filled > UINT32_MAX ||
			completion->size_filled > slot->buffer->datas[0].maxsize) {
		res = -ENOSPC;
		goto recycle;
	}
	spa_meta_acquisition_init(&acquisition);
	frame = (struct pwao_image_frame) {
		.data_index = 0,
		.header_flags = header_flags,
		.chunk_flags = chunk_flags,
		.offset = 0,
		.size = (uint32_t)completion->size_filled,
		.stride = (int32_t)self->camera_info.pitch,
		.sequence = completion->frame_id,
		.pts = monotonic_nsec(),
		.acquisition = &acquisition,
	};
	if (self->port.io == NULL ||
			self->port.io->status == SPA_STATUS_HAVE_DATA) {
		res = -EBUSY;
	} else if ((res = pwao_image_frame_write(slot->buffer, &frame,
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
	struct edtpdv_camera_completion completion;
	int res;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	if (!self->started)
		return SPA_STATUS_OK;
	if ((res = recycle_buffer(self)) < 0)
		return res;
	if ((res = edtpdv_camera_try_get_completion(self->camera,
			&completion)) < 0)
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
	edtpdv_camera_close(self->camera);
	self->camera = NULL;
	return first_error;
}

static size_t get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static void configure_props(struct impl *self,
		const struct edtpdv_camera_options *options)
{
	uint32_t n = 0;

	snprintf(self->node_name, sizeof(self->node_name), "edtpdv_source.%u.%u",
			options->unit, options->channel);
	snprintf(self->node_description, sizeof(self->node_description),
			"%s (EDT %s unit %u channel %u)", self->camera_info.model,
			options->device, options->unit, options->channel);
	snprintf(self->unit, sizeof(self->unit), "%u", options->unit);
	snprintf(self->channel, sizeof(self->channel), "%u", options->channel);
	snprintf(self->ring_buffers, sizeof(self->ring_buffers), "%u",
			options->ring_buffers);
#define ADD_ITEM(key, value) \
	self->prop_items[n++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "edtpdv");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->node_description);
	ADD_ITEM(SPA_KEY_NODE_DRIVER, "true");
	ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, self->camera_info.model);
	ADD_ITEM(SPA_KEY_API_EDTPDV_DEVICE, self->device);
	ADD_ITEM(SPA_KEY_API_EDTPDV_UNIT, self->unit);
	ADD_ITEM(SPA_KEY_API_EDTPDV_CHANNEL, self->channel);
	ADD_ITEM(SPA_KEY_API_EDTPDV_RING_BUFFERS, self->ring_buffers);
	ADD_ITEM(SPA_KEY_API_EDTPDV_READINESS, "poll");
#undef ADD_ITEM
	self->props = SPA_DICT_INIT(self->prop_items, n);
}

static int init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *self = (struct impl *)handle;
	struct edtpdv_camera_options options = { .ring_buffers = 4 };
	const struct edtpdv_camera_info *camera_info;
	const char *value;
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(self, 0, sizeof(*self));
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	value = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_EDTPDV_DEVICE);
	if (copy_text(self->device, sizeof(self->device), value) < 0 ||
			parse_u32(info, SPA_KEY_API_EDTPDV_UNIT, 0, INT_MAX,
				&options.unit) < 0 ||
			parse_u32(info, SPA_KEY_API_EDTPDV_CHANNEL, 0, INT_MAX,
				&options.channel) < 0)
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_EDTPDV_RING_BUFFERS);
	if (value != NULL && (!spa_atou32(value, &options.ring_buffers, 10) ||
			options.ring_buffers < 2 || options.ring_buffers > MAX_BUFFERS))
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_EDTPDV_READINESS);
	if (value != NULL && !spa_streq(value, "poll"))
		return -EINVAL;
	options.device = self->device;
	spa_hook_list_init(&self->hooks);
	if ((res = edtpdv_camera_open(&self->camera, &options)) < 0)
		return res;
	camera_info = edtpdv_camera_get_info(self->camera);
	if (camera_info == NULL || camera_info->payload_size > INT32_MAX ||
			camera_info->pitch > INT32_MAX ||
			map_format(camera_info, &self->video_format) < 0) {
		res = -ENOTSUP;
		goto error;
	}
	self->camera_info = *camera_info;
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_POLL_DRIVER;
	configure_props(self, &options);
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
	edtpdv_camera_close(self->camera);
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

const struct spa_handle_factory spa_edtpdv_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_EDTPDV_SOURCE,
	NULL,
	get_size,
	init,
	enum_interface_info,
};

/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <spa/buffer/image-source-latest.h>
#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include "bgapi2.h"
#include "camera.h"
#include "params.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS SPA_IMAGE_SOURCE_MAX_BUFFERS

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	struct spa_video_info_raw format;
	bool have_format;
	uint32_t n_buffers;
};

struct buffer_slot {
	struct spa_image_source_buffer *image;
	BGAPI2_Buffer *camera_buffer;
	bool camera_queued;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_log *log;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[2];
	struct spa_dict props;
	struct spa_dict_item prop_items[11];
	char node_name[192];
	char node_description[256];
	char producer_path[PATH_MAX];
	char interface_index[16];
	char device_index[16];
	char stream_index[16];
	struct port port;
	struct spa_buffer_latest *latest;
	struct spa_image_source_latest transport;
	struct spa_image_source source;
	struct bgapi2_camera *camera;
	struct bgapi2_camera_info camera_info;
	struct buffer_slot slots[MAX_BUFFERS];
	uint32_t video_format;
	uint32_t bytes_per_pixel;
	bool started;
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

static int parse_index(const struct spa_dict *info, const char *key,
		uint32_t default_value, uint32_t *value)
{
	const char *text = info == NULL ? NULL : spa_dict_lookup(info, key);

	if (text == NULL) {
		*value = default_value;
		return 0;
	}
	return spa_atou32(text, value, 0) ? 0 : -EINVAL;
}

static int map_pixel_format(const char *name, uint32_t *format,
		uint32_t *bytes_per_pixel)
{
	if (spa_streq(name, "Mono8")) {
		*format = SPA_VIDEO_FORMAT_GRAY8;
		*bytes_per_pixel = 1;
		return 0;
	}
	if (spa_streq(name, "Mono10") || spa_streq(name, "Mono12") ||
			spa_streq(name, "Mono14") || spa_streq(name, "Mono16")) {
		*format = SPA_VIDEO_FORMAT_GRAY16_LE;
		*bytes_per_pixel = 2;
		return 0;
	}
	return -ENOTSUP;
}

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask != 0) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, bool full)
{
	struct port *port = &this->port;
	uint64_t old = full ? port->info.change_mask : 0;

	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask != 0) {
		spa_node_emit_port_info(&this->hooks, SPA_DIRECTION_OUTPUT, 0,
				&port->info);
		port->info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	emit_node_info(this, true);
	emit_port_info(this, true);
	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_node_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int impl_node_enum_params(void *object, int seq, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod_dynamic_builder dynamic;
	struct spa_pod_builder_state state;
	struct spa_result_node_params result = { 0 };
	uint8_t storage[4096];
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL && num > 0, -EINVAL);
	if (id != SPA_PARAM_PropInfo && id != SPA_PARAM_Props)
		return -ENOENT;
	spa_pod_dynamic_builder_init(&dynamic, storage, sizeof(storage), 4096);
	spa_pod_builder_get_state(&dynamic.b, &state);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_reset(&dynamic.b, &state);
		if (id == SPA_PARAM_PropInfo)
			param = bgapi2_build_feature_prop_info(this->camera,
					result.index, &dynamic.b);
		else if (result.index == 0)
			param = bgapi2_build_feature_props(this->camera, &dynamic.b);
		else
			param = NULL;
		if (param == NULL)
			break;
		if (spa_pod_filter(&dynamic.b, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&this->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	spa_pod_dynamic_builder_clean(&dynamic);
	return 0;
}

static int parse_feature_value(enum bgapi2_feature_kind kind,
		const struct spa_pod *pod, struct bgapi2_feature_value *value)
{
	uint32_t id;

	memset(value, 0, sizeof(*value));
	value->kind = kind;
	switch (kind) {
	case BGAPI2_FEATURE_BOOLEAN:
		return spa_pod_get_bool(pod, &value->boolean);
	case BGAPI2_FEATURE_INTEGER:
		return spa_pod_get_long(pod, &value->integer);
	case BGAPI2_FEATURE_FLOATING:
		return spa_pod_get_double(pod, &value->floating);
	case BGAPI2_FEATURE_ENUMERATION:
		if (spa_pod_get_int(pod, &value->enumeration) == 0)
			return 0;
		if (spa_pod_get_id(pod, &id) < 0 || id > INT32_MAX)
			return -EINVAL;
		value->enumeration = (int32_t)id;
		return 0;
	case BGAPI2_FEATURE_STRING:
		return spa_pod_get_string(pod, &value->string);
	case BGAPI2_FEATURE_COMMAND:
		return -EINVAL;
	}
	return -EINVAL;
}

static int refresh_layout_params(struct impl *this)
{
	const struct bgapi2_camera_info *info;
	uint32_t format, bytes_per_pixel;
	int res;

	if ((res = bgapi2_camera_refresh_info(this->camera)) < 0)
		return res;
	info = bgapi2_camera_get_info(this->camera);
	if (info == NULL || info->width > UINT32_MAX ||
			info->height > UINT32_MAX || info->payload_size > INT32_MAX ||
			info->width > INT32_MAX ||
			map_pixel_format(info->pixel_format, &format, &bytes_per_pixel) < 0 ||
			info->width * bytes_per_pixel > INT32_MAX)
		return -ENOTSUP;
	this->camera_info = *info;
	this->video_format = format;
	this->bytes_per_pixel = bytes_per_pixel;
	this->port.have_format = false;
	this->port.params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(this, false);
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id,
		uint32_t flags SPA_UNUSED, const struct spa_pod *param)
{
	struct impl *this = object;
	struct bgapi2_feature_info info;
	struct bgapi2_feature_value value;
	struct spa_pod_object *object_param;
	struct spa_pod_prop *property;
	const char *name = NULL;
	struct spa_pod *value_pod = NULL;
	uint32_t feature_index, operations = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	if (id != SPA_PARAM_Props)
		return -ENOENT;
	if (param == NULL)
		return 0;
	if (!spa_pod_is_object(param) ||
			SPA_POD_OBJECT_TYPE(param) != SPA_TYPE_OBJECT_Props)
		return -EINVAL;
	object_param = (struct spa_pod_object *)param;
	SPA_POD_OBJECT_FOREACH(object_param, property) {
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;

		if (property->key != SPA_PROP_params)
			continue;
		spa_pod_parser_pod(&parser, &property->value);
		if (spa_pod_parser_push_struct(&parser, &frame) < 0)
			return -EINVAL;
		for (;;) {
			const char *candidate = NULL;
			struct spa_pod *candidate_value = NULL;

			if (spa_pod_parser_get_string(&parser, &candidate) < 0)
				break;
			if (spa_pod_parser_get_pod(&parser, &candidate_value) < 0)
				return -EINVAL;
			if (++operations > 1)
				return -EINVAL;
			name = candidate;
			value_pod = candidate_value;
		}
	}
	if (operations == 0)
		return 0;
	if (this->started)
		return -EBUSY;
	if ((res = bgapi2_camera_find_feature(this->camera, name,
			&feature_index)) < 0 ||
			(res = bgapi2_camera_get_feature_info(this->camera, feature_index,
			&info)) < 0)
		return res;
	if (!info.available)
		return -ENODATA;
	if (!info.writable)
		return -EACCES;
	if (info.changes_layout && this->port.n_buffers != 0)
		return -EBUSY;
	if ((res = parse_feature_value(info.kind, value_pod, &value)) < 0)
		return res;
	if (spa_streq(info.name, "PixelFormat")) {
		const char *entry;
		uint32_t format, bytes_per_pixel;

		if (value.enumeration < 0 ||
				(uint32_t)value.enumeration >= info.n_enum_entries ||
				(entry = bgapi2_camera_get_feature_enum_entry(this->camera,
				feature_index, (uint32_t)value.enumeration)) == NULL)
			return -EINVAL;
		if (map_pixel_format(entry, &format, &bytes_per_pixel) < 0)
			return -ENOTSUP;
	}
	if ((res = bgapi2_camera_set_feature_value(this->camera, feature_index,
			&value)) < 0)
		return res;
	if (info.changes_layout && (res = refresh_layout_params(this)) < 0)
		return res;
	this->params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	emit_node_info(this, false);
	return 0;
}

static int impl_node_set_io(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		void *data SPA_UNUSED, size_t size SPA_UNUSED)
{
	return -ENOTSUP;
}

static int queue_slot(struct impl *this, struct buffer_slot *slot)
{
	int res;

	if ((res = bgapi2_camera_queue(this->camera,
				slot->camera_buffer)) < 0)
		return res;
	slot->camera_queued = true;
	return 0;
}

static int queue_producer_buffers(struct impl *this)
{
	uint32_t i;

	for (i = 0; i < this->port.n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];

		if (slot->camera_queued || slot->image == NULL ||
				spa_image_source_buffer_get_state(slot->image) !=
				SPA_IMAGE_BUFFER_STATE_PRODUCER)
			continue;
		if (queue_slot(this, slot) < 0)
			return -EIO;
	}
	return 0;
}

static int stop_source(struct impl *this)
{
	int first_error = 0, res;
	uint32_t i;

	if (!this->started)
		return 0;
	if ((res = bgapi2_camera_stop(this->camera)) < 0)
		first_error = res;
	if ((res = bgapi2_camera_discard_buffers(this->camera)) < 0 &&
			first_error == 0)
		first_error = res;
	for (i = 0; i < this->port.n_buffers; i++)
		this->slots[i].camera_queued = false;
	this->started = false;
	if ((res = spa_buffer_latest_worker_end(this->latest)) < 0 &&
			first_error == 0)
		first_error = res;
	return first_error;
}

static int impl_node_send_command(void *object,
		const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->port.have_format || this->port.n_buffers == 0 ||
				!spa_buffer_latest_has_links(this->latest))
			return -EIO;
		if (this->started)
			return 0;
		if ((res = queue_producer_buffers(this)) < 0)
			return res;
		if ((res = spa_buffer_latest_worker_begin(this->latest)) < 0)
			return res;
		if ((res = bgapi2_camera_start(this->camera)) < 0) {
			(void) spa_buffer_latest_worker_end(this->latest);
			(void) bgapi2_camera_discard_buffers(this->camera);
			return res;
		}
		this->started = true;
		this->have_sequence = false;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		return stop_source(this);
	default:
		return -ENOTSUP;
	}
}

static int impl_node_add_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED,
		const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int build_port_param(struct impl *this, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	struct port *port = &this->port;
	uint32_t stride = (uint32_t)this->camera_info.width * this->bytes_per_pixel;

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(this->video_format),
				SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(
						this->camera_info.width, this->camera_info.height)),
				SPA_FORMAT_VIDEO_framerate,
				SPA_POD_Fraction(&SPA_FRACTION(0, 1)));
		return 1;
	case SPA_PARAM_Format:
		if (index > 0)
			return 0;
		if (!port->have_format)
			return -EIO;
		*param = spa_format_video_raw_build(builder, id, &port->format);
		return 1;
	case SPA_PARAM_Buffers:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int((int32_t)this->camera_info.payload_size),
				SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int32_t)stride),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return 1;
	case SPA_PARAM_Meta:
		switch (index) {
		case 0:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
			return 1;
		case 1:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
			return 1;
		default:
			return 0;
		}
	case SPA_PARAM_IO:
		switch (index) {
		case 0:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatest),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest)));
			return 1;
		case 1:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id,
					SPA_POD_Id(SPA_IO_BuffersLatestLink),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
			return 1;
		default:
			return 0;
		}
	default:
		return -ENOENT;
	}
}

static int impl_node_port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod_builder builder;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_init(&builder, buffer, sizeof(buffer));
		res = build_port_param(this, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&this->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int impl_node_port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags SPA_UNUSED,
		const struct spa_pod *param)
{
	struct impl *this = object;
	struct spa_video_info_raw format;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (this->started || this->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		this->port.have_format = false;
		return 0;
	}
	if (spa_format_video_raw_parse(param, &format) < 0 ||
			format.format != this->video_format ||
			format.size.width != this->camera_info.width ||
			format.size.height != this->camera_info.height)
		return -EINVAL;
	this->port.format = format;
	this->port.have_format = true;
	return 0;
}

static int release_buffers(struct impl *this)
{
	uint32_t i;
	int first_error = 0, res;

	if (spa_buffer_latest_has_links(this->latest))
		return -EBUSY;
	if ((res = bgapi2_camera_discard_buffers(this->camera)) < 0)
		first_error = res;
	for (i = 0; i < this->port.n_buffers; i++)
		if (this->slots[i].image != NULL)
			spa_image_source_buffer_set_user_data(this->slots[i].image, NULL);
	res = spa_image_source_latest_teardown(&this->transport, &this->source);
	if (res < 0)
		return first_error == 0 ? res : first_error;
	for (i = 0; i < this->port.n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];

		if (slot->camera_buffer != NULL &&
				(res = bgapi2_camera_revoke(this->camera,
					&slot->camera_buffer)) < 0 && first_error == 0)
			first_error = res;
		memset(slot, 0, sizeof(*slot));
	}
	this->port.n_buffers = 0;
	return first_error;
}

static int impl_node_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t flags SPA_UNUSED,
		uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	bool prepared = false;
	uint32_t i;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (this->started)
		return -EBUSY;
	if (n_buffers == 0)
		return this->port.n_buffers == 0 ? 0 : release_buffers(this);
	if (!this->port.have_format || this->port.n_buffers != 0 ||
			buffers == NULL || n_buffers < MIN_BUFFERS ||
			n_buffers > MAX_BUFFERS)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				(data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_MemFd) ||
				data->maxsize < this->camera_info.payload_size ||
				data->chunk == NULL)
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];
		struct spa_data *data = &buffers[i]->datas[0];

		if ((res = bgapi2_camera_announce(this->camera, data->data,
				data->maxsize, slot, &slot->camera_buffer)) < 0 ||
				(res = queue_slot(this, slot)) < 0)
			goto error;
	}
	res = spa_image_source_latest_prepare(&this->transport, &this->source,
			buffers, n_buffers);
	if (res < 0)
		goto error;
	prepared = true;
	this->port.n_buffers = n_buffers;
	for (i = 0; i < n_buffers; i++) {
		struct spa_image_source_buffer *image = NULL;
		struct buffer_slot *slot = &this->slots[i];

		if (spa_image_source_try_acquire(&this->source, &image) != 1) {
			res = -EPROTO;
			goto error;
		}
		slot->image = image;
		spa_image_source_buffer_set_user_data(image, slot);
	}
	return 0;

error:
	(void) bgapi2_camera_discard_buffers(this->camera);
	for (i = 0; i < n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];

		if (slot->camera_buffer != NULL)
			(void) bgapi2_camera_revoke(this->camera, &slot->camera_buffer);
		if (slot->image != NULL)
			spa_image_source_buffer_set_user_data(slot->image, NULL);
		memset(slot, 0, sizeof(*slot));
	}
	if (prepared)
		(void) spa_image_source_latest_teardown(&this->transport, &this->source);
	this->port.n_buffers = 0;
	return res;
}

static int impl_node_port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	return spa_buffer_latest_set_io(this->latest, id, data, size);
}

static int impl_node_port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int recycle_buffers(struct impl *this)
{
	uint32_t count;
	bool changed = false;

	for (count = 0; count < this->port.n_buffers; count++) {
		struct spa_image_source_buffer *image = NULL;
		struct buffer_slot *slot;
		int res = spa_image_source_try_acquire(&this->source, &image);

		if (res < 0)
			return res;
		if (res == 0)
			break;
		slot = spa_image_source_buffer_get_user_data(image);
		if (slot == NULL || slot->image != image || slot->camera_queued)
			return -EPROTO;
		if (queue_slot(this, slot) < 0)
			return -EIO;
		changed = true;
	}
	return changed ? 1 : 0;
}

static int publish_buffer(struct impl *this,
		const struct bgapi2_camera_completion *completion)
{
	const struct bgapi2_frame_info *info = &completion->frame;
	BGAPI2_Buffer *camera_buffer = completion->buffer;
	struct spa_meta_acquisition acquisition;
	struct spa_image_frame frame;
	struct buffer_slot *slot;
	struct spa_buffer *buffer;
	struct spa_data *data;
	uint64_t stride, expected, available, size;
	uint32_t header_flags = 0, chunk_flags = 0;
	int res;

	if (completion->user_data == NULL) {
		(void) bgapi2_camera_queue(this->camera, camera_buffer);
		return completion->result < 0 ? completion->result : -EPROTO;
	}
	slot = completion->user_data;
	if (slot->camera_buffer != camera_buffer || !slot->camera_queued ||
			slot->image == NULL)
		return -EPROTO;
	slot->camera_queued = false;
	if (this->have_sequence && info->frame_id != this->last_sequence + 1)
		header_flags |= SPA_META_HEADER_FLAG_DISCONT;
	this->have_sequence = true;
	this->last_sequence = info->frame_id;
	if (completion->result < 0) {
		res = completion->result;
		goto recycle;
	}
	buffer = spa_image_source_buffer_get_buffer(slot->image);
	data = &buffer->datas[0];
	stride = this->camera_info.width * this->bytes_per_pixel + info->x_padding;
	expected = stride * this->camera_info.height;
	available = info->size_filled > info->image_offset ?
			info->size_filled - info->image_offset : 0;
	size = info->image_length < available ? info->image_length : available;
	if (size > expected)
		size = expected;
	if (info->image_offset > UINT32_MAX || size > UINT32_MAX ||
			stride > INT32_MAX || info->image_offset + size > data->maxsize) {
		res = -ENOSPC;
		goto recycle;
	}
	if (info->incomplete || size != expected)
		chunk_flags |= SPA_CHUNK_FLAG_CORRUPTED;
	spa_meta_acquisition_init(&acquisition);
	frame = (struct spa_image_frame) {
		.version = SPA_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.header_flags = header_flags,
		.chunk_flags = chunk_flags,
		.offset = (uint32_t)info->image_offset,
		.size = (uint32_t)size,
		.stride = (int32_t)stride,
		.sequence = info->frame_id,
		.pts = monotonic_nsec(),
		.acquisition = &acquisition,
	};
	res = spa_image_source_publish_complete(&this->source, slot->image, &frame);
	if (res >= 0)
		return 1;

recycle:
	(void) queue_slot(this, slot);
	return res;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct bgapi2_camera_completion completion;
	bool changed = false;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	if (!this->started)
		return SPA_STATUS_OK;
	res = bgapi2_camera_try_get_completion(this->camera, &completion);
	if (res < 0)
		return res;
	if (res == 1) {
		res = publish_buffer(this, &completion);
		if (res < 0)
			return res;
		changed = true;
	}
	/* QueueBuffer is producer-controlled and may allocate. Return released
	 * leases only after observing and publishing data that is already ready so
	 * requeue jitter consumes pool headroom instead of preceding completion. */
	res = recycle_buffers(this);
	if (res < 0)
		return res;
	changed = changed || res > 0;
	return changed ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &this->node;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *)handle;
	int first_error = 0, res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	if ((res = stop_source(this)) < 0)
		first_error = res;
	if (this->port.n_buffers != 0 &&
			(res = release_buffers(this)) < 0 && first_error == 0)
		first_error = res;
	spa_buffer_latest_destroy(this->latest);
	this->latest = NULL;
	bgapi2_camera_close(this->camera);
	this->camera = NULL;
	return first_error;
}

static size_t impl_get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static void configure_props(struct impl *this,
		const struct bgapi2_camera_options *options)
{
	uint32_t n_items = 0;

	snprintf(this->node_name, sizeof(this->node_name), "bgapi2_source.%s",
			this->camera_info.serial);
	snprintf(this->node_description, sizeof(this->node_description), "%s %s",
			this->camera_info.model, this->camera_info.serial);
	snprintf(this->producer_path, sizeof(this->producer_path), "%s",
			options->producer_path);
	snprintf(this->interface_index, sizeof(this->interface_index), "%u",
			this->camera_info.interface_index);
	snprintf(this->device_index, sizeof(this->device_index), "%u",
			this->camera_info.device_index);
	snprintf(this->stream_index, sizeof(this->stream_index), "%u",
			this->camera_info.stream_index);
#define ADD_ITEM(key, value) \
	this->prop_items[n_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "bgapi2");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, this->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, this->node_description);
	ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, this->camera_info.model);
	ADD_ITEM(SPA_KEY_DEVICE_SERIAL, this->camera_info.serial);
	ADD_ITEM(SPA_KEY_API_BGAPI2_PRODUCER, this->producer_path);
	ADD_ITEM(SPA_KEY_API_BGAPI2_INTERFACE_INDEX, this->interface_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_DEVICE_INDEX, this->device_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_STREAM_INDEX, this->stream_index);
#undef ADD_ITEM
	this->props = SPA_DICT_INIT(this->prop_items, n_items);
}

static int impl_init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *this = (struct impl *)handle;
	struct bgapi2_camera_options options = {
		.interface_index = BGAPI2_CAMERA_ANY_INTERFACE,
		.interface_timeout_ms = 100,
		.device_timeout_ms = 200,
	};
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = MIN_BUFFERS,
		.max_buffers = MAX_BUFFERS,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION,
	};
	const struct bgapi2_camera_info *camera_info;
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(this, 0, sizeof(*this));
	options.producer_path = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_BGAPI2_PRODUCER);
	options.completion_mode = BGAPI2_CAMERA_COMPLETION_CALLBACK;
	if (options.producer_path == NULL ||
			parse_index(info, SPA_KEY_API_BGAPI2_INTERFACE_INDEX,
				BGAPI2_CAMERA_ANY_INTERFACE, &options.interface_index) < 0 ||
			parse_index(info, SPA_KEY_API_BGAPI2_DEVICE_INDEX, 0,
				&options.device_index) < 0 ||
			parse_index(info, SPA_KEY_API_BGAPI2_STREAM_INDEX, 0,
				&options.stream_index) < 0)
		return -EINVAL;
	this->handle.get_interface = impl_get_interface;
	this->handle.clear = impl_clear;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_hook_list_init(&this->hooks);
	if ((res = bgapi2_camera_open(&this->camera, &options)) < 0)
		return res;
	camera_info = bgapi2_camera_get_info(this->camera);
	if (camera_info == NULL || camera_info->width > UINT32_MAX ||
			camera_info->height > UINT32_MAX ||
			camera_info->payload_size > INT32_MAX ||
			camera_info->width > INT32_MAX ||
			map_pixel_format(camera_info->pixel_format, &this->video_format,
				&this->bytes_per_pixel) < 0 ||
			camera_info->width * this->bytes_per_pixel > INT32_MAX) {
		bgapi2_camera_close(this->camera);
		this->camera = NULL;
		return -ENOTSUP;
	}
	this->camera_info = *camera_info;
	this->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &impl_node, this);
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_output_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_RTC_PROCESS;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_PropInfo,
			SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props,
			SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = SPA_N_ELEMENTS(this->params);
	configure_props(this, &options);
	this->info.props = &this->props;
	this->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	this->port.info = SPA_PORT_INFO_INIT();
	this->port.info.flags = SPA_PORT_FLAG_LIVE;
	this->port.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	this->port.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	this->port.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	this->port.params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta,
			SPA_PARAM_INFO_READ);
	this->port.params[4] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	this->port.info.params = this->port.params;
	this->port.info.n_params = SPA_N_ELEMENTS(this->port.params);
	this->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, this, this->log);
	if (this->latest == NULL) {
		res = -errno;
		goto error;
	}
	res = spa_image_source_latest_init(&this->transport, &this->source,
			this->latest, &config);
	if (res < 0) {
		spa_buffer_latest_destroy(this->latest);
		this->latest = NULL;
		goto error;
	}
	return 0;

error:
	bgapi2_camera_close(this->camera);
	this->camera = NULL;
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int impl_enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;
	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_bgapi2_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BGAPI2_SOURCE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

/* SPDX-License-Identifier: MIT */
#include <errno.h>
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
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/calculon.h>
#include <pipewireao-plugins/pod.h>

#include "../image-frame.h"
#include "aravis.h"
#include "camera.h"
#include "params.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS 64u

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
	ArvBuffer *camera_buffer;
	void *camera_memory;
	uint32_t id;
	bool camera_queued;
	bool published;
	bool row_initialized;
	bool row_terminal_ready;
	bool row_terminal_valid;
	bool row_drop;
	bool row_discontinuity;
	uint32_t next_row;
	uint64_t frame_id;
	uint64_t committed_size;
	int64_t terminal_pts;
};

struct row_output {
	struct spa_buffer *buffer;
	uint32_t id;
	bool published;
};

enum output_mode {
	OUTPUT_MODE_FRAME,
	OUTPUT_MODE_ROW_BLOCK,
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
	struct spa_dict_item prop_items[16];
	char node_name[192];
	char node_description[256];
	char device_id[256];
	char transport_name[16];
	char output_mode_name[16];
	char row_block_rows_text[16];
	char detector_profile[256];
	struct port port;
	struct aravis_camera *camera;
	struct aravis_camera_info camera_info;
	struct buffer_slot slots[MAX_BUFFERS];
	struct row_output row_outputs[MAX_BUFFERS];
	struct buffer_slot *row_submissions[MAX_BUFFERS];
	uint32_t row_submission_head;
	uint32_t row_submission_size;
	struct buffer_slot *row_slot;
	enum aravis_transport transport;
	enum output_mode output_mode;
	uint32_t row_block_rows;
	uint32_t video_format;
	uint32_t bytes_per_pixel;
	struct spa_fraction frame_rate;
	bool layout_valid;
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

static int map_pixel_format(
		const char *name, uint32_t *format, uint32_t *bytes_per_pixel)
{
	if (spa_streq(name, "Mono8")) {
		*format = SPA_VIDEO_FORMAT_GRAY8;
		*bytes_per_pixel = 1;
		return 0;
	}
	if (spa_streq(name, "Mono10") || spa_streq(name, "Mono12") ||
			spa_streq(name, "Mono14") ||
			spa_streq(name, "Mono16")) {
		*format = SPA_VIDEO_FORMAT_GRAY16_LE;
		*bytes_per_pixel = 2;
		return 0;
	}
	return -ENOTSUP;
}

static int parse_transport(const char *value, enum aravis_transport *transport)
{
	if (value == NULL || spa_streq(value, "auto"))
		*transport = ARAVIS_TRANSPORT_AUTO;
	else if (spa_streq(value, "gentl"))
		*transport = ARAVIS_TRANSPORT_GENTL;
	else if (spa_streq(value, "native-gv"))
		*transport = ARAVIS_TRANSPORT_NATIVE_GV;
	else
		return -EINVAL;
	return 0;
}

static const char *transport_name(enum aravis_transport transport)
{
	switch (transport) {
	case ARAVIS_TRANSPORT_GENTL:
		return "gentl";
	case ARAVIS_TRANSPORT_NATIVE_GV:
		return "native-gv";
	case ARAVIS_TRANSPORT_AUTO:
		return "auto";
	}
	return "auto";
}

static int parse_output_mode(const char *value, enum output_mode *mode)
{
	if (value == NULL || spa_streq(value, "frame"))
		*mode = OUTPUT_MODE_FRAME;
	else if (spa_streq(value, "row-block"))
		*mode = OUTPUT_MODE_ROW_BLOCK;
	else
		return -EINVAL;
	return 0;
}

static int parse_positive_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (text == NULL || text[0] == '\0' || text[0] == '-')
		return -EINVAL;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
			parsed > UINT32_MAX)
		return -EINVAL;
	*value = (uint32_t)parsed;
	return 0;
}

static uint32_t row_element_type(const struct impl *this)
{
	return this->video_format == SPA_VIDEO_FORMAT_GRAY8
			? SPA_ELEMENT_TYPE_U8
			: SPA_ELEMENT_TYPE_U16_LE;
}

static uint32_t greatest_common_divisor(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t remainder = a % b;
		a = b;
		b = remainder;
	}
	return a;
}

static int make_frame_rate(double value, struct spa_fraction *rate)
{
	const uint32_t denominator = 1000;
	uint32_t numerator, divisor;

	if (value <= 0.0 || value > (double)UINT32_MAX / denominator)
		return -ERANGE;
	numerator = (uint32_t)(value * denominator + 0.5);
	if (numerator == 0)
		return -ERANGE;
	divisor = greatest_common_divisor(numerator, denominator);
	*rate = SPA_FRACTION(numerator / divisor, denominator / divisor);
	return 0;
}

static struct spa_fraction output_rate(const struct impl *this)
{
	uint64_t numerator = this->frame_rate.num;

	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK)
		numerator *= this->camera_info.height / this->row_block_rows;
	if (numerator > UINT32_MAX)
		return SPA_FRACTION(0, 1);
	return SPA_FRACTION((uint32_t)numerator, this->frame_rate.denom);
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
			param = aravis_build_feature_prop_info(
					this->camera, result.index, &dynamic.b);
		else if (result.index == 0)
			param = aravis_build_feature_props(
					this->camera, &dynamic.b);
		else
			param = NULL;
		if (param == NULL)
			break;
		if (spa_pod_filter(&dynamic.b, &result.param, param, filter) <
				0)
			continue;
		spa_node_emit_result(&this->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	spa_pod_dynamic_builder_clean(&dynamic);
	return 0;
}

static int parse_feature_value(enum aravis_feature_kind kind,
		const struct spa_pod *pod, struct aravis_feature_value *value)
{
	uint32_t id;

	memset(value, 0, sizeof(*value));
	value->kind = kind;
	switch (kind) {
	case ARAVIS_FEATURE_BOOLEAN:
		return spa_pod_get_bool(pod, &value->boolean);
	case ARAVIS_FEATURE_INTEGER:
		return spa_pod_get_long(pod, &value->integer);
	case ARAVIS_FEATURE_FLOATING:
		return spa_pod_get_double(pod, &value->floating);
	case ARAVIS_FEATURE_ENUMERATION:
		if (spa_pod_get_int(pod, &value->enumeration) == 0)
			return 0;
		if (spa_pod_get_id(pod, &id) < 0 || id > INT32_MAX)
			return -EINVAL;
		value->enumeration = (int32_t)id;
		return 0;
	case ARAVIS_FEATURE_STRING:
		return spa_pod_get_string(pod, &value->string);
	case ARAVIS_FEATURE_COMMAND:
		return -EINVAL;
	}
	return -EINVAL;
}

static int read_layout(struct impl *this, struct aravis_camera_info *candidate,
		uint32_t *format, uint32_t *bytes_per_pixel)
{
	const struct aravis_camera_info *info;
	int res;

	if ((res = aravis_camera_refresh_info(this->camera)) < 0)
		return res;
	info = aravis_camera_get_info(this->camera);
	if (info == NULL || info->payload_size > INT32_MAX ||
			map_pixel_format(info->pixel_format, format,
					bytes_per_pixel) < 0 ||
			info->width > INT32_MAX / (*bytes_per_pixel))
		return -ENOTSUP;
	*candidate = *info;
	return 0;
}

static int refresh_layout_params(struct impl *this)
{
	struct aravis_camera_info candidate;
	struct spa_fraction frame_rate;
	uint32_t format, bytes_per_pixel;
	int res;

	if ((res = read_layout(this, &candidate, &format, &bytes_per_pixel)) <
			0)
		return res;
	if (make_frame_rate(candidate.frame_rate, &frame_rate) < 0)
		frame_rate = SPA_FRACTION(0, 1);
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK &&
			(this->row_block_rows >= candidate.height ||
					candidate.height % this->row_block_rows !=
							0 ||
					candidate.payload_size !=
							(uint64_t)candidate.width *
									candidate.height *
									bytes_per_pixel ||
					frame_rate.num == 0))
		return -ENOTSUP;
	this->camera_info = candidate;
	this->video_format = format;
	this->bytes_per_pixel = bytes_per_pixel;
	this->frame_rate = frame_rate;
	this->layout_valid = true;
	this->port.have_format = false;
	this->port.params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(this, false);
	return 0;
}

static bool layout_matches(const struct aravis_camera_info *a,
		const struct aravis_camera_info *b)
{
	return a->payload_size == b->payload_size && a->width == b->width &&
			a->height == b->height && a->offset_x == b->offset_x &&
			a->offset_y == b->offset_y &&
			a->frame_rate == b->frame_rate &&
			spa_streq(a->pixel_format, b->pixel_format);
}

static void invalidate_layout_params(struct impl *this)
{
	this->layout_valid = false;
	this->port.have_format = false;
	this->port.params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(this, false);
}

static int restore_layout_feature(struct impl *this, uint32_t feature_index,
		const struct aravis_feature_value *old_value)
{
	struct aravis_camera_info restored;
	uint32_t format, bytes_per_pixel;
	int res;

	if ((res = aravis_camera_set_feature_value(
			     this->camera, feature_index, old_value)) < 0 ||
			(res = read_layout(this, &restored, &format,
					 &bytes_per_pixel)) < 0)
		return res;
	return format == this->video_format &&
					bytes_per_pixel ==
							this->bytes_per_pixel &&
					layout_matches(&restored,
							&this->camera_info)
			? 0
			: -EIO;
}

static int impl_node_set_param(void *object, uint32_t id,
		uint32_t flags SPA_UNUSED, const struct spa_pod *param)
{
	struct impl *this = object;
	struct aravis_feature_info info;
	struct aravis_feature_value value, old_value;
	struct spa_pod_object *object_param;
	struct spa_pod_prop *property;
	const char *name = NULL;
	struct spa_pod *value_pod = NULL;
	uint32_t feature_index, operations = 0;
	char *old_string = NULL;
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
	SPA_POD_OBJECT_FOREACH(object_param, property)
	{
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
			if (spa_pod_parser_get_pod(&parser, &candidate_value) <
					0)
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
	if ((res = aravis_camera_find_feature(
			     this->camera, name, &feature_index)) < 0 ||
			(res = aravis_camera_get_feature_info(this->camera,
					 feature_index, &info)) < 0)
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
				(uint32_t)value.enumeration >=
						info.n_enum_entries ||
				(entry = aravis_camera_get_feature_enum_entry(
						 this->camera, feature_index,
						 (uint32_t)value.enumeration)) ==
						NULL)
			return -EINVAL;
		if (map_pixel_format(entry, &format, &bytes_per_pixel) < 0)
			return -ENOTSUP;
	}
	if (info.changes_layout) {
		if (!info.readable ||
				(res = aravis_camera_get_feature_value(
						 this->camera, feature_index,
						 &old_value)) < 0)
			return !info.readable ? -EACCES : res;
		if (old_value.kind == ARAVIS_FEATURE_STRING) {
			if (old_value.string == NULL ||
					(old_string = strdup(
							 old_value.string)) ==
							NULL)
				return old_value.string == NULL ? -EINVAL
								: -errno;
			old_value.string = old_string;
		}
	}
	if ((res = aravis_camera_set_feature_value(
			     this->camera, feature_index, &value)) < 0)
		goto done;
	if (info.changes_layout && (res = refresh_layout_params(this)) < 0) {
		int refresh_error = res;

		if (restore_layout_feature(this, feature_index, &old_value) <
				0) {
			spa_log_error(this->log,
					"could not roll back layout feature %s",
					info.name);
			invalidate_layout_params(this);
			res = -EIO;
		} else {
			res = refresh_error;
		}
		goto done;
	}
	this->params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	emit_node_info(this, false);
	res = 0;

done:
	free(old_string);
	return res;
}

static int impl_node_set_io(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		void *data SPA_UNUSED, size_t size SPA_UNUSED)
{
	return -ENOTSUP;
}

static int queue_slot(struct impl *this, struct buffer_slot *slot)
{
	int res;
	uint32_t tail;

	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK &&
			this->row_submission_size == MAX_BUFFERS)
		return -ENOSPC;

	if ((res = aravis_camera_queue(this->camera, slot->camera_buffer)) < 0)
		return res;
	slot->camera_queued = true;
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
		tail = (this->row_submission_head + this->row_submission_size) %
				MAX_BUFFERS;
		this->row_submissions[tail] = slot;
		this->row_submission_size++;
	}
	return 0;
}

static struct buffer_slot *row_submission_front(struct impl *this)
{
	return this->row_submission_size == 0
			? NULL
			: this->row_submissions[this->row_submission_head];
}

static int row_submission_remove(struct impl *this, struct buffer_slot *slot)
{
	if (row_submission_front(this) != slot)
		return -EPROTO;
	this->row_submissions[this->row_submission_head] = NULL;
	this->row_submission_head =
			(this->row_submission_head + 1) % MAX_BUFFERS;
	this->row_submission_size--;
	if (this->row_submission_size == 0)
		this->row_submission_head = 0;
	return 0;
}

static int queue_producer_buffers(struct impl *this)
{
	uint32_t i;

	for (i = 0; i < this->port.n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];

		if (slot->camera_queued || slot->published ||
				slot->camera_buffer == NULL)
			continue;
		if (queue_slot(this, slot) < 0)
			return -EIO;
	}
	return 0;
}

static int stop_source(struct impl *this)
{
	int res;
	uint32_t i;

	if (!this->started)
		return 0;
	if ((res = aravis_camera_stop(this->camera)) < 0)
		return res;
	for (i = 0; i < this->port.n_buffers; i++) {
		this->slots[i].camera_queued = false;
		this->slots[i].row_initialized = false;
		this->slots[i].row_terminal_ready = false;
		this->slots[i].row_terminal_valid = false;
		this->slots[i].row_drop = false;
		this->slots[i].row_discontinuity = false;
		this->slots[i].next_row = 0;
		this->slots[i].frame_id = 0;
		this->slots[i].committed_size = 0;
		this->slots[i].terminal_pts = SPA_TIME_INVALID;
	}
	memset(this->row_submissions, 0, sizeof(this->row_submissions));
	this->row_submission_head = 0;
	this->row_submission_size = 0;
	this->row_slot = NULL;
	this->started = false;
	return 0;
}

static int impl_node_send_command(
		void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->port.have_format || this->port.n_buffers == 0 ||
				this->port.io == NULL)
			return -EIO;
		if (this->started)
			return 0;
		if ((res = queue_producer_buffers(this)) < 0)
			return res;
		if ((res = aravis_camera_start(this->camera)) < 0)
			return res;
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
		enum spa_direction direction SPA_UNUSED,
		uint32_t port_id SPA_UNUSED,
		const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED,
		uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static struct spa_pod *build_row_format(
		struct impl *this, uint32_t id, struct spa_pod_builder *builder)
{
	int32_t shape[2] = {
		(int32_t)this->row_block_rows,
		(int32_t)this->camera_info.width,
	};
	const struct spa_fraction rate = output_rate(this);

	return spa_pod_builder_add_object(builder, SPA_TYPE_OBJECT_Format, id,
			SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String(SPA_CALCULON_SCHEMA_RAW_PIXEL_ROW_BLOCK),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(row_element_type(this)),
			SPA_FORMAT_NDARRAY_shape,
			SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int, 2, shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate),
			SPA_FORMAT_NDARRAY_profile,
			SPA_POD_String(this->detector_profile));
}

static int build_port_param(struct impl *this, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	struct port *port = &this->port;
	uint32_t stride = this->camera_info.width * this->bytes_per_pixel;

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		if (!this->layout_valid)
			return -EIO;
		if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
			*param = build_row_format(this, id, builder);
			return 1;
		}
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType,
				SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype,
				SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format,
				SPA_POD_Id(this->video_format),
				SPA_FORMAT_VIDEO_size,
				SPA_POD_Rectangle(&SPA_RECTANGLE(
						this->camera_info.width,
						this->camera_info.height)),
				SPA_FORMAT_VIDEO_framerate,
				SPA_POD_Fraction(&this->frame_rate));
		return 1;
	case SPA_PARAM_Format:
		if (index > 0)
			return 0;
		if (!port->have_format)
			return -EIO;
		if (this->output_mode == OUTPUT_MODE_FRAME) {
			*param = spa_format_video_raw_build(
					builder, id, &port->format);
			return 1;
		}
		*param = build_row_format(this, id, builder);
		return 1;
	case SPA_PARAM_Buffers:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(
						8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int((int32_t)(this->output_mode == OUTPUT_MODE_ROW_BLOCK
								? (uint64_t)stride *
										this->row_block_rows
								: this->camera_info
										  .payload_size)),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int((int32_t)stride),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int(
						(1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return 1;
	case SPA_PARAM_Meta:
		switch (index) {
		case 0:
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct
							spa_meta_header)));
			return 1;
		case 1: {
			struct spa_pod_frame object;
			spa_pod_builder_push_object(builder, &object,
					SPA_TYPE_OBJECT_ParamMeta, id);
			spa_pod_builder_add(builder, SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct
							spa_meta_acquisition)),
					0);
			spa_pod_builder_prop(builder, SPA_PARAM_META_features,
					SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_int(builder,
					SPA_META_FEATURE_ACQUISITION_CURRENT);
			*param = spa_pod_builder_pop(builder, &object);
			return 1;
		}
		default:
			return 0;
		}
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id, SPA_PARAM_IO_id,
				SPA_POD_Id(SPA_IO_Buffers), SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers)));
		return 1;
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
	spa_return_val_if_fail(
			direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_init(&builder, buffer, sizeof(buffer));
		res = build_port_param(
				this, id, result.index, &builder, &param);
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

static int validate_row_format(struct impl *this, const struct spa_pod *param)
{
	uint8_t storage[2048];
	struct spa_pod_builder builder =
			SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	const struct spa_pod *fixed =
			pipewireao_pod_unwrap_fixed_choices(&builder, param);
	struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();
	const struct spa_pod_prop *property;
	const char *schema = NULL, *profile = NULL;
	const struct spa_fraction rate = output_rate(this);

	if (fixed == NULL || spa_format_ndarray_parse(fixed, &format) < 0 ||
			format.element_type !=
					(enum spa_element_type)row_element_type(
							this) ||
			format.layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
			format.rate.num != rate.num ||
			format.rate.denom != rate.denom ||
			format.n_dimensions != 2 ||
			format.shape[0] != this->row_block_rows ||
			format.shape[1] != this->camera_info.width ||
			spa_ndarray_format_key_count(fixed,
					SPA_FORMAT_NDARRAY_schema) != 1 ||
			spa_ndarray_format_key_count(
					fixed, SPA_FORMAT_NDARRAY_profile) != 1)
		return -EINVAL;
	property = spa_pod_find_prop(fixed, NULL, SPA_FORMAT_NDARRAY_schema);
	if (property == NULL ||
			spa_pod_get_string(&property->value, &schema) < 0 ||
			!spa_streq(schema,
					SPA_CALCULON_SCHEMA_RAW_PIXEL_ROW_BLOCK))
		return -EINVAL;
	property = spa_pod_find_prop(fixed, NULL, SPA_FORMAT_NDARRAY_profile);
	if (property == NULL ||
			spa_pod_get_string(&property->value, &profile) < 0 ||
			!spa_streq(profile, this->detector_profile))
		return -EINVAL;
	return 0;
}

static int impl_node_port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags SPA_UNUSED,
		const struct spa_pod *param)
{
	struct impl *this = object;
	struct spa_video_info_raw format;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(
			direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (this->started || this->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		this->port.have_format = false;
		return 0;
	}
	if (!this->layout_valid)
		return -EIO;
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
		if (validate_row_format(this, param) < 0)
			return -EINVAL;
		this->port.have_format = true;
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

	for (i = 0; i < this->port.n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];

		if (slot->camera_buffer != NULL) {
			if (this->camera == NULL) {
				g_clear_object(&slot->camera_buffer);
			} else if ((res = aravis_camera_revoke(this->camera,
						   &slot->camera_buffer)) < 0 &&
					first_error == 0) {
				first_error = res;
			}
		}
		free(slot->camera_memory);
		memset(slot, 0, sizeof(*slot));
		memset(&this->row_outputs[i], 0, sizeof(this->row_outputs[i]));
	}
	this->row_slot = NULL;
	memset(this->row_submissions, 0, sizeof(this->row_submissions));
	this->row_submission_head = 0;
	this->row_submission_size = 0;
	this->port.n_buffers = 0;
	return first_error;
}

static int impl_node_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t flags SPA_UNUSED,
		uint32_t port_id, struct spa_buffer **buffers,
		uint32_t n_buffers)
{
	struct impl *this = object;
	uint32_t announced = 0, i;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(
			direction == SPA_DIRECTION_OUTPUT && port_id == 0,
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
		uint64_t required = this->output_mode == OUTPUT_MODE_ROW_BLOCK
				? (uint64_t)this->camera_info.width *
						this->bytes_per_pixel *
						this->row_block_rows
				: this->camera_info.payload_size;

		if (buffers[i] == NULL || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				(data->type != SPA_DATA_MemPtr &&
						data->type != SPA_DATA_MemFd) ||
				data->maxsize < required || data->chunk == NULL)
			return -EINVAL;
		if (spa_buffer_find_meta_data(buffers[i], SPA_META_Header,
				    sizeof(struct spa_meta_header)) == NULL ||
				spa_buffer_find_meta_data(buffers[i],
						SPA_META_Acquisition,
						sizeof(struct spa_meta_acquisition)) ==
						NULL)
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer_slot *slot = &this->slots[i];
		struct spa_data *data = &buffers[i]->datas[0];

		slot->id = i;
		if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
			this->row_outputs[i] = (struct row_output){
				.buffer = buffers[i],
				.id = i,
			};
			slot->camera_memory = malloc(
					(size_t)this->camera_info.payload_size);
			if (slot->camera_memory == NULL) {
				res = -errno;
				goto error;
			}
			if ((res = aravis_camera_announce(this->camera,
					     slot->camera_memory,
					     this->camera_info.payload_size,
					     slot, &slot->camera_buffer)) < 0)
				goto error;
		} else {
			slot->buffer = buffers[i];
			if ((res = aravis_camera_announce(this->camera,
					     data->data, data->maxsize, slot,
					     &slot->camera_buffer)) < 0)
				goto error;
		}
		announced++;
	}
	this->port.n_buffers = n_buffers;
	return 0;

error:
	for (i = 0; i < announced; i++)
		if (this->slots[i].camera_buffer != NULL)
			(void)aravis_camera_revoke(this->camera,
					&this->slots[i].camera_buffer);
	for (i = 0; i < n_buffers; i++) {
		free(this->slots[i].camera_memory);
		memset(&this->slots[i], 0, sizeof(this->slots[i]));
		memset(&this->row_outputs[i], 0, sizeof(this->row_outputs[i]));
	}
	this->port.n_buffers = 0;
	return res;
}

static int impl_node_port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(
			direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_Buffers)
		return -ENOENT;
	if (data != NULL && size < sizeof(struct spa_io_buffers))
		return -EINVAL;
	this->port.io = data;
	return 0;
}

static int impl_node_port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int recycle_buffer(struct impl *this)
{
	struct spa_io_buffers *io = this->port.io;
	struct buffer_slot *slot;
	uint32_t id;

	if (io == NULL || io->status == SPA_STATUS_HAVE_DATA ||
			io->buffer_id == SPA_ID_INVALID)
		return 0;
	id = io->buffer_id;
	if (id >= this->port.n_buffers)
		return -EPROTO;
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
		struct row_output *output = &this->row_outputs[id];

		if (!output->published || output->buffer == NULL)
			return -EPROTO;
		io->buffer_id = SPA_ID_INVALID;
		output->published = false;
		return 1;
	}
	slot = &this->slots[id];
	if (!slot->published || slot->camera_queued || slot->buffer == NULL)
		return -EPROTO;
	io->buffer_id = SPA_ID_INVALID;
	slot->published = false;
	return queue_slot(this, slot) < 0 ? -EIO : 1;
}

static void initialize_row_slot(
		struct impl *this, struct buffer_slot *slot, uint64_t frame_id)
{
	slot->row_initialized = true;
	slot->frame_id = frame_id;
	slot->next_row = 0;
	slot->committed_size = 0;
	slot->row_discontinuity = this->have_sequence &&
			frame_id != this->last_sequence + 1;
	this->have_sequence = true;
	this->last_sequence = frame_id;
	if (this->row_slot == NULL)
		this->row_slot = slot;
}

static int finish_row_slot(struct impl *this, struct buffer_slot *slot)
{
	bool dropped = slot->row_drop || !slot->row_terminal_valid;
	int res;

	if ((res = row_submission_remove(this, slot)) < 0)
		return res;

	slot->row_initialized = false;
	slot->row_terminal_ready = false;
	slot->row_terminal_valid = false;
	slot->row_drop = false;
	slot->row_discontinuity = false;
	slot->next_row = 0;
	slot->frame_id = 0;
	slot->committed_size = 0;
	slot->terminal_pts = SPA_TIME_INVALID;
	if (this->row_slot == slot)
		this->row_slot = NULL;
	if (dropped)
		this->discontinuity = true;
	return queue_slot(this, slot);
}

static int record_row_completion(struct impl *this,
		const struct aravis_camera_completion *completion)
{
	const struct aravis_frame_info *info = &completion->frame;
	struct buffer_slot *slot = completion->user_data;
	uint64_t stride = (uint64_t)this->camera_info.width *
			this->bytes_per_pixel;
	uint64_t expected = stride * this->camera_info.height;

	if (slot == NULL || slot->camera_buffer != completion->buffer ||
			!slot->camera_queued)
		return -EPROTO;
	slot->camera_queued = false;
	if (slot->row_initialized && slot->frame_id != info->frame_id)
		return -EPROTO;
	if (!slot->row_initialized)
		slot->frame_id = info->frame_id;
	slot->row_terminal_ready = true;
	slot->row_terminal_valid = completion->result >= 0 &&
			!info->incomplete &&
			info->width == this->camera_info.width &&
			info->height == this->camera_info.height &&
			info->x_padding == 0 && info->y_padding == 0 &&
			info->image_offset == 0 &&
			info->image_size == expected &&
			info->size_filled >= expected;
	slot->terminal_pts = monotonic_nsec();
	if (slot->row_terminal_valid)
		slot->committed_size = expected;
	return 0;
}

static void observe_row_progress(struct impl *this)
{
	struct buffer_slot *slot = row_submission_front(this);

	if (slot == NULL)
		return;
	if (slot->row_terminal_ready) {
		if (!slot->row_initialized)
			initialize_row_slot(this, slot, slot->frame_id);
	} else if (slot->camera_queued) {
		struct aravis_buffer_progress progress;

		if (aravis_camera_get_buffer_progress(this->camera,
						slot->camera_buffer,
						&progress) < 0 ||
				!progress.active || !progress.supported)
			return;
		if (!slot->row_initialized)
			initialize_row_slot(this, slot, progress.frame_id);
		if (slot->frame_id == progress.frame_id &&
				progress.committed_size > slot->committed_size)
			slot->committed_size = progress.committed_size;
	}
}

static struct row_output *take_row_output(struct impl *this)
{
	uint32_t i;

	for (i = 0; i < this->port.n_buffers; i++)
		if (!this->row_outputs[i].published &&
				this->row_outputs[i].buffer != NULL)
			return &this->row_outputs[i];
	return NULL;
}

static int publish_row_block(struct impl *this)
{
	struct buffer_slot *slot = this->row_slot;
	struct row_output *output;
	struct spa_meta_acquisition acquisition;
	struct pwao_image_frame frame;
	struct spa_data *data;
	uint64_t pitch, bytes, available_rows, end_row;
	uint32_t header_flags = 0;
	int res;

	if (slot == NULL)
		return 0;
	if (slot->row_drop) {
		if (slot->row_terminal_ready)
			return finish_row_slot(this, slot) < 0 ? -EIO : 0;
		return 0;
	}
	if (slot->row_terminal_ready && !slot->row_terminal_valid) {
		return finish_row_slot(this, slot) < 0 ? -EIO : 0;
	}
	if (this->port.io == NULL ||
			this->port.io->status == SPA_STATUS_HAVE_DATA)
		return 0;
	pitch = (uint64_t)this->camera_info.width * this->bytes_per_pixel;
	available_rows = slot->committed_size / pitch;
	if (available_rows > this->camera_info.height)
		available_rows = this->camera_info.height;
	available_rows = available_rows / this->row_block_rows *
			this->row_block_rows;
	end_row = (uint64_t)slot->next_row + this->row_block_rows;
	if (end_row > available_rows ||
			(end_row == this->camera_info.height &&
					!slot->row_terminal_ready))
		return 0;
	output = take_row_output(this);
	if (output == NULL) {
		slot->row_drop = true;
		this->discontinuity = true;
		return 0;
	}
	data = &output->buffer->datas[0];
	bytes = pitch * this->row_block_rows;
	if (data->data == NULL || data->chunk == NULL || bytes > data->maxsize)
		return -ENOSPC;
	memcpy(data->data,
			(const uint8_t *)slot->camera_memory +
					(uint64_t)slot->next_row * pitch,
			(size_t)bytes);
	if (end_row == this->camera_info.height)
		header_flags |= SPA_META_HEADER_FLAG_MARKER;
	if (slot->next_row == 0 &&
			(this->discontinuity || slot->row_discontinuity))
		header_flags |= SPA_META_HEADER_FLAG_DISCONT;
	spa_meta_acquisition_init(&acquisition);
	frame = (struct pwao_image_frame){
		.data_index = 0,
		.header_flags = header_flags,
		.chunk_flags = 0,
		.offset = 0,
		.size = (uint32_t)bytes,
		.stride = (int32_t)pitch,
		.header_offset = slot->next_row,
		.sequence = slot->frame_id,
		.pts = slot->row_terminal_ready ? slot->terminal_pts
						: SPA_TIME_INVALID,
		.acquisition = &acquisition,
	};
	res = pwao_image_frame_write(output->buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER |
					PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION);
	if (res < 0)
		return res;
	this->port.io->buffer_id = output->id;
	this->port.io->status = SPA_STATUS_HAVE_DATA;
	output->published = true;
	slot->next_row = (uint32_t)end_row;
	this->discontinuity = false;
	if (end_row == this->camera_info.height &&
			finish_row_slot(this, slot) < 0)
		return -EIO;
	return 1;
}

static int publish_buffer(struct impl *this,
		const struct aravis_camera_completion *completion)
{
	const struct aravis_frame_info *info = &completion->frame;
	struct spa_meta_acquisition acquisition;
	struct pwao_image_frame frame;
	struct buffer_slot *slot;
	struct spa_buffer *buffer;
	struct spa_data *data;
	uint64_t stride, expected, available, size;
	uint32_t header_flags = this->discontinuity
			? SPA_META_HEADER_FLAG_DISCONT
			: 0,
		 chunk_flags = 0;
	int res;

	if (completion->user_data == NULL) {
		(void)aravis_camera_queue(this->camera, completion->buffer);
		return -EPROTO;
	}
	slot = completion->user_data;
	if (slot->camera_buffer != completion->buffer || !slot->camera_queued ||
			slot->buffer == NULL || slot->published)
		return -EPROTO;
	slot->camera_queued = false;
	if (this->have_sequence && info->frame_id != this->last_sequence + 1)
		header_flags |= SPA_META_HEADER_FLAG_DISCONT;
	this->have_sequence = true;
	this->last_sequence = info->frame_id;
	if (completion->result < 0) {
		this->discontinuity = true;
		res = 0;
		goto recycle;
	}
	buffer = slot->buffer;
	data = &buffer->datas[0];
	stride = (uint64_t)this->camera_info.width * this->bytes_per_pixel +
			info->x_padding;
	expected = stride * this->camera_info.height;
	available = info->size_filled > info->image_offset
			? info->size_filled - info->image_offset
			: 0;
	size = info->image_size < available ? info->image_size : available;
	if (size > expected)
		size = expected;
	if (info->image_offset > UINT32_MAX || size > UINT32_MAX ||
			stride > INT32_MAX ||
			info->image_offset + size > data->maxsize) {
		res = -ENOSPC;
		goto recycle;
	}
	if (info->incomplete || size != expected)
		chunk_flags |= SPA_CHUNK_FLAG_CORRUPTED;
	spa_meta_acquisition_init(&acquisition);
	frame = (struct pwao_image_frame){
		.data_index = 0,
		.header_flags = header_flags,
		.chunk_flags = chunk_flags,
		.offset = info->image_offset,
		.size = (uint32_t)size,
		.stride = (int32_t)stride,
		.sequence = info->frame_id,
		.pts = monotonic_nsec(),
		.acquisition = &acquisition,
	};
	if (this->port.io == NULL ||
			this->port.io->status == SPA_STATUS_HAVE_DATA) {
		res = -EBUSY;
	} else if ((res = pwao_image_frame_write(buffer, &frame,
				    PWAO_IMAGE_FRAME_REQUIRE_HEADER |
						    PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION)) >=
			0) {
		this->port.io->buffer_id = slot->id;
		this->port.io->status = SPA_STATUS_HAVE_DATA;
		slot->published = true;
	}
	if (res >= 0) {
		this->discontinuity = false;
		return 1;
	}
	if (res == -EBUSY) {
		this->discontinuity = true;
		res = 0;
	}

recycle:
	(void)queue_slot(this, slot);
	return res;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct aravis_camera_completion completion;
	bool published = false;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	if (!this->started)
		return SPA_STATUS_OK;
	res = recycle_buffer(this);
	if (res < 0)
		return res;
	res = aravis_camera_try_get_completion(this->camera, &completion);
	if (res < 0)
		return res;
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
		if (res == 1 &&
				(res = record_row_completion(
						 this, &completion)) < 0)
			return res;
		observe_row_progress(this);
		res = publish_row_block(this);
		if (res < 0)
			return res;
		return res > 0 ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
	}
	if (res == 1) {
		res = publish_buffer(this, &completion);
		if (res < 0)
			return res;
		published = res > 0;
	}
	return published ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
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

static int impl_get_interface(
		struct spa_handle *handle, const char *type, void **interface)
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
	if ((res = stop_source(this)) < 0) {
		first_error = res;
		/* Destroying the stream quiesces its receive thread before the
		 * external camera storage is released. */
		aravis_camera_close(this->camera);
		this->camera = NULL;
	}
	if (this->port.n_buffers != 0 && (res = release_buffers(this)) < 0 &&
			first_error == 0)
		first_error = res;
	if (this->camera != NULL) {
		aravis_camera_close(this->camera);
		this->camera = NULL;
	}
	return first_error;
}

static size_t impl_get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static void configure_props(
		struct impl *this, const struct aravis_camera_options *options)
{
	uint32_t n_items = 0;

	snprintf(this->node_name, sizeof(this->node_name), "aravis_source.%s",
			this->camera_info.serial);
	snprintf(this->node_description, sizeof(this->node_description),
			"%s %s", this->camera_info.model,
			this->camera_info.serial);
	snprintf(this->device_id, sizeof(this->device_id), "%s",
			options->device_id);
	snprintf(this->transport_name, sizeof(this->transport_name), "%s",
			transport_name(this->transport));
	snprintf(this->output_mode_name, sizeof(this->output_mode_name), "%s",
			this->output_mode == OUTPUT_MODE_ROW_BLOCK ? "row-block"
								   : "frame");
	snprintf(this->row_block_rows_text, sizeof(this->row_block_rows_text),
			"%u", this->row_block_rows);
#define ADD_ITEM(key, value)                                                   \
	this->prop_items[n_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "aravis");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, this->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, this->node_description);
	ADD_ITEM(SPA_KEY_NODE_DRIVER, "true");
	ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, this->camera_info.model);
	ADD_ITEM(SPA_KEY_DEVICE_SERIAL, this->camera_info.serial);
	ADD_ITEM(SPA_KEY_API_ARAVIS_DEVICE, this->device_id);
	ADD_ITEM(SPA_KEY_API_ARAVIS_TRANSPORT, this->transport_name);
	ADD_ITEM(SPA_KEY_API_ARAVIS_OUTPUT_MODE, this->output_mode_name);
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK) {
		ADD_ITEM(SPA_KEY_API_ARAVIS_ROW_BLOCK_ROWS,
				this->row_block_rows_text);
		ADD_ITEM(SPA_KEY_API_ARAVIS_DETECTOR_PROFILE,
				this->detector_profile);
	}
#undef ADD_ITEM
	this->props = SPA_DICT_INIT(this->prop_items, n_items);
}

static int impl_init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *this = (struct impl *)handle;
	struct aravis_camera_options options;
	const struct aravis_camera_info *camera_info;
	const char *value;
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(this, 0, sizeof(*this));
	memset(&options, 0, sizeof(options));
	options.device_id = info == NULL
			? NULL
			: spa_dict_lookup(info, SPA_KEY_API_ARAVIS_DEVICE);
	if (options.device_id == NULL)
		return -EINVAL;
	value = info == NULL
			? NULL
			: spa_dict_lookup(info, SPA_KEY_API_ARAVIS_TRANSPORT);
	if ((res = parse_transport(value, &options.transport)) < 0)
		return res;
	value = info == NULL
			? NULL
			: spa_dict_lookup(info, SPA_KEY_API_ARAVIS_OUTPUT_MODE);
	if ((res = parse_output_mode(value, &this->output_mode)) < 0)
		return res;
	this->row_block_rows = 1;
	value = info == NULL
			? NULL
			: spa_dict_lookup(info,
					  SPA_KEY_API_ARAVIS_ROW_BLOCK_ROWS);
	if (value != NULL &&
			(res = parse_positive_u32(
					 value, &this->row_block_rows)) < 0)
		return res;
	value = info == NULL
			? NULL
			: spa_dict_lookup(info,
					  SPA_KEY_API_ARAVIS_DETECTOR_PROFILE);
	if (value != NULL &&
			snprintf(this->detector_profile,
					sizeof(this->detector_profile), "%s",
					value) >=
					(int)sizeof(this->detector_profile))
		return -ENAMETOOLONG;
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK &&
			this->detector_profile[0] == '\0')
		return -EINVAL;
	this->handle.get_interface = impl_get_interface;
	this->handle.clear = impl_clear;
	this->log = spa_support_find(
			support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_hook_list_init(&this->hooks);
	if ((res = aravis_camera_open(&this->camera, &options)) < 0)
		return res;
	camera_info = aravis_camera_get_info(this->camera);
	if (camera_info == NULL || camera_info->payload_size > INT32_MAX ||
			map_pixel_format(camera_info->pixel_format,
					&this->video_format,
					&this->bytes_per_pixel) < 0 ||
			camera_info->width >
					INT32_MAX / this->bytes_per_pixel) {
		res = -ENOTSUP;
		goto error;
	}
	this->camera_info = *camera_info;
	if (make_frame_rate(camera_info->frame_rate, &this->frame_rate) < 0)
		this->frame_rate = SPA_FRACTION(0, 1);
	this->transport = aravis_camera_get_transport(this->camera);
	if (this->output_mode == OUTPUT_MODE_ROW_BLOCK &&
			(this->transport != ARAVIS_TRANSPORT_NATIVE_GV ||
					this->row_block_rows >=
							this->camera_info
									.height ||
					this->camera_info.height % this->row_block_rows !=
							0 ||
					this->camera_info.payload_size !=
							(uint64_t)this->camera_info.width *
									this->camera_info
											.height *
									this->bytes_per_pixel ||
					this->frame_rate.num == 0 ||
					output_rate(this).num == 0)) {
		res = -ENOTSUP;
		goto error;
	}
	this->layout_valid = true;
	this->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &impl_node, this);
	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_output_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_POLL_DRIVER;
	this->params[0] =
			SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(
			SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = SPA_N_ELEMENTS(this->params);
	configure_props(this, &options);
	this->info.props = &this->props;
	this->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	this->port.info = SPA_PORT_INFO_INIT();
	this->port.info.flags = SPA_PORT_FLAG_LIVE;
	this->port.params[0] = SPA_PARAM_INFO(
			SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->port.params[1] = SPA_PARAM_INFO(
			SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
	this->port.params[2] =
			SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	this->port.params[3] =
			SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	this->port.params[4] =
			SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->port.info.params = this->port.params;
	this->port.info.n_params = SPA_N_ELEMENTS(this->port.params);
	return 0;

error:
	aravis_camera_close(this->camera);
	this->camera = NULL;
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{
			SPA_TYPE_INTERFACE_Node,
	},
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

const struct spa_handle_factory spa_aravis_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ARAVIS_SOURCE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

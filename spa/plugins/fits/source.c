/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <spa/param/ndarray-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include "cube.h"
#include "fits.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS SPA_IMAGE_SOURCE_MAX_BUFFERS
#define TEXT_SIZE 512u

enum output_kind {
	OUTPUT_NONE,
	OUTPUT_NDARRAY,
	OUTPUT_GRAY16,
};

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	enum output_kind output;
	bool have_format;
	uint32_t n_buffers;
};

struct cadence {
	struct spa_fraction rate;
	uint64_t epoch;
	uint64_t next_sequence;
	uint64_t next_pts;
	bool ended;
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
	struct spa_dict_item prop_items[16];
	char path[PATH_MAX];
	char schema[TEXT_SIZE];
	char profile[TEXT_SIZE];
	char hdu_text[32];
	char frame_rank_text[8];
	char rate_text[32];
	char node_name[TEXT_SIZE];
	char description[TEXT_SIZE];
	char io_mode_text[16];
	char prefault_text[8];
	char loop_text[8];
	struct port port;
	struct spa_buffer_latest *latest;
	struct spa_image_source_latest transport;
	struct spa_image_source source;
	struct fits_cube *cube;
	struct fits_cube_info cube_info;
	struct spa_fraction rate;
	struct cadence cadence;
	bool loop;
	bool started;
};

static int copy_text(char *destination, size_t size, const char *source)
{
	if (source == NULL || source[0] == '\0' || strlen(source) >= size)
		return -EINVAL;
	memcpy(destination, source, strlen(source) + 1u);
	return 0;
}

static int parse_u32(const char *text, uint32_t fallback, uint32_t *value)
{
	if (text == NULL) {
		*value = fallback;
		return 0;
	}
	return spa_atou32(text, value, 10) ? 0 : -EINVAL;
}

static int parse_bool(const char *text, bool fallback, bool *value)
{
	if (text == NULL) {
		*value = fallback;
		return 0;
	}
	if (spa_streq(text, "true") || spa_streq(text, "1")) {
		*value = true;
		return 0;
	}
	if (spa_streq(text, "false") || spa_streq(text, "0")) {
		*value = false;
		return 0;
	}
	return -EINVAL;
}

static int parse_rate(const char *text, struct spa_fraction *rate)
{
	char trailing;
	unsigned int numerator, denominator;

	if (text == NULL)
		return -EINVAL;
	if (sscanf(text, "%u/%u%c", &numerator, &denominator, &trailing) == 2) {
		/* parsed fraction */
	} else if (sscanf(text, "%u%c", &numerator, &trailing) == 1) {
		denominator = 1;
	} else {
		return -EINVAL;
	}
	if (numerator == 0 || denominator == 0 ||
			(__uint128_t)SPA_NSEC_PER_SEC * denominator < numerator)
		return -EINVAL;
	rate->num = numerator;
	rate->denom = denominator;
	return 0;
}

static int monotonic_nsec(uint64_t *result)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -errno;
	*result = (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC +
			(uint64_t)now.tv_nsec;
	return 0;
}

static uint64_t sequence_pts(const struct cadence *cadence, uint64_t sequence)
{
	__uint128_t offset = (__uint128_t)sequence * SPA_NSEC_PER_SEC *
			cadence->rate.denom / cadence->rate.num;

	return cadence->epoch + (uint64_t)offset;
}

static void cadence_start(struct cadence *cadence,
		const struct spa_fraction *rate, uint64_t now)
{
	cadence->rate = *rate;
	cadence->epoch = now;
	cadence->next_sequence = 0;
	cadence->next_pts = now;
	cadence->ended = false;
}

static int cadence_due(struct cadence *cadence, uint64_t now,
		uint64_t frame_count, bool loop, uint64_t *sequence,
		uint64_t *frame, uint64_t *pts, bool *discontinuity)
{
	uint64_t due_sequence, selected;
	__uint128_t elapsed;

	if (cadence->ended || now < cadence->next_pts)
		return 0;
	elapsed = (__uint128_t)(now - cadence->epoch) * cadence->rate.num;
	due_sequence = (uint64_t)(elapsed /
			((__uint128_t)SPA_NSEC_PER_SEC * cadence->rate.denom));
	if (due_sequence < cadence->next_sequence)
		due_sequence = cadence->next_sequence;
	selected = due_sequence;
	if (!loop && selected >= frame_count) {
		if (cadence->next_sequence >= frame_count) {
			cadence->ended = true;
			return 0;
		}
		selected = frame_count - 1u;
	}
	*sequence = selected;
	*frame = loop ? selected % frame_count : selected;
	*pts = sequence_pts(cadence, selected);
	*discontinuity = selected == 0 || selected > cadence->next_sequence;
	cadence->next_sequence = selected + 1u;
	if (!loop && cadence->next_sequence >= frame_count) {
		cadence->ended = true;
		cadence->next_pts = UINT64_MAX;
	} else {
		cadence->next_pts = sequence_pts(cadence, cadence->next_sequence);
	}
	return 1;
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
	return -ENOENT;
}

static int stop_source(struct impl *self)
{
	int res;

	if (!self->started)
		return 0;
	self->started = false;
	res = spa_buffer_latest_worker_end(self->latest);
	return res < 0 ? res : 0;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;
	uint64_t now;
	int res;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!self->port.have_format || self->port.n_buffers == 0 ||
				!spa_buffer_latest_has_links(self->latest))
			return -EIO;
		if (self->started)
			return 0;
		if ((res = monotonic_nsec(&now)) < 0)
			return res;
		if ((res = spa_buffer_latest_worker_begin(self->latest)) < 0)
			return res;
		cadence_start(&self->cadence, &self->rate, now);
		self->started = true;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		return stop_source(self);
	default:
		return -ENOTSUP;
	}
}

static int node_add_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static struct spa_pod *build_ndarray_format(struct impl *self,
		struct spa_pod_builder *builder, uint32_t id)
{
	struct spa_pod_frame object;
	int32_t shape[2] = {
		(int32_t)self->cube_info.width,
		(int32_t)self->cube_info.height,
	};

	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema, SPA_POD_String(self->schema),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(self->cube_info.element_type),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, self->cube_info.frame_rank, shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(self->cube_info.frame_rank == 1 ?
					SPA_NDARRAY_LAYOUT_ROW_MAJOR :
					SPA_NDARRAY_LAYOUT_COLUMN_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&self->rate), 0);
	if (self->profile[0] != '\0')
		spa_pod_builder_add(builder, SPA_FORMAT_NDARRAY_profile,
				SPA_POD_String(self->profile), 0);
	return spa_pod_builder_pop(builder, &object);
}

static struct spa_pod *build_video_format(struct impl *self,
		struct spa_pod_builder *builder, uint32_t id)
{
	return spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_Format, id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_GRAY16_LE),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(
					self->cube_info.width, self->cube_info.height)),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&self->rate));
}

static size_t output_size(const struct impl *self)
{
	return self->port.output == OUTPUT_NDARRAY ? self->cube_info.plane_size :
			self->cube_info.plane_elements * sizeof(uint16_t);
}

static uint32_t output_stride(const struct impl *self)
{
	size_t element_size = self->port.output == OUTPUT_NDARRAY ?
			self->cube_info.element_size : sizeof(uint16_t);

	return self->cube_info.width * element_size;
}

static int build_port_param(struct impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	size_t size;

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index == 0)
			*param = build_ndarray_format(self, builder, id);
		else if (index == 1 && self->cube_info.frame_rank == 2)
			*param = build_video_format(self, builder, id);
		else
			return 0;
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Format:
		if (index > 0 || !self->port.have_format)
			return 0;
		*param = self->port.output == OUTPUT_NDARRAY ?
				build_ndarray_format(self, builder, id) :
				build_video_format(self, builder, id);
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Buffers:
		if (index > 0 || !self->port.have_format)
			return 0;
		size = output_size(self);
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)size),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int((int32_t)output_stride(self)),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Meta:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size,
				SPA_POD_Int(sizeof(struct spa_meta_header)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_IO:
		if (index == 0)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatest),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest)));
		else if (index == 1)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatestLink),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
		else
			return 0;
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
	uint8_t storage[2048];
	uint32_t count = 0;

	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
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

static int validate_ndarray_format(struct impl *self,
		const struct spa_pod *param)
{
	struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();
	const struct spa_pod_prop *property;
	const char *value;

	if (spa_format_ndarray_parse(param, &format) < 0 ||
			format.element_type != self->cube_info.element_type ||
			format.layout != (self->cube_info.frame_rank == 1 ?
					SPA_NDARRAY_LAYOUT_ROW_MAJOR :
					SPA_NDARRAY_LAYOUT_COLUMN_MAJOR) ||
			format.rate.num != self->rate.num ||
			format.rate.denom != self->rate.denom ||
			format.n_dimensions != self->cube_info.frame_rank ||
			format.shape[0] != self->cube_info.width ||
			(self->cube_info.frame_rank == 2 &&
			 format.shape[1] != self->cube_info.height) ||
			spa_ndarray_format_key_count(param,
					SPA_FORMAT_NDARRAY_schema) != 1)
		return -EINVAL;
	property = spa_pod_find_prop(param, NULL, SPA_FORMAT_NDARRAY_schema);
	if (property == NULL || spa_pod_get_string(&property->value, &value) < 0 ||
			!spa_streq(value, self->schema))
		return -EINVAL;
	if (self->profile[0] == '\0')
		return spa_ndarray_format_key_count(param,
				SPA_FORMAT_NDARRAY_profile) == 0 ? 0 : -EINVAL;
	if (spa_ndarray_format_key_count(param,
			SPA_FORMAT_NDARRAY_profile) != 1)
		return -EINVAL;
	property = spa_pod_find_prop(param, NULL, SPA_FORMAT_NDARRAY_profile);
	return property != NULL &&
			spa_pod_get_string(&property->value, &value) == 0 &&
			spa_streq(value, self->profile) ? 0 : -EINVAL;
}

static int validate_video_format(struct impl *self,
		const struct spa_pod *param)
{
	struct spa_video_info_raw format = { 0 };

	return self->cube_info.frame_rank == 2 &&
			spa_format_video_raw_parse(param, &format) >= 0 &&
			format.format == SPA_VIDEO_FORMAT_GRAY16_LE &&
			format.size.width == self->cube_info.width &&
			format.size.height == self->cube_info.height &&
			format.framerate.num == self->rate.num &&
			format.framerate.denom == self->rate.denom ? 0 : -EINVAL;
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	enum output_kind output;
	int ndarray_result, video_result;

	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (self->started || self->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			self->port.have_format = false;
			self->port.output = OUTPUT_NONE;
		}
		return 0;
	}
	ndarray_result = validate_ndarray_format(self, param);
	video_result = validate_video_format(self, param);
	if (ndarray_result == 0)
		output = OUTPUT_NDARRAY;
	else if (video_result == 0)
		output = OUTPUT_GRAY16;
	else
		return -EINVAL;
	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		self->port.output = output;
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
	int res;

	if (self->started || spa_buffer_latest_has_links(self->latest))
		return -EBUSY;
	res = spa_image_source_latest_teardown(&self->transport, &self->source);
	if (res < 0)
		return res;
	self->port.n_buffers = 0;
	return 0;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t flags SPA_UNUSED, uint32_t port_id,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *self = object;
	size_t required;
	uint32_t i;
	int res;

	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return self->port.n_buffers == 0 ? 0 : release_buffers(self);
	if (!self->port.have_format || self->port.n_buffers != 0 ||
			buffers == NULL || n_buffers < MIN_BUFFERS ||
			n_buffers > MAX_BUFFERS)
		return -EINVAL;
	required = output_size(self);
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				(data->type != SPA_DATA_MemPtr &&
				 data->type != SPA_DATA_MemFd) ||
				data->maxsize < required || data->chunk == NULL)
			return -EINVAL;
	}
	res = spa_image_source_latest_prepare(&self->transport, &self->source,
			buffers, n_buffers);
	if (res < 0)
		return res;
	self->port.n_buffers = n_buffers;
	return 0;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	return spa_buffer_latest_set_io(self->latest, id, data, size);
}

static int port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int node_process(void *object)
{
	struct impl *self = object;
	struct spa_image_source_buffer *image = NULL;
	struct spa_image_frame publication;
	struct spa_buffer *buffer;
	struct spa_data *data;
	uint64_t now, sequence, frame, pts;
	bool discontinuity;
	int res;

	if (!self->started)
		return SPA_STATUS_OK;
	if ((res = monotonic_nsec(&now)) < 0)
		return res;
	if (cadence_due(&self->cadence, now, self->cube_info.frames,
			self->loop, &sequence, &frame, &pts, &discontinuity) == 0)
		return SPA_STATUS_OK;
	res = spa_image_source_try_acquire(&self->source, &image);
	if (res < 0)
		return res == -EPIPE ? SPA_STATUS_OK : res;
	if (res == 0)
		return SPA_STATUS_OK;
	buffer = spa_image_source_buffer_get_buffer(image);
	if (buffer == NULL || buffer->n_datas == 0) {
		(void)spa_image_source_return_buffer(&self->source, image);
		return -EPROTO;
	}
	data = &buffer->datas[0];
	res = fits_cube_read_plane(self->cube, frame,
			self->port.output == OUTPUT_NDARRAY ?
					FITS_CUBE_OUTPUT_NATIVE : FITS_CUBE_OUTPUT_GRAY16,
			data->data, data->maxsize);
	if (res < 0) {
		(void)spa_image_source_return_buffer(&self->source, image);
		return res;
	}
	publication = (struct spa_image_frame) {
		.version = SPA_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.header_flags = discontinuity ? SPA_META_HEADER_FLAG_DISCONT : 0,
		.offset = 0,
		.size = (uint32_t)output_size(self),
		.stride = (int32_t)output_stride(self),
		.sequence = sequence,
		.pts = (int64_t)pts,
	};
	res = spa_image_source_publish_complete(&self->source, image,
			&publication);
	return res < 0 ? res : SPA_STATUS_HAVE_DATA;
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

	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct impl *self = (struct impl *)handle;
	int first_error = 0, res;

	if ((res = stop_source(self)) < 0)
		first_error = res;
	if (self->port.n_buffers != 0 &&
			(res = release_buffers(self)) < 0 && first_error == 0)
		first_error = res;
	spa_buffer_latest_destroy(self->latest);
	self->latest = NULL;
	fits_cube_close(self->cube);
	self->cube = NULL;
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

	snprintf(self->node_name, sizeof(self->node_name), "fits_source");
	if (self->cube_info.frame_rank == 1)
		snprintf(self->description, sizeof(self->description),
				"FITS vector sequence %u (%" PRIu64 " frames)",
				self->cube_info.width, self->cube_info.frames);
	else
		snprintf(self->description, sizeof(self->description),
				"FITS image cube %ux%u (%" PRIu64 " frames)",
				self->cube_info.width, self->cube_info.height,
				self->cube_info.frames);
#define ADD_ITEM(key, value) \
	self->prop_items[n++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "fits");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS,
			self->cube_info.frame_rank == 2 ? "Video/Source" : "Data/Source");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description);
	ADD_ITEM(SPA_KEY_API_FITS_PATH, self->path);
	ADD_ITEM(SPA_KEY_API_FITS_HDU, self->hdu_text);
	ADD_ITEM(SPA_KEY_API_FITS_FRAME_RANK, self->frame_rank_text);
	ADD_ITEM(SPA_KEY_API_FITS_RATE, self->rate_text);
	ADD_ITEM(SPA_KEY_API_FITS_SCHEMA, self->schema);
	if (self->profile[0] != '\0')
		ADD_ITEM(SPA_KEY_API_FITS_PROFILE, self->profile);
	ADD_ITEM(SPA_KEY_API_FITS_IO_MODE, self->io_mode_text);
	ADD_ITEM(SPA_KEY_API_FITS_PREFAULT, self->prefault_text);
	ADD_ITEM(SPA_KEY_API_FITS_LOOP, self->loop_text);
#undef ADD_ITEM
	self->props = SPA_DICT_INIT(self->prop_items, n);
}

static int init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *self = (struct impl *)handle;
	struct fits_cube_options options = {
		.hdu = 1,
		.frame_rank = 2,
		.io_mode = FITS_CUBE_IO_FILE,
	};
	struct spa_image_source_config source_config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = MIN_BUFFERS,
		.max_buffers = MAX_BUFFERS,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER,
	};
	const struct fits_cube_info *cube_info;
	const char *value;
	char message[256];
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(self, 0, sizeof(*self));
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	value = info == NULL ? NULL : spa_dict_lookup(info, SPA_KEY_API_FITS_PATH);
	if (copy_text(self->path, sizeof(self->path), value) < 0)
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_SCHEMA);
	if (copy_text(self->schema, sizeof(self->schema), value) < 0)
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_PROFILE);
	if (value != NULL && copy_text(self->profile, sizeof(self->profile), value) < 0)
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_RATE);
	if (parse_rate(value, &self->rate) < 0)
		return -EINVAL;
	if (parse_u32(spa_dict_lookup(info, SPA_KEY_API_FITS_HDU), 1,
			&options.hdu) < 0 || options.hdu == 0)
		return -EINVAL;
	if (parse_u32(spa_dict_lookup(info, SPA_KEY_API_FITS_FRAME_RANK), 2,
			&options.frame_rank) < 0 ||
			(options.frame_rank != 1 && options.frame_rank != 2))
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_IO_MODE);
	if (value != NULL) {
		if (spa_streq(value, "file"))
			options.io_mode = FITS_CUBE_IO_FILE;
		else if (spa_streq(value, "mmap"))
			options.io_mode = FITS_CUBE_IO_MMAP;
		else
			return -EINVAL;
	}
	if (parse_bool(spa_dict_lookup(info, SPA_KEY_API_FITS_PREFAULT), false,
			&options.prefault) < 0)
		return -EINVAL;
	if (options.prefault && options.io_mode != FITS_CUBE_IO_MMAP)
		return -EINVAL;
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_LOOP);
	if (parse_bool(value, true, &self->loop) < 0)
		return -EINVAL;
	options.path = self->path;
	if ((res = fits_cube_open(&self->cube, &options, message,
			sizeof(message))) < 0) {
		spa_log_error(self->log, "%s", message);
		return res;
	}
	cube_info = fits_cube_get_info(self->cube);
	self->cube_info = *cube_info;
	if (self->cube_info.plane_size > INT32_MAX ||
			self->cube_info.plane_elements > INT32_MAX / sizeof(uint16_t) ||
			(uint64_t)self->cube_info.width * self->cube_info.element_size >
					INT32_MAX) {
		res = -EOVERFLOW;
		goto error;
	}
	snprintf(self->hdu_text, sizeof(self->hdu_text), "%u", options.hdu);
	snprintf(self->frame_rank_text, sizeof(self->frame_rank_text), "%u",
			options.frame_rank);
	snprintf(self->rate_text, sizeof(self->rate_text), "%u/%u",
			self->rate.num, self->rate.denom);
	snprintf(self->io_mode_text, sizeof(self->io_mode_text), "%s",
			options.io_mode == FITS_CUBE_IO_FILE ? "file" : "mmap");
	snprintf(self->prefault_text, sizeof(self->prefault_text), "%s",
			options.prefault ? "true" : "false");
	snprintf(self->loop_text, sizeof(self->loop_text), "%s",
			self->loop ? "true" : "false");
	spa_hook_list_init(&self->hooks);
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RTC_PROCESS;
	configure_props(self);
	self->info.props = &self->props;
	self->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->port.info = SPA_PORT_INFO_INIT();
	self->port.info.flags = SPA_PORT_FLAG_LIVE;
	self->port.params[0] = (struct spa_param_info) {
		.id = SPA_PARAM_EnumFormat,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[1] = (struct spa_param_info) {
		.id = SPA_PARAM_Format,
		.flags = SPA_PARAM_INFO_READWRITE,
	};
	self->port.params[2] = (struct spa_param_info) {
		.id = SPA_PARAM_Buffers,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[3] = (struct spa_param_info) {
		.id = SPA_PARAM_Meta,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[4] = (struct spa_param_info) {
		.id = SPA_PARAM_IO,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.info.params = self->port.params;
	self->port.info.n_params = SPA_N_ELEMENTS(self->port.params);
	self->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, self, self->log);
	if (self->latest == NULL) {
		res = -errno;
		goto error;
	}
	res = spa_image_source_latest_init(&self->transport, &self->source,
			self->latest, &source_config);
	if (res < 0) {
		spa_buffer_latest_destroy(self->latest);
		self->latest = NULL;
		goto error;
	}
	return 0;

error:
	fits_cube_close(self->cube);
	self->cube = NULL;
	return res;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_fits_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_FITS_SOURCE,
	NULL,
	get_size,
	init,
	enum_interface_info,
};

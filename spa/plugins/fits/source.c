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

#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/io.h>
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
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/support/system.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include "cube.h"
#include "fits.h"
#include "../image-frame.h"

#define MIN_BUFFERS 2u
#define MAX_BUFFERS 64u
#define TEXT_SIZE 512u

enum output_kind {
	OUTPUT_NONE,
	OUTPUT_NDARRAY,
	OUTPUT_GRAY16,
};

enum buffer_state {
	BUFFER_AVAILABLE,
	BUFFER_PRODUCER,
	BUFFER_PUBLISHED,
};

struct buffer {
	struct spa_buffer *buffer;
	uint32_t id;
	enum buffer_state state;
};

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	enum output_kind output;
	struct spa_io_buffers *io;
	struct buffer buffers[MAX_BUFFERS];
	uint32_t scan_hint;
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
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source timer_source;
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
	char sample_rank_text[8];
	char rate_text[32];
	char node_name[TEXT_SIZE];
	char description[TEXT_SIZE];
	char io_mode_text[16];
	char prefault_text[8];
	char loop_text[8];
	char readiness_text[8];
	struct port port;
	struct fits_cube *cube;
	struct fits_cube_info cube_info;
	struct spa_fraction rate;
	struct cadence cadence;
	bool loop;
	bool discontinuity;
	bool started;
	bool timerfd_readiness;
};

static int node_process(void *object);

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
		uint64_t sample_count, bool loop, uint64_t *sequence,
		uint64_t *sample, uint64_t *pts, bool *discontinuity)
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
	if (!loop && selected >= sample_count) {
		if (cadence->next_sequence >= sample_count) {
			cadence->ended = true;
			return 0;
		}
		selected = sample_count - 1u;
	}
	*sequence = selected;
	*sample = loop ? selected % sample_count : selected;
	*pts = sequence_pts(cadence, selected);
	*discontinuity = selected == 0 || selected > cadence->next_sequence;
	cadence->next_sequence = selected + 1u;
	if (!loop && cadence->next_sequence >= sample_count) {
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

static int set_release_timer(struct impl *self, uint64_t release)
{
	struct itimerspec timer = { 0 };

	if (release != UINT64_MAX) {
		timer.it_value.tv_sec = (time_t)(release / SPA_NSEC_PER_SEC);
		timer.it_value.tv_nsec = (long)(release % SPA_NSEC_PER_SEC);
	}
	return spa_system_timerfd_settime(self->data_system,
			self->timer_source.fd, SPA_FD_TIMER_ABSTIME, &timer, NULL);
}

static uint64_t release_after(const struct cadence *cadence, uint64_t now)
{
	uint64_t sequence;
	__uint128_t elapsed;

	if (cadence->ended)
		return UINT64_MAX;
	if (cadence->next_pts > now)
		return cadence->next_pts;
	elapsed = (__uint128_t)(now - cadence->epoch) * cadence->rate.num;
	sequence = (uint64_t)(elapsed /
			((__uint128_t)SPA_NSEC_PER_SEC * cadence->rate.denom)) + 1u;
	if (sequence < cadence->next_sequence)
		sequence = cadence->next_sequence;
	return sequence_pts(cadence, sequence);
}

static void timer_ready(struct spa_source *source)
{
	struct impl *self = source->data;
	uint64_t expirations, now = 0;
	int res;

	if (SPA_UNLIKELY(source->rmask & (SPA_IO_ERR | SPA_IO_HUP))) {
		spa_log_error(self->log, "release timer error: 0x%08x",
				source->rmask);
		return;
	}
	if (!(source->rmask & SPA_IO_IN))
		return;
	if ((res = spa_system_timerfd_read(self->data_system, source->fd,
			&expirations)) < 0) {
		if (res != -EAGAIN)
			spa_log_error(self->log, "release timer read failed: %s",
					spa_strerror(res));
		return;
	}
	if (!self->started)
		return;
	if (self->port.io != NULL &&
			self->port.io->status == SPA_STATUS_HAVE_DATA) {
		self->discontinuity = true;
		if (monotonic_nsec(&now) == 0)
			(void) set_release_timer(self,
					release_after(&self->cadence, now));
		return;
	}
	res = node_process(self);
	if (res < 0) {
		spa_log_error(self->log, "timed FITS publication failed: %s",
				spa_strerror(res));
		(void) set_release_timer(self, UINT64_MAX);
		return;
	}
	if (res != SPA_STATUS_OK)
		spa_node_call_ready(&self->callbacks, res);
	if (monotonic_nsec(&now) == 0)
		(void) set_release_timer(self,
				release_after(&self->cadence, now));
}

static int remove_timer_source(struct spa_loop *loop SPA_UNUSED,
		bool async SPA_UNUSED, uint32_t seq SPA_UNUSED,
		const void *data SPA_UNUSED, size_t size SPA_UNUSED, void *user_data)
{
	struct impl *self = user_data;

	if (self->timer_source.loop != NULL)
		return spa_loop_remove_source(self->data_loop, &self->timer_source);
	return 0;
}

static int stop_source(struct impl *self)
{
	int res = 0;

	if (!self->started)
		return 0;
	self->started = false;
	if (self->timerfd_readiness)
		res = set_release_timer(self, UINT64_MAX);
	return res;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;
	uint64_t now = 0;
	int res;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!self->port.have_format || self->port.n_buffers == 0 ||
				self->port.io == NULL)
			return -EIO;
		if (self->started)
			return 0;
		if ((res = monotonic_nsec(&now)) < 0)
			return res;
		cadence_start(&self->cadence, &self->rate, now);
		self->started = true;
		if (self->timerfd_readiness &&
				(res = set_release_timer(self,
					self->cadence.next_pts)) < 0) {
			self->started = false;
			return res;
		}
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
					SPA_TYPE_Int, self->cube_info.sample_rank, shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(self->cube_info.sample_rank == 1 ?
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
		else if (index == 1 && self->cube_info.sample_rank == 2)
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
		if (index == 0)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
		else
			return 0;
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_IO:
		if (index == 0)
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamIO, id,
					SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
					SPA_PARAM_IO_size,
					SPA_POD_Int(sizeof(struct spa_io_buffers)));
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
			format.layout != (self->cube_info.sample_rank == 1 ?
					SPA_NDARRAY_LAYOUT_ROW_MAJOR :
					SPA_NDARRAY_LAYOUT_COLUMN_MAJOR) ||
			format.rate.num != self->rate.num ||
			format.rate.denom != self->rate.denom ||
			format.n_dimensions != self->cube_info.sample_rank ||
			format.shape[0] != self->cube_info.width ||
			(self->cube_info.sample_rank == 2 &&
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

	return self->cube_info.sample_rank == 2 &&
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
	uint32_t i;

	if (self->started)
		return -EBUSY;
	for (i = 0; i < self->port.n_buffers; i++)
		memset(&self->port.buffers[i], 0, sizeof(self->port.buffers[i]));
	self->port.n_buffers = 0;
	self->port.scan_hint = 0;
	return 0;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t flags SPA_UNUSED, uint32_t port_id,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *self = object;
	size_t required;
	uint32_t i;

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
	for (i = 0; i < n_buffers; i++)
		self->port.buffers[i] = (struct buffer) {
			.buffer = buffers[i],
			.id = i,
			.state = BUFFER_AVAILABLE,
		};
	self->port.n_buffers = n_buffers;
	self->port.scan_hint = 0;
	return 0;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
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
	struct buffer *buffer;
	uint32_t id;

	if (io == NULL || io->status == SPA_STATUS_HAVE_DATA ||
			io->buffer_id == SPA_ID_INVALID)
		return 0;
	id = io->buffer_id;
	if (id >= self->port.n_buffers)
		return -EPROTO;
	buffer = &self->port.buffers[id];
	if (buffer->state != BUFFER_PUBLISHED)
		return -EPROTO;
	io->buffer_id = SPA_ID_INVALID;
	buffer->state = BUFFER_AVAILABLE;
	self->port.scan_hint = id + 1u;
	if (self->port.scan_hint == self->port.n_buffers)
		self->port.scan_hint = 0;
	return 1;
}

static struct buffer *take_buffer(struct impl *self)
{
	uint32_t i;

	for (i = 0; i < self->port.n_buffers; i++) {
		uint32_t id = self->port.scan_hint + i;
		struct buffer *buffer;

		if (id >= self->port.n_buffers)
			id -= self->port.n_buffers;
		buffer = &self->port.buffers[id];
		if (buffer->state != BUFFER_AVAILABLE)
			continue;
		buffer->state = BUFFER_PRODUCER;
		self->port.scan_hint = id + 1u;
		if (self->port.scan_hint == self->port.n_buffers)
			self->port.scan_hint = 0;
		return buffer;
	}
	return NULL;
}

static void return_buffer(struct impl *self, struct buffer *buffer)
{
	buffer->state = BUFFER_AVAILABLE;
	self->port.scan_hint = buffer->id + 1u;
	if (self->port.scan_hint == self->port.n_buffers)
		self->port.scan_hint = 0;
}

static int node_process(void *object)
{
	struct impl *self = object;
	struct pwao_image_frame publication;
	struct buffer *output;
	struct spa_data *data;
	uint64_t now = 0, sequence, sample, pts;
	uint32_t size;
	bool discontinuity;
	int res;

	if (!self->started)
		return SPA_STATUS_OK;
	if (self->port.io == NULL)
		return -EIO;
	if ((res = recycle_buffer(self)) < 0)
		return res;
	if ((res = monotonic_nsec(&now)) < 0)
		return res;
	if (cadence_due(&self->cadence, now, self->cube_info.samples,
			self->loop, &sequence, &sample, &pts, &discontinuity) == 0)
		return SPA_STATUS_OK;
	if (self->port.io->status == SPA_STATUS_HAVE_DATA) {
		self->discontinuity = true;
		return SPA_STATUS_HAVE_DATA;
	}
	output = take_buffer(self);
	if (output == NULL) {
		self->discontinuity = true;
		return SPA_STATUS_OK;
	}
	if (output->buffer == NULL || output->buffer->n_datas == 0) {
		return_buffer(self, output);
		return -EPROTO;
	}
	data = &output->buffer->datas[0];
	res = fits_cube_read_plane(self->cube, sample,
			self->port.output == OUTPUT_NDARRAY ?
					FITS_CUBE_OUTPUT_NATIVE : FITS_CUBE_OUTPUT_GRAY16,
			data->data, data->maxsize);
	if (res < 0) {
		return_buffer(self, output);
		return res;
	}
	size = (uint32_t)output_size(self);
	publication = (struct pwao_image_frame) {
		.data_index = 0,
		.header_flags = (discontinuity || self->discontinuity) ?
				SPA_META_HEADER_FLAG_DISCONT : 0,
		.offset = 0,
		.size = size,
		.stride = (int32_t)output_stride(self),
		.sequence = sequence,
		.pts = (int64_t)pts,
	};
	res = pwao_image_frame_write(output->buffer, &publication,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER);
	if (res < 0) {
		return_buffer(self, output);
		return res;
	}
	self->port.io->buffer_id = output->id;
	self->port.io->status = SPA_STATUS_HAVE_DATA;
	output->state = BUFFER_PUBLISHED;
	self->discontinuity = false;
	return SPA_STATUS_HAVE_DATA;
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
	if (self->timer_source.loop != NULL) {
		res = spa_loop_locked(self->data_loop, remove_timer_source, 0,
				NULL, 0, self);
		if (res < 0 && first_error == 0)
			first_error = res;
	}
	if (self->timer_source.fd >= 0) {
		res = spa_system_close(self->data_system, self->timer_source.fd);
		if (res < 0 && first_error == 0)
			first_error = res;
		self->timer_source.fd = -1;
	}
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
	if (self->cube_info.sample_rank == 1)
		snprintf(self->description, sizeof(self->description),
				"FITS vector sequence %u (%" PRIu64 " samples)",
				self->cube_info.width, self->cube_info.samples);
	else
		snprintf(self->description, sizeof(self->description),
				"FITS image cube %ux%u (%" PRIu64 " frames)",
				self->cube_info.width, self->cube_info.height,
				self->cube_info.samples);
#define ADD_ITEM(key, value) \
	self->prop_items[n++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "fits");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS,
			self->cube_info.sample_rank == 2 ? "Video/Source" : "Data/Source");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description);
	ADD_ITEM(SPA_KEY_NODE_DRIVER, "true");
	ADD_ITEM(SPA_KEY_API_FITS_PATH, self->path);
	ADD_ITEM(SPA_KEY_API_FITS_HDU, self->hdu_text);
	ADD_ITEM(SPA_KEY_API_FITS_SAMPLE_RANK, self->sample_rank_text);
	ADD_ITEM(SPA_KEY_API_FITS_RATE, self->rate_text);
	ADD_ITEM(SPA_KEY_API_FITS_SCHEMA, self->schema);
	if (self->profile[0] != '\0')
		ADD_ITEM(SPA_KEY_API_FITS_PROFILE, self->profile);
	ADD_ITEM(SPA_KEY_API_FITS_IO_MODE, self->io_mode_text);
	ADD_ITEM(SPA_KEY_API_FITS_PREFAULT, self->prefault_text);
	ADD_ITEM(SPA_KEY_API_FITS_LOOP, self->loop_text);
	ADD_ITEM(SPA_KEY_API_FITS_READINESS, self->readiness_text);
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
		.sample_rank = 2,
		.io_mode = FITS_CUBE_IO_FILE,
	};
	const struct fits_cube_info *cube_info;
	const char *value;
	char message[256];
	int res;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	memset(self, 0, sizeof(*self));
	self->timer_source.fd = -1;
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	self->data_loop = spa_support_find(support, n_support,
			SPA_TYPE_INTERFACE_DataLoop);
	self->data_system = spa_support_find(support, n_support,
			SPA_TYPE_INTERFACE_DataSystem);
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
	if (parse_u32(spa_dict_lookup(info, SPA_KEY_API_FITS_SAMPLE_RANK), 2,
			&options.sample_rank) < 0 ||
			(options.sample_rank != 1 && options.sample_rank != 2))
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
	value = spa_dict_lookup(info, SPA_KEY_API_FITS_READINESS);
	if (value == NULL || spa_streq(value, "poll"))
		self->timerfd_readiness = false;
	else if (spa_streq(value, "timerfd"))
		self->timerfd_readiness = true;
	else
		return -EINVAL;
	if (self->timerfd_readiness &&
			(self->data_loop == NULL || self->data_system == NULL))
		return -ENOTSUP;
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
	snprintf(self->sample_rank_text, sizeof(self->sample_rank_text), "%u",
			options.sample_rank);
	snprintf(self->rate_text, sizeof(self->rate_text), "%u/%u",
			self->rate.num, self->rate.denom);
	snprintf(self->io_mode_text, sizeof(self->io_mode_text), "%s",
			options.io_mode == FITS_CUBE_IO_FILE ? "file" : "mmap");
	snprintf(self->prefault_text, sizeof(self->prefault_text), "%s",
			options.prefault ? "true" : "false");
	snprintf(self->loop_text, sizeof(self->loop_text), "%s",
			self->loop ? "true" : "false");
	snprintf(self->readiness_text, sizeof(self->readiness_text), "%s",
			self->timerfd_readiness ? "timerfd" : "poll");
	if (self->timerfd_readiness) {
		self->timer_source.func = timer_ready;
		self->timer_source.data = self;
		self->timer_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
		self->timer_source.fd = spa_system_timerfd_create(self->data_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		if (self->timer_source.fd < 0) {
			res = self->timer_source.fd;
			goto error;
		}
		if ((res = spa_loop_add_source(self->data_loop,
				&self->timer_source)) < 0) {
			(void) spa_system_close(self->data_system,
					self->timer_source.fd);
			self->timer_source.fd = -1;
			goto error;
		}
	}
	spa_hook_list_init(&self->hooks);
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT;
	if (!self->timerfd_readiness)
		self->info.flags |= SPA_NODE_FLAG_POLL_DRIVER;
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

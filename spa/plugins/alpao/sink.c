/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/monitor/device.h>
#include <spa/node/buffer-latest.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/alpao.h>

#include "backend.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the ALPAO normalized-command backend currently requires little endian"
#endif

#define MAX_BUFFERS SPA_BUFFER_LATEST_MAX_BUFFERS
#define PROFILE_DIGEST_CHARACTERS 64u
#define BACKEND_NAME_SIZE 8u
#define SERIAL_SIZE 128u
#define PROFILE_SIZE (sizeof("sha256:") - 1u + PROFILE_DIGEST_CHARACTERS + 1u)
#define ACTUATOR_COUNT_TEXT_SIZE 16u
#define NODE_NAME_SIZE (sizeof("alpao_sink.") - 1u + SERIAL_SIZE)

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.alpao.sink");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct input_port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[4];
	struct spa_buffer *buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	bool have_format;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_log *log;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_dict node_props;
	struct spa_dict_item node_items[8];
	char backend_name[BACKEND_NAME_SIZE];
	char serial[SERIAL_SIZE];
	char profile[PROFILE_SIZE];
	char actuator_count_text[ACTUATOR_COUNT_TEXT_SIZE];
	char node_name[NODE_NAME_SIZE];
	struct alpao_backend *backend;
	struct spa_buffer_latest *latest;
	struct input_port input;
	uint32_t actuator_count;
	size_t command_bytes;
	bool started;
};

static int copy_string(char *destination, size_t size, const char *source)
{
	size_t length;

	if (destination == NULL || size == 0 || source == NULL)
		return -EINVAL;
	length = strlen(source);
	if (length >= size)
		return -ENAMETOOLONG;
	memcpy(destination, source, length + 1u);
	return 0;
}

static bool valid_profile(const char *profile)
{
	uint32_t i;

	if (profile == NULL || !spa_strstartswith(profile, "sha256:"))
		return false;
	profile += strlen("sha256:");
	if (strlen(profile) != PROFILE_DIGEST_CHARACTERS)
		return false;
	for (i = 0; i < PROFILE_DIGEST_CHARACTERS; i++)
		if (!((profile[i] >= '0' && profile[i] <= '9') ||
				(profile[i] >= 'a' && profile[i] <= 'f')))
			return false;
	return true;
}

static int read_options(struct impl *self, const struct spa_dict *info)
{
	const char *backend = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_ALPAO_BACKEND);
	const char *serial = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_ALPAO_SERIAL);
	const char *count = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_ALPAO_ACTUATOR_COUNT);
	const char *profile = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_ALPAO_PROFILE);
	int res;

	if (backend == NULL || (strcmp(backend, "mock") != 0 &&
			strcmp(backend, "asdk") != 0))
		return -EINVAL;
	if (strcmp(backend, "asdk") == 0 && (serial == NULL || serial[0] == '\0'))
		return -EINVAL;
	if (!spa_atou32(count, &self->actuator_count, 10) ||
			self->actuator_count == 0 ||
			self->actuator_count > (uint32_t)(INT32_MAX / sizeof(double)))
		return -EINVAL;
	if (!valid_profile(profile))
		return -EINVAL;
	if ((res = copy_string(self->backend_name, sizeof(self->backend_name),
				backend)) < 0 ||
			(res = copy_string(self->serial, sizeof(self->serial),
				serial == NULL ? "" : serial)) < 0 ||
			(res = copy_string(self->profile, sizeof(self->profile), profile)) < 0 ||
			(res = copy_string(self->actuator_count_text,
				sizeof(self->actuator_count_text), count)) < 0)
		return res;
	self->command_bytes = (size_t)self->actuator_count * sizeof(double);
	return alpao_backend_new(self->backend_name, &self->backend);
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
	struct input_port *input = &self->input;
	uint64_t old = full ? input->info.change_mask : 0;

	if (full)
		input->info.change_mask = input->info_all;
	if (input->info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_INPUT, 0,
				&input->info);
		input->info.change_mask = old;
	}
}

static int add_listener(void *object, struct spa_hook *listener,
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

static int set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int enum_params(void *object, int seq, uint32_t id, uint32_t start,
		uint32_t num, const struct spa_pod *filter)
{
	(void)object;
	(void)seq;
	(void)id;
	(void)start;
	(void)num;
	(void)filter;
	return -ENOENT;
}

static int set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	(void)object;
	(void)id;
	(void)flags;
	(void)param;
	return -ENOENT;
}

static int set_io(void *object, uint32_t id, void *data, size_t size)
{
	(void)object;
	(void)id;
	(void)data;
	(void)size;
	return -ENOENT;
}

static int add_port(void *object, enum spa_direction direction,
		uint32_t port_id, const struct spa_dict *props)
{
	(void)object;
	(void)direction;
	(void)port_id;
	(void)props;
	return -ENOTSUP;
}

static int remove_port(void *object, enum spa_direction direction,
		uint32_t port_id)
{
	(void)object;
	(void)direction;
	(void)port_id;
	return -ENOTSUP;
}

static int build_port_param(struct impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format: {
		int32_t shape[] = { (int32_t)self->actuator_count };

		if (index > 0 || (id == SPA_PARAM_Format && !self->input.have_format))
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
				SPA_FORMAT_NDARRAY_schema,
				SPA_POD_String(SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND),
				SPA_FORMAT_NDARRAY_elementType,
				SPA_POD_Id(SPA_ELEMENT_TYPE_F64_LE),
				SPA_FORMAT_NDARRAY_shape,
				SPA_POD_Array(sizeof(int32_t), SPA_TYPE_Int,
						SPA_N_ELEMENTS(shape), shape),
				SPA_FORMAT_NDARRAY_layout,
				SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
				SPA_FORMAT_NDARRAY_profile, SPA_POD_String(self->profile));
		return *param == NULL ? -ENOSPC : 1;
	}
	case SPA_PARAM_Buffers:
		if (index > 0 || !self->input.have_format)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2,
						(int32_t)MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)self->command_bytes),
				SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(double)),
				SPA_PARAM_BUFFERS_align, SPA_POD_Int(_Alignof(double)),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatestLink),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
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
	uint8_t storage[2048];
	uint32_t count = 0;
	struct spa_result_node_params result = { 0 };

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
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

static int validate_format(struct impl *self, const struct spa_pod *param)
{
	struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();
	const struct spa_pod_prop *schema_property;
	const struct spa_pod_prop *profile_property;
	const char *schema = NULL;
	const char *profile = NULL;

	if (spa_format_ndarray_parse(param, &format) < 0 ||
			format.element_type != SPA_ELEMENT_TYPE_F64_LE ||
			format.layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
			format.rate.denom != 0 || format.n_dimensions != 1 ||
			format.shape[0] != self->actuator_count ||
			spa_ndarray_format_key_count(param, SPA_FORMAT_NDARRAY_schema) != 1 ||
			spa_ndarray_format_key_count(param, SPA_FORMAT_NDARRAY_profile) != 1)
		return -EINVAL;
	schema_property = spa_pod_find_prop(param, NULL,
			SPA_FORMAT_NDARRAY_schema);
	profile_property = spa_pod_find_prop(param, NULL,
			SPA_FORMAT_NDARRAY_profile);
	if (schema_property == NULL || profile_property == NULL ||
			spa_pod_get_string(&schema_property->value, &schema) < 0 ||
			spa_pod_get_string(&profile_property->value, &profile) < 0 ||
			!spa_streq(schema, SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND) ||
			!spa_streq(profile, self->profile))
		return -EINVAL;
	return 0;
}

static int release_buffers(struct impl *self)
{
	if (self->started)
		return -EBUSY;
	if (spa_buffer_latest_has_links(self->latest))
		return -EBUSY;
	spa_buffer_latest_clear_buffers(self->latest);
	memset(self->input.buffers, 0, sizeof(self->input.buffers));
	self->input.n_buffers = 0;
	return 0;
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	int res;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (param == NULL) {
		if (self->started || self->input.n_buffers != 0)
			return -EBUSY;
		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			self->input.have_format = false;
			self->input.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
			self->input.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
			self->input.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			emit_port_info(self, false);
		}
		return 0;
	}
	if ((res = validate_format(self, param)) < 0)
		return res;
	if (self->input.have_format &&
			(self->started || self->input.n_buffers != 0))
		return 0;
	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		self->input.have_format = true;
		self->input.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
		self->input.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
		self->input.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		emit_port_info(self, false);
	}
	return 0;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t flags, struct spa_buffer **buffers,
		uint32_t n_buffers)
{
	struct impl *self = object;
	uint32_t i;

	(void)flags;
	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return release_buffers(self);
	if (!self->input.have_format || buffers == NULL || n_buffers < 2 ||
			n_buffers > MAX_BUFFERS || self->input.n_buffers != 0)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		const struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas != 1)
			return -EINVAL;
		data = &buffers[i]->datas[0];
		if ((data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_MemFd) ||
				data->data == NULL || data->chunk == NULL ||
				data->maxsize < self->command_bytes ||
				(uintptr_t)data->data % _Alignof(double) != 0)
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++)
		self->input.buffers[i] = buffers[i];
	self->input.n_buffers = n_buffers;
	spa_buffer_latest_set_buffers(self->latest, buffers, n_buffers);
	return 0;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	if (self->started)
		return -EBUSY;
	return spa_buffer_latest_set_io(self->latest, id, data, size);
}

static int reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	(void)object;
	(void)port_id;
	(void)buffer_id;
	return -ENOTSUP;
}

static int send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;
	int res;

	spa_return_val_if_fail(self != NULL && command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!self->input.have_format || self->input.n_buffers == 0 ||
				!spa_buffer_latest_has_links(self->latest))
			return -EIO;
		if (self->started)
			return 0;
		if ((res = alpao_backend_start(self->backend, self->serial,
				self->actuator_count)) < 0)
			return res;
		if ((res = spa_buffer_latest_worker_begin(self->latest)) < 0) {
			(void)alpao_backend_stop(self->backend);
			return res;
		}
		self->started = true;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend: {
		int worker, backend;

		if (!self->started)
			return 0;
		self->started = false;
		worker = spa_buffer_latest_worker_end(self->latest);
		backend = alpao_backend_stop(self->backend);
		return worker < 0 ? worker : backend;
	}
	default:
		return -ENOTSUP;
	}
}

static int process_command(struct impl *self, uint32_t buffer_id)
{
	struct spa_buffer *buffer;
	const struct spa_data *data;
	const double *command;
	uint32_t offset, i;

	if (buffer_id >= self->input.n_buffers)
		return -EPROTO;
	buffer = self->input.buffers[buffer_id];
	if (buffer == NULL || buffer->n_datas != 1)
		return -EPROTO;
	data = &buffer->datas[0];
	if (data->data == NULL || data->chunk == NULL || data->maxsize == 0 ||
			data->chunk->size != self->command_bytes ||
			(data->chunk->stride != 0 &&
				data->chunk->stride != (int32_t)sizeof(double)) ||
			(data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0)
		return -EINVAL;
	offset = data->chunk->offset % data->maxsize;
	if (self->command_bytes > data->maxsize - offset)
		return -EINVAL;
	command = SPA_PTROFF(data->data, offset, const double);
	if ((uintptr_t)command % _Alignof(double) != 0)
		return -EINVAL;
	for (i = 0; i < self->actuator_count; i++)
		if (!isfinite(command[i]) || command[i] < -1.0 || command[i] > 1.0)
			return -ERANGE;
	return alpao_backend_send(self->backend, command, self->actuator_count);
}

static int process(void *object)
{
	struct impl *self = object;
	uint32_t buffer_id = SPA_ID_INVALID;
	uint64_t submission_sequence = 0;
	int dequeued, sent, completed;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	if (!self->started)
		return SPA_STATUS_OK;
	dequeued = spa_buffer_latest_try_dequeue(self->latest, &buffer_id,
			&submission_sequence);
	(void)submission_sequence;
	if (dequeued <= 0)
		return dequeued;
	sent = process_command(self, buffer_id);
	completed = spa_buffer_latest_queue(self->latest, buffer_id);
	if (completed < 0)
		return completed;
	if (sent < 0)
		return sent;
	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods node_methods = {
	.version = SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.set_callbacks = set_callbacks,
	.enum_params = enum_params,
	.set_param = set_param,
	.set_io = set_io,
	.send_command = send_command,
	.add_port = add_port,
	.remove_port = remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = reuse_buffer,
	.process = process,
};

static int get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *self = (struct impl *)handle;

	spa_return_val_if_fail(self != NULL && interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct impl *self = (struct impl *)handle;
	int result = 0;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	if (self->started) {
		int stopped;

		self->started = false;
		result = spa_buffer_latest_worker_end(self->latest);
		stopped = alpao_backend_stop(self->backend);
		if (result == 0)
			result = stopped;
	}
	spa_buffer_latest_destroy(self->latest);
	self->latest = NULL;
	alpao_backend_destroy(self->backend);
	self->backend = NULL;
	return result;
}

static size_t get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	(void)factory;
	(void)params;
	return sizeof(struct impl);
}

static void configure_node_props(struct impl *self)
{
	uint32_t count = 0;

	if (self->serial[0] == '\0')
		(void)copy_string(self->node_name, sizeof(self->node_name),
				"alpao_sink.mock");
	else
		(void)snprintf(self->node_name, sizeof(self->node_name),
				"alpao_sink.%s", self->serial);
#define ADD_ITEM(key, value) \
	self->node_items[count++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_API, "alpao");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Array/Sink");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "DeformableMirror");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION,
			"ALPAO normalized actuator command sink");
	ADD_ITEM(SPA_KEY_API_ALPAO_BACKEND, self->backend_name);
	ADD_ITEM(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, self->actuator_count_text);
	if (self->serial[0] != '\0')
		ADD_ITEM(SPA_KEY_API_ALPAO_SERIAL, self->serial);
#undef ADD_ITEM
	self->node_props = SPA_DICT_INIT(self->node_items, count);
}

static int init(const struct spa_handle_factory *factory,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *self;
	int res;

	(void)factory;
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	self = (struct impl *)handle;
	memset(self, 0, sizeof(*self));
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_hook_list_init(&self->hooks);
	if ((res = read_options(self, info)) < 0)
		return res;

	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_input_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RTC_PROCESS;
	configure_node_props(self);
	self->info.props = &self->node_props;

	self->input.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->input.info = SPA_PORT_INFO_INIT();
	self->input.params[0] = (struct spa_param_info) {
		.id = SPA_PARAM_EnumFormat,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->input.params[1] = (struct spa_param_info) {
		.id = SPA_PARAM_Format,
		.flags = SPA_PARAM_INFO_READWRITE,
	};
	self->input.params[2] = (struct spa_param_info) {
		.id = SPA_PARAM_Buffers,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->input.params[3] = (struct spa_param_info) {
		.id = SPA_PARAM_IO,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->input.info.params = self->input.params;
	self->input.info.n_params = SPA_N_ELEMENTS(self->input.params);

	self->latest = spa_buffer_latest_new(SPA_DIRECTION_INPUT, self, self->log);
	if (self->latest == NULL) {
		res = -errno;
		alpao_backend_destroy(self->backend);
		self->backend = NULL;
		return res;
	}
	return 0;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node },
};

static int enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	(void)factory;
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_alpao_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALPAO_SINK,
	NULL,
	get_size,
	init,
	enum_interface_info,
};

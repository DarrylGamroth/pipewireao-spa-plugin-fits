/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/loop.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/support/plugin.h>

#include "bgapi2.h"

#define REQUESTED_BUFFERS 8u
#define REQUESTED_FRAMES 10u

struct param_result {
	uint32_t expected;
	uint8_t *storage;
	size_t capacity;
	struct spa_pod *param;
	uint64_t node_flags;
	const char *readiness;
};

struct readiness_state {
	uint32_t ready_calls;
};

static int on_ready(void *data, int status)
{
	struct readiness_state *state = data;

	spa_assert_se(status == SPA_STATUS_HAVE_DATA);
	state->ready_calls++;
	return 0;
}

static const struct spa_node_callbacks node_callbacks = {
	.version = SPA_VERSION_NODE_CALLBACKS,
	.ready = on_ready,
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	void *payload;
};

static void on_result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct param_result *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = result;
	if (params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	if (size > capture->capacity) {
		capture->storage = realloc(capture->storage, size);
		spa_assert_se(capture->storage != NULL);
		capture->capacity = size;
	}
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static void on_info(void *data, const struct spa_node_info *info)
{
	struct param_result *capture = data;

	if (info->change_mask & SPA_NODE_CHANGE_MASK_FLAGS)
		capture->node_flags = info->flags;
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS)
		capture->readiness = spa_dict_lookup(info->props,
				SPA_KEY_API_BGAPI2_READINESS);
}

static bool props_have_values(struct spa_pod *props)
{
	struct spa_pod_object *object = (struct spa_pod_object *)props;
	struct spa_pod_prop *property;

	SPA_POD_OBJECT_FOREACH(object, property) {
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		const char *name = NULL;
		struct spa_pod *value = NULL;

		if (property->key != SPA_PROP_params)
			continue;
		spa_pod_parser_pod(&parser, &property->value);
		if (spa_pod_parser_push_struct(&parser, &frame) < 0)
			return false;
		return spa_pod_parser_get_string(&parser, &name) == 0 &&
				spa_pod_parser_get_pod(&parser, &value) == 0 &&
				name != NULL && value != NULL;
	}
	return false;
}

static struct spa_pod *find_control_value(struct spa_pod *props,
		const char *requested)
{
	struct spa_pod_object *object = (struct spa_pod_object *)props;
	struct spa_pod_prop *property;

	SPA_POD_OBJECT_FOREACH(object, property) {
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;

		if (property->key != SPA_PROP_params)
			continue;
		spa_pod_parser_pod(&parser, &property->value);
		spa_assert_se(spa_pod_parser_push_struct(&parser, &frame) == 0);
		for (;;) {
			const char *name = NULL;
			struct spa_pod *value = NULL;

			if (spa_pod_parser_get_string(&parser, &name) < 0)
				break;
			spa_assert_se(spa_pod_parser_get_pod(&parser, &value) == 0);
			if (spa_streq(name, requested))
				return value;
		}
	}
	return NULL;
}

static struct spa_pod *build_control_write(uint8_t *storage, size_t size,
		const char *name, const struct spa_pod *value)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, size);
	struct spa_pod_frame object, values;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, name);
	spa_pod_builder_primitive(&builder, value);
	spa_pod_builder_pop(&builder, &values);
	return spa_pod_builder_pop(&builder, &object);
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.info = on_info,
	.result = on_result,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct param_result *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static struct spa_pod *enum_node_one(struct spa_node *node,
		struct param_result *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static struct spa_pod *enum_node_at(struct spa_node *node,
		struct param_result *capture, uint32_t id, uint32_t start)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, start, 1, NULL) == 0);
	return capture->param;
}

static bool feature_is_writable(struct spa_node *node,
		struct param_result *capture, const char *requested)
{
	uint32_t start;

	for (start = 0;; start++) {
		struct spa_pod *info = enum_node_at(node, capture,
				SPA_PARAM_PropInfo, start);
		const char *name = NULL;
		bool writable = false;

		if (info == NULL)
			return false;
		if (spa_pod_parse_object(info, SPA_TYPE_OBJECT_PropInfo, NULL,
				SPA_PROP_INFO_name, SPA_POD_String(&name),
				SPA_PROP_INFO_params, SPA_POD_Bool(&writable)) >= 0 &&
				spa_streq(name, requested))
			return writable;
	}
}

static void init_buffer(struct test_buffer *storage, uint32_t size)
{
	storage->payload = calloc(1, size);
	spa_assert_se(storage->payload != NULL);
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.data = storage->payload;
	storage->data.maxsize = size;
	storage->data.fd = -1;
	storage->data.chunk = &storage->chunk;
	storage->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	storage->metas[1] = (struct spa_meta) {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage->acquisition),
		.data = &storage->acquisition,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int capture(const struct spa_handle_factory *factory,
		const char *producer, const char *readiness)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_PRODUCER, producer),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_BGAPI2_READINESS, readiness),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct test_buffer storage[REQUESTED_BUFFERS] = { 0 };
	struct spa_buffer *buffers[REQUESTED_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct param_result params = { .expected = SPA_ID_INVALID };
	struct readiness_state readiness_state = { 0 };
	struct spa_hook listener;
	struct pw_loop *loop = NULL;
	struct spa_support support[2];
	uint32_t n_support = 0;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_video_info_raw video = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_pod *format, *buffers_param, *prop_info, *props;
	struct spa_pod *scalar_value, *scalar_write, *width_value, *width_write;
	uint8_t scalar_write_storage[512], width_write_storage[512];
	const char *scalar_name = NULL;
	const char *property_name = NULL;
	bool width_writable;
	uint64_t deadline;
	int64_t last_pts = SPA_TIME_INVALID;
	int32_t payload_size = 0;
	uint32_t frames = 0, i;
	int res;

	if (spa_streq(readiness, "eventfd")) {
		loop = pw_loop_new(NULL);
		spa_assert_se(loop != NULL);
		pw_loop_enter(loop);
		support[n_support++] = (struct spa_support) {
			.type = SPA_TYPE_INTERFACE_DataLoop,
			.data = loop->loop,
		};
		support[n_support++] = (struct spa_support) {
			.type = SPA_TYPE_INTERFACE_DataSystem,
			.data = loop->system,
		};
	}

	handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(handle != NULL);
	res = factory->init(factory, handle, &info, support, n_support);
	if (res < 0) {
		free(handle);
		if (loop != NULL) {
			pw_loop_leave(loop);
			pw_loop_destroy(loop);
		}
		return 77;
	}
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&params) == 0);
	spa_assert_se(params.readiness != NULL &&
			spa_streq(params.readiness, readiness));
	spa_assert_se((params.node_flags & SPA_NODE_FLAG_POLL_DRIVER) ==
			(spa_streq(readiness, "poll") ?
					SPA_NODE_FLAG_POLL_DRIVER : 0));
	spa_assert_se(spa_node_set_callbacks(node, &node_callbacks,
			&readiness_state) == 0);
	width_writable = feature_is_writable(node, &params, "genicam.Width");
	prop_info = enum_node_one(node, &params, SPA_PARAM_PropInfo);
	spa_assert_se(spa_pod_parse_object(prop_info, SPA_TYPE_OBJECT_PropInfo, NULL,
			SPA_PROP_INFO_name, SPA_POD_String(&property_name)) >= 0);
	spa_assert_se(property_name != NULL &&
			strncmp(property_name, "genicam.", 8) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	spa_assert_se(props_have_values(props));
	for (i = 0; i < 3; i++) {
		static const char *const candidates[] = {
			"genicam.ExposureTime", "genicam.Gain",
			"genicam.AcquisitionFrameRate",
		};
		if ((scalar_value = find_control_value(props, candidates[i])) != NULL) {
			scalar_name = candidates[i];
			break;
		}
	}
	spa_assert_se(scalar_name != NULL && scalar_value != NULL);
	scalar_write = build_control_write(scalar_write_storage,
			sizeof(scalar_write_storage), scalar_name, scalar_value);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			scalar_write) == 0);
	format = enum_one(node, &params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);
	spa_assert_se(video.size.width > 0 && video.size.height > 0);
	buffers_param = enum_one(node, &params, SPA_PARAM_Buffers);
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size)) >= 0);
	spa_assert_se(payload_size > 0);
	for (i = 0; i < REQUESTED_BUFFERS; i++) {
		init_buffer(&storage[i], (uint32_t)payload_size);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, REQUESTED_BUFFERS) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	width_value = find_control_value(props, "genicam.Width");
	spa_assert_se(width_value != NULL);
	if (width_writable) {
		width_write = build_control_write(width_write_storage,
				sizeof(width_write_storage), "genicam.Width", width_value);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				width_write) == -EBUSY);
	}
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			scalar_write) == -EBUSY);
	deadline = monotonic_nsec() + 3 * SPA_NSEC_PER_SEC;
	while (frames < REQUESTED_FRAMES) {
		uint32_t id;

		if (loop != NULL)
			spa_assert_se(pw_loop_iterate(loop, 1000) >= 0);
		else {
			res = spa_node_process(node);
			spa_assert_se(res >= SPA_STATUS_OK);
		}
		if (io.status != SPA_STATUS_HAVE_DATA) {
			spa_assert_se(monotonic_nsec() < deadline);
			continue;
		}
		id = io.buffer_id;
		spa_assert_se(id < REQUESTED_BUFFERS);
		spa_assert_se(storage[id].chunk.size > 0 &&
				storage[id].chunk.size <= (uint32_t)payload_size);
		spa_assert_se(storage[id].header.seq > 0);
		spa_assert_se(storage[id].header.pts != SPA_TIME_INVALID);
		if (last_pts != SPA_TIME_INVALID)
			spa_assert_se(storage[id].header.pts > last_pts);
		last_pts = storage[id].header.pts;
		spa_assert_se(spa_meta_acquisition_is_valid(&storage[id].metas[1]));
		io.status = SPA_STATUS_NEED_DATA;
		frames++;
		if (frames == REQUESTED_FRAMES / 2u) {
			spa_assert_se(spa_node_send_command(node, &pause) == 0);
			spa_assert_se(spa_node_send_command(node, &start) == 0);
		}
	}
	printf("captured %u complete frames\n", frames);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, NULL) == 0);
	props = enum_node_one(node, &params, SPA_PARAM_Props);
	width_value = find_control_value(props, "genicam.Width");
	spa_assert_se(width_value != NULL);
	if (width_writable) {
		width_write = build_control_write(width_write_storage,
				sizeof(width_write_storage), "genicam.Width", width_value);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				width_write) == 0);
	}
	format = enum_one(node, &params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	if (loop != NULL) {
		spa_assert_se(readiness_state.ready_calls >= REQUESTED_FRAMES);
		pw_loop_leave(loop);
		pw_loop_destroy(loop);
	}
	free(params.storage);
	for (i = 0; i < REQUESTED_BUFFERS; i++)
		free(storage[i].payload);
	return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	void *library;
	int res;

	spa_assert_se(argc == 3 || argc == 4);
	pw_init(&argc, &argv);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	while (enumerate(&factory, &index) > 0)
		if (spa_streq(factory->name, SPA_NAME_API_BGAPI2_SOURCE))
			break;
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_BGAPI2_SOURCE));
	res = capture(factory, argv[2], argc == 4 ? argv[3] : "poll");
	spa_assert_se(dlclose(library) == 0);
	pw_deinit();
	return res;
}

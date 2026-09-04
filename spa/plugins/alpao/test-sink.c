/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/ndarray-utils.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/alpao.h>

#define ACTUATOR_COUNT 8u

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct param_capture {
	uint32_t expected;
	uint8_t storage[4096];
	struct spa_pod *param;
	uint64_t port_flags;
	uint32_t error_events;
};

static void on_port_info(void *data, enum spa_direction direction,
		uint32_t port_id, const struct spa_port_info *info)
{
	struct param_capture *capture = data;

	if (direction == SPA_DIRECTION_INPUT && port_id == 0 && info != NULL)
		capture->port_flags = info->flags;
}

static void on_result(void *data, int seq, int result, uint32_t type,
		const void *value)
{
	struct param_capture *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	(void)seq;
	spa_assert_se(result >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = value;
	if (params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static void on_event(void *data, const struct spa_event *event)
{
	struct param_capture *capture = data;

	if (SPA_NODE_EVENT_ID(event) == SPA_NODE_EVENT_Error)
		capture->error_events++;
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.port_info = on_port_info,
	.result = on_result,
	.event = on_event,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct param_capture *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_INPUT, 0,
			id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static struct spa_pod *enum_node_one(struct spa_node *node,
		struct param_capture *capture, uint32_t id, uint32_t start)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, start, 1, NULL) == 0);
	return capture->param;
}

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(double) double command[ACTUATOR_COUNT];
};

static void init_test_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = sizeof(storage->command);
	storage->data.data = storage->command;
	storage->data.chunk = &storage->chunk;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static struct spa_pod *build_format(uint8_t *storage, size_t size,
		const char *schema, uint32_t object_id)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { (int32_t)ACTUATOR_COUNT };
	const struct spa_fraction rate = SPA_FRACTION(1000, 1);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, object_id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema, SPA_POD_String(schema),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_F64_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate));
}

static int exercise(const struct spa_handle_factory *factory,
		const char *backend)
{
	const char *serial = spa_streq(backend, "asdk") ? "SIM001" : "";
	struct spa_dict_item items[5] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, backend),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_SERIAL, serial),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "8"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, profile),
	};
	uint32_t n_items = 4;
	struct spa_dict info;
	size_t size;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_hook listener;
	struct param_capture capture = { .expected = SPA_ID_INVALID };
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	uint8_t format_storage[1024];
	struct spa_pod *format, *wrong_schema;
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_io_position position = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_pod *node_io;
	uint32_t node_io_id = SPA_ID_INVALID;
	int32_t node_io_size = 0;
	int initialized;

	if (spa_streq(backend, "mock"))
		items[n_items++] = SPA_DICT_ITEM_INIT(
				SPA_KEY_API_ALPAO_DAQ_FREQUENCY, "20000");
	info = SPA_DICT_INIT(items, n_items);
	size = spa_handle_factory_get_size(factory, &info);
	handle = calloc(1, size);
	spa_assert_se(handle != NULL);
	initialized = spa_handle_factory_init(factory, handle, &info, NULL, 0);
	if (initialized == -ENOTSUP && spa_streq(backend, "asdk")) {
		free(handle);
		return 77;
	}
	spa_assert_se(initialized == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	spa_assert_se(capture.port_flags ==
			(SPA_PORT_FLAG_NO_REF | SPA_PORT_FLAG_TERMINAL));
	node_io = enum_node_one(node, &capture, SPA_PARAM_IO, 0);
	spa_assert_se(node_io != NULL);
	spa_assert_se(spa_pod_parse_object(node_io,
			SPA_TYPE_OBJECT_ParamIO, NULL,
			SPA_PARAM_IO_id, SPA_POD_Id(&node_io_id),
			SPA_PARAM_IO_size, SPA_POD_Int(&node_io_size)) >= 0);
	spa_assert_se(node_io_id == SPA_IO_Position);
	spa_assert_se(node_io_size == (int32_t)sizeof(struct spa_io_position));
	spa_assert_se(enum_node_one(node, &capture, SPA_PARAM_IO, 1) == NULL);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, &position,
			sizeof(position) - 1u) == -EINVAL);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, &position,
			sizeof(position)) == 0);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, NULL, 0) == 0);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Clock, NULL, 0) == -ENOENT);

	format = enum_one(node, &capture, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_format_ndarray_parse(format, &ndarray) == 0);
	spa_assert_se(ndarray.element_type == SPA_ELEMENT_TYPE_F64_LE);
	spa_assert_se(ndarray.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	spa_assert_se(ndarray.n_dimensions == 1);
	spa_assert_se(ndarray.shape[0] == ACTUATOR_COUNT);

	wrong_schema = build_format(format_storage, sizeof(format_storage),
			"org.pipewireao.test.wrong/1", SPA_PARAM_EnumFormat);
	capture.expected = SPA_PARAM_EnumFormat;
	capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_EnumFormat, 0, 1, wrong_schema) == 0);
	spa_assert_se(capture.param == NULL);
	wrong_schema = build_format(format_storage, sizeof(format_storage),
			"org.pipewireao.test.wrong/1", SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, wrong_schema) == -EINVAL);
	format = build_format(format_storage, sizeof(format_storage),
			SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND, SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	init_test_buffer(&storage[0]);
	init_test_buffer(&storage[1]);
	buffers[0] = &storage[0].buffer;
	buffers[1] = &storage[1].buffer;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);

	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	storage[0].chunk.size = sizeof(storage[0].command);
	storage[0].chunk.stride = sizeof(storage[0].command);
	io.buffer_id = 0;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(io.status == SPA_STATUS_NEED_DATA);

	storage[1].command[3] = 1.5;
	storage[1].chunk.size = sizeof(storage[1].command);
	storage[1].chunk.stride = sizeof(double);
	io.buffer_id = 1;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == -ERANGE);
	spa_assert_se(io.status == -ERANGE);
	spa_assert_se(capture.error_events == 1);

	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			NULL, 0) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, NULL) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
	return 0;
}

static void expect_invalid_daq_frequency(
		const struct spa_handle_factory *factory)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "8"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_DAQ_FREQUENCY, "999"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, profile),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	const size_t size = spa_handle_factory_get_size(factory, &info);
	struct spa_handle *handle = calloc(1, size);

	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &info,
			NULL, 0) == -EINVAL);
	free(handle);
}

static void expect_invalid_profile(const struct spa_handle_factory *factory)
{
	const struct spa_dict_item invalid_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "8"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, "sha256:bad"),
	};
	const struct spa_dict invalid =
			SPA_DICT_INIT(invalid_items, SPA_N_ELEMENTS(invalid_items));
	const struct spa_dict_item missing_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "8"),
	};
	const struct spa_dict missing =
			SPA_DICT_INIT(missing_items, SPA_N_ELEMENTS(missing_items));
	struct spa_handle *handle;

	handle = calloc(1, spa_handle_factory_get_size(factory, &invalid));
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &invalid,
			NULL, 0) == -EINVAL);
	free(handle);
	handle = calloc(1, spa_handle_factory_get_size(factory, &missing));
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &missing,
			NULL, 0) == -EINVAL);
	free(handle);
}

static void expect_mock_unavailable(const struct spa_handle_factory *factory)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "8"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, profile),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	const size_t size = spa_handle_factory_get_size(factory, &info);
	struct spa_handle *handle = calloc(1, size);

	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &info,
			NULL, 0) == -ENOTSUP);
	free(handle);
}

int main(int argc, char **argv)
{
	void *library, *symbol;
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	int result = 0;

	spa_assert_se(argc == 3);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	spa_assert_se(sizeof(enumerate) == sizeof(symbol));
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL);
	spa_assert_se(spa_streq(factory->name, SPA_NAME_API_ALPAO_SINK));
	spa_assert_se(enumerate(&factory, &index) == 0);
	expect_invalid_profile(factory);
	if (spa_streq(argv[2], "factory"))
		expect_mock_unavailable(factory);
	else {
		if (spa_streq(argv[2], "mock"))
			expect_invalid_daq_frequency(factory);
		result = exercise(factory, argv[2]);
	}
	spa_assert_se(dlclose(library) == 0);
	return result;
}

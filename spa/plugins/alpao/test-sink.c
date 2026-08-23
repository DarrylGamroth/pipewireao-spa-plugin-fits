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
};

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

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
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
		const char *schema, const char *selected_profile, uint32_t object_id)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { (int32_t)ACTUATOR_COUNT };

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
			SPA_FORMAT_NDARRAY_profile, SPA_POD_String(selected_profile));
}

static struct spa_pod *build_format_without_profile(uint8_t *storage,
		size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { (int32_t)ACTUATOR_COUNT };

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String(SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_F64_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR));
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
	struct spa_pod *format, *missing_profile, *wrong_schema, *wrong_profile;
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers_latest io = { 0 };
	struct spa_io_buffers_latest_link link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &io,
		.notify_fd = -1,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint32_t completed = SPA_ID_INVALID;
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

	format = enum_one(node, &capture, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_format_ndarray_parse(format, &ndarray) == 0);
	spa_assert_se(ndarray.element_type == SPA_ELEMENT_TYPE_F64_LE);
	spa_assert_se(ndarray.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	spa_assert_se(ndarray.n_dimensions == 1);
	spa_assert_se(ndarray.shape[0] == ACTUATOR_COUNT);

	wrong_schema = build_format(format_storage, sizeof(format_storage),
			"org.pipewireao.test.wrong/1", profile, SPA_PARAM_EnumFormat);
	capture.expected = SPA_PARAM_EnumFormat;
	capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_EnumFormat, 0, 1, wrong_schema) == 0);
	spa_assert_se(capture.param == NULL);
	wrong_schema = build_format(format_storage, sizeof(format_storage),
			"org.pipewireao.test.wrong/1", profile, SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, wrong_schema) == -EINVAL);
	wrong_profile = build_format(format_storage, sizeof(format_storage),
			SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND,
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, wrong_profile) == -EINVAL);
	missing_profile = build_format_without_profile(format_storage,
			sizeof(format_storage));
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, missing_profile) == -EINVAL);

	format = enum_one(node, &capture, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	init_test_buffer(&storage[0]);
	init_test_buffer(&storage[1]);
	buffers[0] = &storage[0].buffer;
	buffers[1] = &storage[1].buffer;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);

	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	storage[0].chunk.size = sizeof(storage[0].command);
	storage[0].chunk.stride = sizeof(double);
	spa_assert_se(spa_io_buffers_latest_submit(&io, 1, 0, NULL, NULL) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(spa_io_buffers_latest_reclaim_completion(&io,
			&completed) == 0);
	spa_assert_se(completed == 0);

	storage[1].command[3] = 1.5;
	storage[1].chunk.size = sizeof(storage[1].command);
	storage[1].chunk.stride = sizeof(double);
	spa_assert_se(spa_io_buffers_latest_submit(&io, 2, 1, NULL, NULL) == 0);
	spa_assert_se(spa_node_process(node) == -ERANGE);
	spa_assert_se(spa_io_buffers_latest_reclaim_completion(&io,
			&completed) == 0);
	spa_assert_se(completed == 1);

	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
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

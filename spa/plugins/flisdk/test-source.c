/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/props.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>

#include "flisdk.h"

#define N_BUFFERS 3u
#define FRAME_SIZE 12u

struct capture {
	uint32_t expected;
	uint8_t storage[65536];
	struct spa_pod *param;
	uint64_t node_flags;
	const char *readiness;
	const char *camera;
	const char *pixel_sign;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	uint8_t payload[FRAME_SIZE];
};

static void on_info(void *data, const struct spa_node_info *info)
{
	struct capture *capture = data;

	if (info->change_mask & SPA_NODE_CHANGE_MASK_FLAGS)
		capture->node_flags = info->flags;
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS) {
		capture->readiness = spa_dict_lookup(info->props,
				SPA_KEY_API_FLISDK_READINESS);
		capture->camera = spa_dict_lookup(info->props,
				SPA_KEY_API_FLISDK_CAMERA);
		capture->pixel_sign = spa_dict_lookup(info->props,
				SPA_KEY_API_FLISDK_PIXEL_SIGN);
	}
}

static void on_result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct capture *capture = data;
	const struct spa_result_node_params *params = result;
	uint32_t size;

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS ||
			params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.info = on_info,
	.result = on_result,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct capture *capture, uint32_t id)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static struct spa_pod *enum_node_one(struct spa_node *node,
		struct capture *capture, uint32_t id, uint32_t index)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, index, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
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

static void init_buffer(struct test_buffer *storage)
{
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.data = storage->payload;
	storage->data.maxsize = sizeof(storage->payload);
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

static void check_frame(struct test_buffer *buffer, uint64_t sequence,
		bool discontinuity)
{
	struct spa_meta acquisition_meta = buffer->metas[1];
	uint32_t i;

	spa_assert_se(buffer->chunk.offset == 0);
	spa_assert_se(buffer->chunk.size == FRAME_SIZE);
	spa_assert_se(buffer->chunk.stride == 4);
	spa_assert_se(buffer->chunk.flags == 0);
	spa_assert_se(buffer->header.seq == sequence);
	spa_assert_se(buffer->header.pts >= 0);
	spa_assert_se(!!(buffer->header.flags & SPA_META_HEADER_FLAG_DISCONT) ==
			discontinuity);
	spa_assert_se(spa_meta_acquisition_is_valid(&acquisition_meta));
	for (i = 0; i < FRAME_SIZE; i++)
		spa_assert_se(buffer->payload[i] == (uint8_t)(sequence + i));
}

int main(int argc, char *argv[])
{
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FLISDK_CAMERA, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FLISDK_GRABBER, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FLISDK_CONTROL, "clprotocol"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FLISDK_CLPROTOCOL_LIBRARIES, NULL),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FLISDK_CAMERA_SERIAL, "CRED2-MOCK-001"),
	};
	struct spa_dict info = SPA_DICT_INIT(items, 2);
	struct test_buffer storage[N_BUFFERS] = { 0 };
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_video_info_raw video = { 0 };
	struct capture capture = { 0 };
	struct spa_hook listener;
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	uint32_t index = 0, i, first_id;
	void *library;

	spa_assert_se(argc == 2 || argc == 3);
	if (argc == 3) {
		items[3].value = argv[2];
		info.n_items = SPA_N_ELEMENTS(items);
	}
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, handle, &info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	spa_assert_se((capture.node_flags & (SPA_NODE_FLAG_RT |
			SPA_NODE_FLAG_POLL_DRIVER)) ==
			(SPA_NODE_FLAG_RT | SPA_NODE_FLAG_POLL_DRIVER));
	spa_assert_se(spa_streq(capture.readiness, "poll"));
	spa_assert_se(spa_streq(capture.camera, "mock"));
	spa_assert_se(spa_streq(capture.pixel_sign, "unsigned"));
	if (argc == 3) {
		struct spa_pod *props, *value, *write;
		struct spa_pod_bool bool_value = SPA_POD_INIT_Bool(true);
		struct spa_pod_long long_value = SPA_POD_INIT_Long(320);
		uint8_t write_storage[512];
		const char *property_name = NULL;

		spa_assert_se(spa_pod_parse_object(enum_node_one(node, &capture,
				SPA_PARAM_PropInfo, 0), SPA_TYPE_OBJECT_PropInfo, NULL,
				SPA_PROP_INFO_name, SPA_POD_String(&property_name)) >= 0);
		spa_assert_se(property_name != NULL &&
				strncmp(property_name, "genicam.", 8) == 0);
		props = enum_node_one(node, &capture, SPA_PARAM_Props, 0);
		value = find_control_value(props, "genicam.AcquisitionFrameRate");
		spa_assert_se(value != NULL);
		write = build_control_write(write_storage, sizeof(write_storage),
				"genicam.AcquisitionFrameRate", value);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, write) == 0);
		write = build_control_write(write_storage, sizeof(write_storage),
				"genicam-command.TriggerSoftware", &bool_value.pod);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, write) == 0);
		spa_assert_se(spa_format_video_raw_parse(enum_one(node, &capture,
				SPA_PARAM_EnumFormat), &video) >= 0);
		spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_Format, 0, capture.param) == 0);
		for (i = 0; i < N_BUFFERS; i++) {
			init_buffer(&storage[i]);
			buffers[i] = &storage[i].buffer;
		}
		spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
				buffers, N_BUFFERS) == 0);
		write = build_control_write(write_storage, sizeof(write_storage),
				"genicam.Width", &long_value.pod);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, write) == -EBUSY);
		spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
				NULL, 0) == 0);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, write) == 0);
		spa_assert_se(spa_format_video_raw_parse(enum_one(node, &capture,
				SPA_PARAM_EnumFormat), &video) >= 0);
		spa_assert_se(video.size.width == 320 && video.size.height == 3);
		spa_hook_remove(&listener);
		spa_assert_se(spa_handle_clear(handle) == 0);
		free(handle);
		spa_assert_se(dlclose(library) == 0);
		return 0;
	}
	spa_assert_se(spa_format_video_raw_parse(enum_one(node, &capture,
			SPA_PARAM_EnumFormat), &video) >= 0);
	spa_assert_se(video.format == SPA_VIDEO_FORMAT_GRAY8);
	spa_assert_se(video.size.width == 4 && video.size.height == 3);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, capture.param) == 0);
	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i]);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA && io.buffer_id < N_BUFFERS);
	first_id = io.buffer_id;
	check_frame(&storage[first_id], 1, true);
	io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA && io.buffer_id < N_BUFFERS);
	spa_assert_se(io.buffer_id != first_id);
	check_frame(&storage[io.buffer_id], 2, false);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA && io.buffer_id < N_BUFFERS);
	check_frame(&storage[io.buffer_id], 1, true);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}

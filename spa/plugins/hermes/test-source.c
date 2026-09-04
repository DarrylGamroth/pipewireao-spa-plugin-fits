/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/ndarray-utils.h>
#include <spa/support/plugin.h>

#include "hermes.h"

#define N_BUFFERS 3u
#define PAYLOAD_SIZE (64u * 32u)

struct capture {
	uint32_t expected;
	uint8_t storage[2048];
	struct spa_pod *param;
	uint64_t node_flags;
	const char *readiness;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	uint8_t payload[PAYLOAD_SIZE];
};

static void on_info(void *data, const struct spa_node_info *info)
{
	struct capture *capture = data;

	if (info->change_mask & SPA_NODE_CHANGE_MASK_FLAGS)
		capture->node_flags = info->flags;
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS)
		capture->readiness = spa_dict_lookup(info->props,
				SPA_KEY_API_HERMES_READINESS);
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

static const char *format_string(const struct spa_pod *format, uint32_t key)
{
	const struct spa_pod_prop *property = spa_pod_find_prop(format, NULL, key);
	const char *value = NULL;

	spa_assert_se(property != NULL);
	spa_assert_se(spa_pod_get_string(&property->value, &value) == 0);
	return value;
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

static void check_batch(struct test_buffer *buffer, uint64_t sequence,
		bool discontinuity)
{
	struct spa_meta acquisition_meta = buffer->metas[1];
	uint32_t i;

	spa_assert_se(buffer->chunk.offset == 0);
	spa_assert_se(buffer->chunk.size == PAYLOAD_SIZE);
	spa_assert_se(buffer->chunk.stride == PAYLOAD_SIZE);
	spa_assert_se(buffer->chunk.flags == 0);
	spa_assert_se(buffer->header.seq == sequence);
	spa_assert_se(buffer->header.pts >= 0);
	spa_assert_se(!!(buffer->header.flags & SPA_META_HEADER_FLAG_DISCONT) ==
			discontinuity);
	spa_assert_se(spa_meta_acquisition_is_valid(&acquisition_meta));
	for (i = 0; i < PAYLOAD_SIZE; i++)
		spa_assert_se(buffer->payload[i] == (uint8_t)(sequence + i));
}

int main(int argc, char *argv[])
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HERMES_DEVICE_ID, "mock"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HERMES_FRAMES_PER_BUFFER, "1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HERMES_COUNTERS, "1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HERMES_BITS_PER_PIXEL, "8"),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct test_buffer storage[N_BUFFERS] = { 0 };
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	struct capture capture = { 0 };
	struct spa_hook listener;
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_pod *format;
	uint32_t index = 0, i, first_id;
	void *library;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(spa_streq(factory->name, SPA_NAME_API_HERMES_SOURCE));
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
	format = enum_one(node, &capture, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_format_ndarray_parse(format, &ndarray) == 0);
	spa_assert_se(ndarray.element_type == SPA_ELEMENT_TYPE_U8);
	spa_assert_se(ndarray.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	spa_assert_se(ndarray.n_dimensions == 3);
	spa_assert_se(ndarray.shape[0] == 1 && ndarray.shape[1] == 1 &&
			ndarray.shape[2] == PAYLOAD_SIZE);
	spa_assert_se(ndarray.rate.num == 2000 && ndarray.rate.denom == 1);
	spa_assert_se(spa_streq(format_string(format, SPA_FORMAT_NDARRAY_schema),
			SPA_HERMES_RAW_BATCH_SCHEMA));
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
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
	check_batch(&storage[first_id], 1, true);
	io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA && io.buffer_id < N_BUFFERS);
	spa_assert_se(io.buffer_id != first_id);
	check_batch(&storage[io.buffer_id], 2, false);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}

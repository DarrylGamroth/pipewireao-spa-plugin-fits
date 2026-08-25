/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/node/buffer-latest.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/calculon.h>

#define WIDTH 4u
#define HEIGHT 4u
#define BLOCK_ROWS 2u
#define PIXELS (WIDTH * HEIGHT)
#define RAW_BYTES (PIXELS * sizeof(uint16_t))
#define BLOCK_BYTES (WIDTH * BLOCK_ROWS * sizeof(float))
#define FRAME_BYTES (PIXELS * sizeof(float))

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct capture {
	uint32_t expected;
	uint8_t storage[4096];
	struct spa_pod *param;
};

struct instance {
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_hook listener;
	struct capture capture;
};

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_progressive progressive;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(8) uint8_t payload[FRAME_BYTES];
};

static void on_result(void *data, int seq SPA_UNUSED, int result,
		uint32_t type, const void *value)
{
	struct capture *capture = data;
	const struct spa_result_node_params *params = value;
	uint32_t size;

	spa_assert_se(result >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS ||
			params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static const struct spa_handle_factory *find_factory(
		spa_handle_factory_enum_func_t enumerate, const char *name)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;

	while (enumerate(&factory, &index) == 1)
		if (spa_streq(factory->name, name))
			return factory;
	return NULL;
}

static void make_node(struct instance *instance,
		const struct spa_handle_factory *factory, const struct spa_dict *info)
{
	memset(instance, 0, sizeof(*instance));
	instance->capture.expected = SPA_ID_INVALID;
	instance->handle = calloc(1, factory->get_size(factory, info));
	spa_assert_se(instance->handle != NULL);
	spa_assert_se(factory->init(factory, instance->handle, info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(instance->handle,
			SPA_TYPE_INTERFACE_Node, (void **)&instance->node) == 0);
	spa_assert_se(spa_node_add_listener(instance->node, &instance->listener,
			&events, &instance->capture) == 0);
}

static struct spa_pod *enum_format(struct instance *instance,
		enum spa_direction direction)
{
	instance->capture.expected = SPA_PARAM_EnumFormat;
	instance->capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(instance->node, 1, direction, 0,
			SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_assert_se(instance->capture.param != NULL);
	return instance->capture.param;
}

static void configure(struct instance *instance, enum spa_direction direction)
{
	struct spa_pod *format = enum_format(instance, direction);

	spa_assert_se(spa_node_port_set_param(instance->node, direction, 0,
			SPA_PARAM_Format, 0, format) == 0);
}

static void negotiate(struct instance *output, struct instance *input)
{
	uint8_t storage[8192];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod *negotiated = NULL;

	spa_assert_se(spa_pod_filter(&builder, &negotiated,
			enum_format(output, SPA_DIRECTION_OUTPUT),
			enum_format(input, SPA_DIRECTION_INPUT)) == 0);
	spa_assert_se(negotiated != NULL);
	spa_assert_se(spa_node_port_set_param(output->node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, negotiated) == 0);
	spa_assert_se(spa_node_port_set_param(input->node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, negotiated) == 0);
}

static void init_buffer(struct buffer *buffer, uint32_t size, int32_t stride,
		bool progressive)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(buffer->header),
		.data = &buffer->header,
	};
	buffer->metas[1] = (struct spa_meta) {
		.type = SPA_META_Progressive,
		.size = sizeof(buffer->progressive),
		.data = &buffer->progressive,
	};
	buffer->data.type = SPA_DATA_MemPtr;
	buffer->data.fd = -1;
	buffer->data.data = buffer->payload;
	buffer->data.maxsize = sizeof(buffer->payload);
	buffer->data.chunk = &buffer->chunk;
	buffer->chunk.size = size;
	buffer->chunk.stride = stride;
	buffer->buffer.n_metas = progressive ? 2 : 1;
	buffer->buffer.metas = buffer->metas;
	buffer->buffer.n_datas = 1;
	buffer->buffer.datas = &buffer->data;
}

static void use_buffer(struct instance *instance, enum spa_direction direction,
		struct buffer *buffer)
{
	struct spa_buffer *buffers[] = { &buffer->buffer };

	spa_assert_se(spa_node_port_use_buffers(instance->node, direction, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
}

static void destroy(struct instance *instance)
{
	spa_hook_remove(&instance->listener);
	spa_assert_se(instance->handle->clear(instance->handle) == 0);
	free(instance->handle);
}

int main(int argc, char **argv)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_SIZE, "4x4"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_ROW_BLOCK_ROWS, "2"),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *pixel_factory, *assembly_factory;
	struct instance pixel, assembly;
	struct buffer raw, block, frame;
	struct spa_buffer *raw_buffers[] = { &raw.buffer };
	struct spa_buffer_latest *producer;
	struct spa_io_buffers_latest latest_io = { 0 };
	struct spa_io_buffers_latest_link latest_link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest_io,
		.notify_fd = -1,
	};
	struct spa_io_buffers block_io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_io_buffers frame_io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint16_t *raw_values;
	const float *frame_values;
	uint32_t raw_id, i;
	void *library, *symbol;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	pixel_factory = find_factory(enumerate,
			SPA_NAME_API_CALCULON_PIXEL_CALIBRATION);
	assembly_factory = find_factory(enumerate,
			SPA_NAME_API_CALCULON_FRAME_ASSEMBLY);
	spa_assert_se(pixel_factory != NULL && assembly_factory != NULL);
	make_node(&pixel, pixel_factory, &info);
	make_node(&assembly, assembly_factory, &info);
	configure(&pixel, SPA_DIRECTION_INPUT);
	negotiate(&pixel, &assembly);
	configure(&assembly, SPA_DIRECTION_OUTPUT);

	init_buffer(&raw, RAW_BYTES, WIDTH * sizeof(uint16_t), true);
	init_buffer(&block, BLOCK_BYTES, WIDTH * sizeof(float), false);
	init_buffer(&frame, FRAME_BYTES, WIDTH * sizeof(float), false);
	use_buffer(&pixel, SPA_DIRECTION_INPUT, &raw);
	use_buffer(&pixel, SPA_DIRECTION_OUTPUT, &block);
	use_buffer(&assembly, SPA_DIRECTION_INPUT, &block);
	use_buffer(&assembly, SPA_DIRECTION_OUTPUT, &frame);
	producer = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, NULL, NULL);
	spa_assert_se(producer != NULL);
	spa_buffer_latest_set_buffers(producer, raw_buffers,
			SPA_N_ELEMENTS(raw_buffers));
	spa_assert_se(spa_buffer_latest_set_io(producer,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &block_io, sizeof(block_io)) == 0);
	spa_assert_se(spa_node_port_set_io(assembly.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &block_io, sizeof(block_io)) == 0);
	spa_assert_se(spa_node_port_set_io(assembly.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &frame_io, sizeof(frame_io)) == 0);

	spa_assert_se(spa_node_send_command(assembly.node, &start) == 0);
	spa_assert_se(spa_node_send_command(pixel.node, &start) == 0);
	spa_assert_se(spa_buffer_latest_worker_begin(producer) == 0);
	spa_assert_se(spa_buffer_latest_dequeue(producer, &raw_id, NULL) == 1);
	spa_assert_se(raw_id == 0);
	raw_values = (uint16_t *)raw.payload;
	for (i = 0; i < PIXELS; i++)
		raw_values[i] = (uint16_t)(i + 1u);
	raw.header.flags = SPA_META_HEADER_FLAG_DISCONT;
	raw.header.seq = 42;
	raw.header.pts = 123456;
	spa_assert_se(spa_meta_progressive_init(&raw.progressive, 0, 0,
			RAW_BYTES, BLOCK_ROWS * WIDTH * sizeof(uint16_t)));
	spa_meta_progressive_store_release(&raw.progressive,
			spa_meta_progressive_snapshot_encode(RAW_BYTES / 2u,
					SPA_META_PROGRESSIVE_STATE_ACTIVE));
	spa_assert_se(spa_buffer_latest_begin_progressive(producer, raw_id) == 0);
	spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(block.header.seq == 42 && block.header.offset == 0);
	spa_assert_se((block.header.flags & SPA_META_HEADER_FLAG_DISCONT) != 0);
	spa_assert_se((block.header.flags & SPA_META_HEADER_FLAG_MARKER) == 0);
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_NEED_DATA);

	spa_meta_progressive_store_release(&raw.progressive,
			spa_meta_progressive_snapshot_encode(RAW_BYTES,
					SPA_META_PROGRESSIVE_STATE_COMPLETE));
	spa_assert_se(spa_buffer_latest_end_progressive(producer, raw_id) == 0);
	spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(block.header.seq == 42 && block.header.offset == BLOCK_ROWS);
	spa_assert_se((block.header.flags & SPA_META_HEADER_FLAG_MARKER) != 0);
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(frame_io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(frame.header.seq == 42 && frame.header.offset == 0);
	spa_assert_se((frame.header.flags & SPA_META_HEADER_FLAG_DISCONT) != 0);
	spa_assert_se((frame.header.flags & SPA_META_HEADER_FLAG_MARKER) != 0);
	frame_values = (const float *)frame.payload;
	for (i = 0; i < PIXELS; i++)
		spa_assert_se(frame_values[i] == (float)(i + 1u));

	/* A malformed partial frame is consumed and abandoned. The next complete
	 * frame remains usable and advertises the loss as a discontinuity. */
	frame_io.status = SPA_STATUS_NEED_DATA;
	for (i = 0; i < WIDTH * BLOCK_ROWS; i++)
		((float *)block.payload)[i] = (float)(100u + i);
	block.header.flags = 0;
	block.header.seq = 50;
	block.header.offset = 0;
	block_io.buffer_id = 0;
	block_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_NEED_DATA);
	block.header.offset = 3;
	block_io.buffer_id = 0;
	block_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_NEED_DATA);

	for (i = 0; i < WIDTH * BLOCK_ROWS; i++)
		((float *)block.payload)[i] = (float)(200u + i);
	block.header.flags = 0;
	block.header.seq = 51;
	block.header.offset = 0;
	block_io.buffer_id = 0;
	block_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_NEED_DATA);
	for (i = 0; i < WIDTH * BLOCK_ROWS; i++)
		((float *)block.payload)[i] = (float)(300u + i);
	block.header.flags = SPA_META_HEADER_FLAG_MARKER;
	block.header.offset = BLOCK_ROWS;
	block_io.buffer_id = 0;
	block_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(assembly.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(frame.header.seq == 51 && frame.header.offset == 0);
	spa_assert_se((frame.header.flags & SPA_META_HEADER_FLAG_DISCONT) != 0);
	spa_assert_se((frame.header.flags & SPA_META_HEADER_FLAG_MARKER) != 0);

	spa_assert_se(spa_node_send_command(pixel.node, &pause) == 0);
	spa_assert_se(spa_node_send_command(assembly.node, &pause) == 0);
	spa_assert_se(spa_buffer_latest_worker_end(producer) == 0);
	latest_link.flags = 0;
	latest_link.io = NULL;
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_buffer_latest_set_io(producer,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_buffer_latest_destroy(producer);
	destroy(&assembly);
	destroy(&pixel);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}

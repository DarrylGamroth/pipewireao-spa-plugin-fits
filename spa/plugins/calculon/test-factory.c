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
#include <spa/param/props.h>
#include <spa/param/video/raw-utils.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/calculon.h>

#define WIDTH 8u
#define HEIGHT 6u
#define PIXELS (WIDTH * HEIGHT)

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

/* glibc interposition observes allocations made by the loaded Rust cdylib. */
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *memory, size_t size);

static bool measure_allocations;
static uint64_t measured_allocations;

void *malloc(size_t size)
{
	void *memory = __libc_malloc(size);
	if (measure_allocations && memory != NULL)
		measured_allocations++;
	return memory;
}

void *calloc(size_t count, size_t size)
{
	void *memory = __libc_calloc(count, size);
	if (measure_allocations && memory != NULL)
		measured_allocations++;
	return memory;
}

void *realloc(void *previous, size_t size)
{
	void *memory = __libc_realloc(previous, size);
	if (measure_allocations && memory != NULL)
		measured_allocations++;
	return memory;
}

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
		struct param_capture *capture, enum spa_direction direction,
		uint32_t port_id)
{
	capture->expected = SPA_PARAM_EnumFormat;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, direction, port_id,
			SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
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

static void expect_ndarray(struct spa_node *node,
		struct param_capture *capture, enum spa_direction direction,
		uint32_t port_id, const char *schema, bool has_rate)
{
	struct spa_ndarray_info info = SPA_NDARRAY_INFO_INIT();
	struct spa_pod *format = enum_one(node, capture, direction, port_id);

	spa_assert_se(spa_format_ndarray_parse(format, &info) == 0);
	spa_assert_se(info.element_type == SPA_ELEMENT_TYPE_F32_LE);
	spa_assert_se(info.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	spa_assert_se(info.n_dimensions == 2);
	spa_assert_se(info.shape[0] == HEIGHT && info.shape[1] == WIDTH);
	spa_assert_se((info.rate.denom != 0) == has_rate);
	if (has_rate)
		spa_assert_se(info.rate.num == 1000 && info.rate.denom == 1);
	spa_assert_se(spa_streq(format_string(format,
			SPA_FORMAT_NDARRAY_schema), schema));
	spa_assert_se(spa_streq(format_string(format,
			SPA_FORMAT_NDARRAY_profile), profile));
}

static void expect_invalid_info(const struct spa_handle_factory *factory,
		const struct spa_dict *info)
{
	const size_t size = spa_handle_factory_get_size(factory, info);
	struct spa_handle *handle = calloc(1, size);

	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, info, NULL, 0) ==
			-EINVAL);
	free(handle);
}

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(float) uint8_t payload[PIXELS * sizeof(float)];
};

static void init_buffer(struct test_buffer *storage, uint32_t size,
		int32_t stride)
{
	memset(storage, 0, sizeof(*storage));
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = sizeof(storage->payload);
	storage->data.data = storage->payload;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = size;
	storage->chunk.stride = stride;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void configure_port(struct spa_node *node,
		struct param_capture *capture, enum spa_direction direction,
		uint32_t port_id)
{
	struct spa_pod *format = enum_one(node, capture, direction, port_id);

	spa_assert_se(spa_node_port_set_param(node, direction, port_id,
			SPA_PARAM_Format, 0, format) == 0);
}

static struct spa_pod *build_selection(uint8_t *storage, size_t size,
		int64_t flat_seq, int64_t background_seq)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);

	return spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props,
			SPA_PARAM_Props,
			SPA_PROP_CALCULON_PIXEL_CALIBRATION_FLAT_SEQ,
			SPA_POD_Long(flat_seq),
			SPA_PROP_CALCULON_PIXEL_CALIBRATION_BACKGROUND_SEQ,
			SPA_POD_Long(background_seq));
}

static struct spa_pod *build_incomplete_selection(uint8_t *storage,
		size_t size, int64_t flat_seq)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);

	return spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props,
			SPA_PARAM_Props,
			SPA_PROP_CALCULON_PIXEL_CALIBRATION_FLAT_SEQ,
			SPA_POD_Long(flat_seq));
}

static void exercise_processing(struct spa_node *node,
		struct param_capture *capture)
{
	struct test_buffer raw, flat, background, output, undersized;
	struct spa_buffer *raw_buffers[] = { &raw.buffer };
	struct spa_buffer *flat_buffers[] = { &flat.buffer };
	struct spa_buffer *background_buffers[] = { &background.buffer };
	struct spa_buffer *output_buffers[] = { &output.buffer };
	struct spa_buffer *undersized_buffers[] = { &undersized.buffer };
	struct spa_io_buffers raw_io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = 0,
	};
	struct spa_io_buffers flat_io = {
		.status = SPA_STATUS_HAVE_DATA,
		.buffer_id = 0,
	};
	struct spa_io_buffers background_io = {
		.status = SPA_STATUS_HAVE_DATA,
		.buffer_id = 0,
	};
	struct spa_io_buffers output_io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint8_t props_storage[256];
	uint16_t *raw_values;
	float *flat_values, *background_values, *output_values;
	uint32_t index;

	configure_port(node, capture, SPA_DIRECTION_INPUT, 0);
	configure_port(node, capture, SPA_DIRECTION_INPUT, 1);
	configure_port(node, capture, SPA_DIRECTION_INPUT, 2);
	configure_port(node, capture, SPA_DIRECTION_OUTPUT, 0);

	init_buffer(&raw, PIXELS * sizeof(uint16_t), WIDTH * sizeof(uint16_t));
	init_buffer(&flat, sizeof(flat.payload), WIDTH * sizeof(float));
	init_buffer(&background, sizeof(background.payload), WIDTH * sizeof(float));
	init_buffer(&output, 0, WIDTH * sizeof(float));
	init_buffer(&undersized, 0, WIDTH * sizeof(float));
	undersized.data.maxsize = sizeof(float);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			undersized_buffers, SPA_N_ELEMENTS(undersized_buffers)) == -EINVAL);

	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			raw_buffers, SPA_N_ELEMENTS(raw_buffers)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 1, 0,
			flat_buffers, SPA_N_ELEMENTS(flat_buffers)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 2, 0,
			background_buffers, SPA_N_ELEMENTS(background_buffers)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			output_buffers, SPA_N_ELEMENTS(output_buffers)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &raw_io, sizeof(raw_io)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 1,
			SPA_IO_Buffers, &flat_io, sizeof(flat_io)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 2,
			SPA_IO_Buffers, &background_io, sizeof(background_io)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &output_io, sizeof(output_io)) == 0);

	flat.header.seq = 10;
	background.header.seq = 20;
	flat_values = (float *)flat.payload;
	background_values = (float *)background.payload;
	for (index = 0; index < PIXELS; index++) {
		flat_values[index] = 0.5f + (float)index / 100.0f;
		background_values[index] = 3.0f + (float)index;
	}

	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(flat_io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(background_io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			build_incomplete_selection(props_storage,
				sizeof(props_storage), 10)) == -EINVAL);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			build_selection(props_storage, sizeof(props_storage), 10, 20)) == 0);

	raw_values = (uint16_t *)raw.payload;
	for (index = 0; index < PIXELS; index++)
		raw_values[index] = (uint16_t)(100 + 2 * index);
	raw.header.flags = 7;
	raw.header.offset = 9;
	raw.header.pts = 123456;
	raw.header.dts_offset = -12;
	raw.header.seq = 30;
	raw_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(raw_io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(output_io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(output.chunk.offset == 0);
	spa_assert_se(output.chunk.size == sizeof(output.payload));
	spa_assert_se(output.chunk.stride == WIDTH * (int32_t)sizeof(float));
	spa_assert_se(memcmp(&raw.header, &output.header,
			sizeof(raw.header)) == 0);
	output_values = (float *)output.payload;
	for (index = 0; index < PIXELS; index++) {
		const float expected = flat_values[index] *
				((float)raw_values[index] - background_values[index]);
		spa_assert_se(output_values[index] == expected);
	}

	output_io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	raw_io.status = SPA_STATUS_HAVE_DATA;
	measured_allocations = 0;
	measure_allocations = true;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	measure_allocations = false;
	spa_assert_se(measured_allocations == 0);
	output_io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	raw.chunk.size = 1;
	raw_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == -EINVAL);
	spa_assert_se(raw_io.status == -EINVAL);
	spa_assert_se(output_io.status == SPA_STATUS_NEED_DATA);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
}

static void exercise(const struct spa_handle_factory *factory)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_SIZE, "8x6"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	const struct spa_dict_item bad_rate_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_SIZE, "8x6"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/0"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile),
	};
	const struct spa_dict bad_rate = SPA_DICT_INIT(bad_rate_items,
			SPA_N_ELEMENTS(bad_rate_items));
	const struct spa_dict_item missing_profile_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_SIZE, "8x6"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1"),
	};
	const struct spa_dict missing_profile = SPA_DICT_INIT(
			missing_profile_items, SPA_N_ELEMENTS(missing_profile_items));
	const size_t size = spa_handle_factory_get_size(factory, &info);
	struct spa_handle *handle = calloc(1, size);
	struct spa_node *node = NULL;
	struct spa_hook listener;
	struct param_capture capture = { .expected = SPA_ID_INVALID };
	struct spa_video_info_raw raw = SPA_VIDEO_INFO_RAW_INIT();
	struct spa_pod *format;

	expect_invalid_info(factory, &bad_rate);
	expect_invalid_info(factory, &missing_profile);
	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);

	measured_allocations = 0;
	measure_allocations = true;
	format = enum_one(node, &capture, SPA_DIRECTION_INPUT, 0);
	measure_allocations = false;
	spa_assert_se(measured_allocations > 0);
	spa_assert_se(spa_format_video_raw_parse(format, &raw) >= 0);
	spa_assert_se(raw.format == SPA_VIDEO_FORMAT_GRAY16_LE);
	spa_assert_se(raw.size.width == WIDTH && raw.size.height == HEIGHT);
	spa_assert_se(raw.framerate.num == 1000 && raw.framerate.denom == 1);

	expect_ndarray(node, &capture, SPA_DIRECTION_INPUT, 1,
			"org.calculon.ao.flat-calibration/1", false);
	expect_ndarray(node, &capture, SPA_DIRECTION_INPUT, 2,
			"org.calculon.ao.background-calibration/1", false);
	expect_ndarray(node, &capture, SPA_DIRECTION_OUTPUT, 0,
			"org.calculon.ao.calibrated-pixels/1", true);

	{
		struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
		spa_assert_se(spa_node_send_command(node, &start) == -EIO);
	}
	exercise_processing(node, &capture);
	spa_hook_remove(&listener);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
}

int main(int argc, char **argv)
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	const struct spa_handle_factory *pixel_factory = NULL;
	void *library;
	void *symbol;
	uint32_t index = 0;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL);
	spa_assert_se(spa_streq(factory->name,
			SPA_NAME_API_CALCULON_PIXEL_CALIBRATION));
	pixel_factory = factory;
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(spa_streq(factory->name,
			SPA_NAME_API_CALCULON_SHWFS_CONTROLLER));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(spa_streq(factory->name,
			SPA_NAME_API_ALPAO_COMMAND_NORMALIZATION));
	spa_assert_se(enumerate(&factory, &index) == 0);
	exercise(pixel_factory);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}

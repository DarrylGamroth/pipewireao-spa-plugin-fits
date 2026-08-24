/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/calculon.h>
#include <pipewireao-plugins/fits.h>

#define WIDTH 4u
#define HEIGHT 2u
#define PIXELS (WIDTH * HEIGHT)
#define N_RAW_BUFFERS 2u

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct param_capture {
	uint32_t expected;
	uint8_t storage[4096];
	struct spa_pod *param;
};

struct loaded_plugin {
	void *library;
	spa_handle_factory_enum_func_t enumerate;
};

struct node_instance {
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_hook listener;
	struct param_capture capture;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(double) uint8_t payload[PIXELS * sizeof(double)];
};

static void on_result(void *data, int seq SPA_UNUSED, int result,
		uint32_t type, const void *value)
{
	struct param_capture *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

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

static struct loaded_plugin load_plugin(const char *path)
{
	struct loaded_plugin plugin = { 0 };
	void *symbol;

	plugin.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(plugin.library != NULL);
	symbol = dlsym(plugin.library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	memcpy(&plugin.enumerate, &symbol, sizeof(plugin.enumerate));
	return plugin;
}

static const struct spa_handle_factory *find_factory(
		const struct loaded_plugin *plugin, const char *name)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;

	while (plugin->enumerate(&factory, &index) == 1)
		if (spa_streq(factory->name, name))
			return factory;
	return NULL;
}

static void make_node(struct node_instance *instance,
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
			&node_events, &instance->capture) == 0);
}

static struct spa_pod *enum_format(struct node_instance *instance,
		enum spa_direction direction, uint32_t index)
{
	instance->capture.expected = SPA_PARAM_EnumFormat;
	instance->capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(instance->node, 1, direction, 0,
			SPA_PARAM_EnumFormat, index, 1, NULL) == 0);
	spa_assert_se(instance->capture.param != NULL);
	return instance->capture.param;
}

static void init_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.data = storage->payload;
	storage->data.maxsize = sizeof(storage->payload);
	storage->data.chunk = &storage->chunk;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void make_image_cube(char path[static 64])
{
	uint16_t values[2][HEIGHT][WIDTH] = {
		{ { 4, 1, 1, 1 }, { 1, 1, 1, 4 } },
		{ { 8, 2, 2, 2 }, { 2, 2, 2, 8 } },
	};
	LONGLONG axes[] = { WIDTH, HEIGHT, 2 };
	char create_path[96];
	fitsfile *file = NULL;
	int descriptor, status = 0;

	memcpy(path, "/tmp/pipewireao-fits-calculon-XXXXXX",
			sizeof("/tmp/pipewireao-fits-calculon-XXXXXX"));
	descriptor = mkstemp(path);
	spa_assert_se(descriptor >= 0);
	spa_assert_se(close(descriptor) == 0);
	spa_assert_se(unlink(path) == 0);
	spa_assert_se(snprintf(create_path, sizeof(create_path), "!%s", path) > 0);
	fits_create_file(&file, create_path, &status);
	fits_create_imgll(file, USHORT_IMG, SPA_N_ELEMENTS(axes), axes, &status);
	fits_write_img(file, TUSHORT, 1,
			sizeof(values) / sizeof(values[0][0][0]), values, &status);
	fits_close_file(file, &status);
	spa_assert_se(status == 0);
}

static void destroy_node(struct node_instance *instance)
{
	spa_hook_remove(&instance->listener);
	spa_assert_se(instance->handle->clear(instance->handle) == 0);
	free(instance->handle);
}

int main(int argc, char *argv[])
{
	struct loaded_plugin fits, calculon;
	const struct spa_handle_factory *source_factory, *pixel_factory;
	struct node_instance source, pixel;
	char fits_path[64];
	const struct spa_dict_item source_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PATH, fits_path),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_SAMPLE_RANK, "2"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_RATE, "1000/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_SCHEMA,
				"org.pipewireao.test.raw-fits/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PROFILE, profile),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_IO_MODE, "mmap"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PREFAULT, "true"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_LOOP, "true"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PROGRESSIVE, "disabled"),
	};
	const struct spa_dict source_info = SPA_DICT_INIT(source_items,
			SPA_N_ELEMENTS(source_items));
	const struct spa_dict_item pixel_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_SIZE, "4x2"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile),
	};
	const struct spa_dict pixel_info = SPA_DICT_INIT(pixel_items,
			SPA_N_ELEMENTS(pixel_items));
	struct test_buffer raw[N_RAW_BUFFERS], calibrated;
	struct spa_buffer *raw_buffers[N_RAW_BUFFERS];
	struct spa_buffer *calibrated_buffers[] = { &calibrated.buffer };
	struct spa_io_buffers_latest latest = { 0 };
	struct spa_io_buffers_latest_link latest_link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &latest,
		.notify_fd = -1,
	};
	struct spa_io_buffers output = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint8_t negotiated_storage[8192];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(negotiated_storage,
			sizeof(negotiated_storage));
	struct spa_pod *source_format, *pixel_format, *negotiated = NULL;
	const float *values;
	uint32_t index;

	spa_assert_se(argc == 3);
	make_image_cube(fits_path);
	fits = load_plugin(argv[1]);
	calculon = load_plugin(argv[2]);
	source_factory = find_factory(&fits, SPA_NAME_API_FITS_SOURCE);
	pixel_factory = find_factory(&calculon,
			SPA_NAME_API_CALCULON_PIXEL_CALIBRATION);
	spa_assert_se(source_factory != NULL && pixel_factory != NULL);
	make_node(&source, source_factory, &source_info);
	make_node(&pixel, pixel_factory, &pixel_info);

	/* FITS format index 1 is video/raw GRAY16_LE. Intersect it with the
	 * consumer constraint and apply one fixed result to both ports. */
	source_format = enum_format(&source, SPA_DIRECTION_OUTPUT, 1);
	pixel_format = enum_format(&pixel, SPA_DIRECTION_INPUT, 0);
	spa_assert_se(spa_pod_filter(&builder, &negotiated, source_format,
			pixel_format) == 0);
	spa_assert_se(negotiated != NULL);
	spa_assert_se(spa_node_port_set_param(source.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, negotiated) == 0);
	spa_assert_se(spa_node_port_set_param(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, negotiated) == 0);
	pixel_format = enum_format(&pixel, SPA_DIRECTION_OUTPUT, 0);
	spa_assert_se(spa_node_port_set_param(pixel.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, pixel_format) == 0);

	for (index = 0; index < N_RAW_BUFFERS; index++) {
		init_buffer(&raw[index]);
		raw_buffers[index] = &raw[index].buffer;
	}
	init_buffer(&calibrated);
	spa_assert_se(spa_node_port_use_buffers(source.node, SPA_DIRECTION_OUTPUT,
			0, 0, raw_buffers, N_RAW_BUFFERS) == 0);
	spa_assert_se(spa_node_port_use_buffers(pixel.node, SPA_DIRECTION_INPUT,
			0, 0, raw_buffers, N_RAW_BUFFERS) == 0);
	spa_assert_se(spa_node_port_use_buffers(pixel.node, SPA_DIRECTION_OUTPUT,
			0, 0, calibrated_buffers, 1) == 0);
	spa_assert_se(spa_node_port_set_io(source.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &output, sizeof(output)) == 0);

	spa_assert_se(spa_node_send_command(pixel.node, &start) == 0);
	spa_assert_se(spa_node_send_command(source.node, &start) == 0);
	spa_assert_se(spa_node_process(source.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(output.buffer_id == 0);
	spa_assert_se(calibrated.chunk.size == PIXELS * sizeof(float));
	values = (const float *)calibrated.payload;
	for (index = 0; index < PIXELS; index++) {
		const uint16_t expected[] = { 4, 1, 1, 1, 1, 1, 1, 4 };
		spa_assert_se(values[index] == (float)expected[index]);
	}

	spa_assert_se(spa_node_send_command(source.node, &pause) == 0);
	spa_assert_se(spa_node_send_command(pixel.node, &pause) == 0);
	latest_link.flags = 0;
	latest_link.io = NULL;
	spa_assert_se(spa_node_port_set_io(source.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_BuffersLatestLink, &latest_link, sizeof(latest_link)) == 0);
	destroy_node(&pixel);
	destroy_node(&source);
	spa_assert_se(dlclose(calculon.library) == 0);
	spa_assert_se(dlclose(fits.library) == 0);
	spa_assert_se(unlink(fits_path) == 0);
	return 0;
}

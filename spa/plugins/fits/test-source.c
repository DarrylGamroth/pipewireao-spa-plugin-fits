/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <fitsio.h>
#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>

#include "fits.h"

#define N_BUFFERS 4u
#define TEST_SCHEMA "org.pipewireao.test.fits-values/1"
#define TEST_PROFILE \
	"sha256:0000000000000000000000000000000000000000000000000000000000000000"

struct param_result {
	uint32_t expected;
	uint8_t *storage;
	size_t capacity;
	struct spa_pod *param;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	void *payload;
};

struct source_case {
	const char *path;
	uint32_t sample_rank;
	uint32_t format_index;
	enum spa_element_type expected_element;
	uint32_t expected_size;
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

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static struct spa_pod *enum_one(struct spa_node *node,
		struct param_result *capture, uint32_t id, uint32_t index)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, index, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static void init_buffer(struct test_buffer *storage, uint32_t size)
{
	storage->payload = calloc(1, size);
	spa_assert_se(storage->payload != NULL);
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.data = storage->payload;
	storage->data.maxsize = size;
	storage->data.chunk = &storage->chunk;
	storage->metas[0].type = SPA_META_Header;
	storage->metas[0].size = sizeof(storage->header);
	storage->metas[0].data = &storage->header;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void temporary_path(char *path, size_t size, const char *prefix)
{
	char pattern[256];
	int fd;

	spa_assert_se(snprintf(pattern, sizeof(pattern), "/tmp/%s-XXXXXX", prefix) > 0);
	fd = mkstemp(pattern);
	spa_assert_se(fd >= 0);
	spa_assert_se(close(fd) == 0);
	spa_assert_se(unlink(pattern) == 0);
	spa_assert_se(strlen(pattern) < size);
	memcpy(path, pattern, strlen(pattern) + 1u);
}

static void make_vector_cube(char *path, size_t size)
{
	double values[3][4];
	LONGLONG axes[] = { 4, 3 };
	fitsfile *file = NULL;
	char create_path[300];
	int status = 0;
	uint32_t frame, element;

	temporary_path(path, size, "pipewireao-fits-vector");
	spa_assert_se(snprintf(create_path, sizeof(create_path), "!%s", path) > 0);
	fits_create_file(&file, create_path, &status);
	fits_create_imgll(file, DOUBLE_IMG, SPA_N_ELEMENTS(axes), axes, &status);
	for (frame = 0; frame < 3; frame++)
		for (element = 0; element < 4; element++)
			values[frame][element] = frame * 10.0 + element;
	fits_write_img(file, TDOUBLE, 1,
			sizeof(values) / sizeof(values[0][0]), values, &status);
	fits_close_file(file, &status);
	spa_assert_se(status == 0);
}

static void make_image_cube(char *path, size_t size)
{
	uint16_t values[2][3][4];
	LONGLONG axes[] = { 4, 3, 2 };
	fitsfile *file = NULL;
	char create_path[300];
	int status = 0;
	uint32_t frame, row, column;

	temporary_path(path, size, "pipewireao-fits-image");
	spa_assert_se(snprintf(create_path, sizeof(create_path), "!%s", path) > 0);
	fits_create_file(&file, create_path, &status);
	fits_create_imgll(file, USHORT_IMG, SPA_N_ELEMENTS(axes), axes, &status);
	for (frame = 0; frame < 2; frame++)
		for (row = 0; row < 3; row++)
			for (column = 0; column < 4; column++)
				values[frame][row][column] =
						(uint16_t)(frame * 100 + row * 10 + column);
	fits_write_img(file, TUSHORT, 1,
			sizeof(values) / sizeof(values[0][0][0]), values, &status);
	fits_close_file(file, &status);
	spa_assert_se(status == 0);
}

static const char *format_string(const struct spa_pod *format, uint32_t key)
{
	const struct spa_pod_prop *property = spa_pod_find_prop(format, NULL, key);
	const char *value = NULL;

	spa_assert_se(property != NULL);
	spa_assert_se(spa_pod_get_string(&property->value, &value) == 0);
	return value;
}

static struct spa_node *make_node(const struct spa_handle_factory *factory,
		const char *path, uint32_t rank, struct spa_handle **handle)
{
	char rank_text[8];
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PATH, path),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_SAMPLE_RANK, rank_text),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_RATE, "1000/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_SCHEMA, TEST_SCHEMA),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_PROFILE, TEST_PROFILE),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_FITS_LOOP, "true"),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct spa_node *node = NULL;

	snprintf(rank_text, sizeof(rank_text), "%u", rank);
	*handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(*handle != NULL);
	spa_assert_se(factory->init(factory, *handle, &info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(*handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	return node;
}

static void run_source(const struct spa_handle_factory *factory,
		const struct source_case *test)
{
	struct test_buffer storage[N_BUFFERS] = { 0 };
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct param_result capture = { .expected = SPA_ID_INVALID };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	struct spa_video_info_raw video = { 0 };
	struct spa_hook listener;
	struct spa_handle *handle;
	struct spa_node *node = make_node(factory, test->path, test->sample_rank,
			&handle);
	struct spa_pod *format, *buffers_param;
	int32_t payload_size = 0;
	uint32_t id, i;
	int res;

	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	format = enum_one(node, &capture, SPA_PARAM_EnumFormat, test->format_index);
	if (test->sample_rank == 1) {
		spa_assert_se(spa_format_ndarray_parse(format, &ndarray) == 0);
		spa_assert_se(ndarray.element_type == test->expected_element);
		spa_assert_se(ndarray.n_dimensions == 1 && ndarray.shape[0] == 4);
		spa_assert_se(ndarray.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
		spa_assert_se(spa_streq(format_string(format,
				SPA_FORMAT_NDARRAY_schema), TEST_SCHEMA));
		spa_assert_se(spa_streq(format_string(format,
				SPA_FORMAT_NDARRAY_profile), TEST_PROFILE));
	} else if (test->format_index == 1) {
		spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);
		spa_assert_se(video.format == SPA_VIDEO_FORMAT_GRAY16_LE);
		spa_assert_se(video.size.width == 4 && video.size.height == 3);
	}
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	buffers_param = enum_one(node, &capture, SPA_PARAM_Buffers, 0);
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size)) >= 0);
	spa_assert_se(payload_size == (int32_t)test->expected_size);
	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i], (uint32_t)payload_size);
		buffers[i] = &storage[i].buffer;
	}
	res = spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS);
	spa_assert_se(res == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	while (io.status != SPA_STATUS_HAVE_DATA) {
		res = spa_node_process(node);
		spa_assert_se(res >= SPA_STATUS_OK);
	}
	id = io.buffer_id;
	spa_assert_se(id < N_BUFFERS);
	spa_assert_se(storage[id].chunk.size == test->expected_size);
	spa_assert_se(storage[id].header.pts != SPA_TIME_INVALID);
	if (test->sample_rank == 1) {
		const double *values = storage[id].payload;
		uint64_t frame = storage[id].header.seq % 3u;

		for (i = 0; i < 4; i++)
			spa_assert_se(values[i] == frame * 10.0 + i);
	} else {
		const uint16_t *values = storage[id].payload;
		uint64_t frame = storage[id].header.seq % 2u;

		for (i = 0; i < 12; i++)
			spa_assert_se(values[i] == frame * 100u + (i / 4u) * 10u + i % 4u);
	}
	io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);

	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	free(capture.storage);
	for (i = 0; i < N_BUFFERS; i++)
		free(storage[i].payload);
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	char vector_path[256], image_path[256];
	uint32_t index = 0;
	void *library;

	spa_assert_se(argc == 2);
	make_vector_cube(vector_path, sizeof(vector_path));
	make_image_cube(image_path, sizeof(image_path));
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	enumerate = (spa_handle_factory_enum_func_t)dlsym(library,
			SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(enumerate != NULL);
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_FITS_SOURCE));
	run_source(factory, &(const struct source_case) {
		.path = vector_path,
		.sample_rank = 1,
		.expected_element = SPA_ELEMENT_TYPE_F64_LE,
		.expected_size = 4u * sizeof(double),
	});
	run_source(factory, &(const struct source_case) {
		.path = image_path,
		.sample_rank = 2,
		.format_index = 1,
		.expected_element = SPA_ELEMENT_TYPE_U16_LE,
		.expected_size = 4u * 3u * sizeof(uint16_t),
	});
	spa_assert_se(dlclose(library) == 0);
	spa_assert_se(unlink(vector_path) == 0);
	spa_assert_se(unlink(image_path) == 0);
	return 0;
}

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
#include <pipewire/loop.h>
#include <pipewire/pipewire.h>
#include <pipewireao-plugins/calculon.h>
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
	uint64_t node_flags;
	const char *readiness;
	const char *output_mode;
	const char *row_block_rows;
	const char *simulated_readout_time_ns;
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
	const char *rate;
	const char *schema;
	const char *profile;
	const char *output_mode;
	const char *row_block_rows;
	const char *simulated_readout_time_ns;
	uint32_t sample_rank;
	uint32_t format_index;
	enum spa_element_type expected_element;
	uint32_t expected_size;
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
				SPA_KEY_API_FITS_READINESS);
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS) {
		capture->output_mode = spa_dict_lookup(info->props,
				SPA_KEY_API_FITS_OUTPUT_MODE);
		capture->row_block_rows = spa_dict_lookup(info->props,
				SPA_KEY_API_FITS_ROW_BLOCK_ROWS);
		capture->simulated_readout_time_ns = spa_dict_lookup(info->props,
				SPA_KEY_API_FITS_SIMULATED_READOUT_TIME_NS);
	}
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.info = on_info,
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

static int init_node(const struct spa_handle_factory *factory,
		const struct source_case *test, const char *readiness,
		const struct spa_support *support, uint32_t n_support,
		struct spa_handle **handle, struct spa_node **node)
{
	char rank_text[8];
	struct spa_dict_item items[11];
	struct spa_dict info;
	uint32_t n_items = 0;
	int res;

	snprintf(rank_text, sizeof(rank_text), "%u", test->sample_rank);
	#define ADD_ITEM(key, value) \
		items[n_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_API_FITS_PATH, test->path);
	ADD_ITEM(SPA_KEY_API_FITS_SAMPLE_RANK, rank_text);
	ADD_ITEM(SPA_KEY_API_FITS_RATE,
			test->rate == NULL ? "1000/1" : test->rate);
	ADD_ITEM(SPA_KEY_API_FITS_SCHEMA,
			test->schema == NULL ? TEST_SCHEMA : test->schema);
	if (test->profile != NULL || test->output_mode == NULL)
		ADD_ITEM(SPA_KEY_API_FITS_PROFILE,
				test->profile == NULL ? TEST_PROFILE : test->profile);
	ADD_ITEM(SPA_KEY_API_FITS_LOOP, "true");
	ADD_ITEM(SPA_KEY_API_FITS_READINESS, readiness);
	if (test->output_mode != NULL)
		ADD_ITEM(SPA_KEY_API_FITS_OUTPUT_MODE, test->output_mode);
	if (test->row_block_rows != NULL)
		ADD_ITEM(SPA_KEY_API_FITS_ROW_BLOCK_ROWS, test->row_block_rows);
	if (test->simulated_readout_time_ns != NULL)
		ADD_ITEM(SPA_KEY_API_FITS_SIMULATED_READOUT_TIME_NS,
				test->simulated_readout_time_ns);
	#undef ADD_ITEM
	info = SPA_DICT_INIT(items, n_items);
	*handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(*handle != NULL);
	*node = NULL;
	res = factory->init(factory, *handle, &info, support, n_support);
	if (res < 0) {
		free(*handle);
		*handle = NULL;
		return res;
	}
	res = spa_handle_get_interface(*handle, SPA_TYPE_INTERFACE_Node,
			(void **)node);
	if (res < 0) {
		spa_assert_se((*handle)->clear(*handle) == 0);
		free(*handle);
		*handle = NULL;
	}
	return res;
}

static struct spa_node *make_node(const struct spa_handle_factory *factory,
		const struct source_case *test, const char *readiness,
		const struct spa_support *support, uint32_t n_support,
		struct spa_handle **handle)
{
	struct spa_node *node = NULL;

	spa_assert_se(init_node(factory, test, readiness, support, n_support,
			handle, &node) == 0);
	return node;
}

static void run_source(const struct spa_handle_factory *factory,
		const struct source_case *test, const char *readiness)
{
	struct test_buffer storage[N_BUFFERS] = { 0 };
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct param_result capture = { .expected = SPA_ID_INVALID };
	struct readiness_state readiness_state = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	struct spa_video_info_raw video = { 0 };
	struct spa_hook listener;
	struct pw_loop *loop = NULL;
	struct spa_support support[2];
	uint32_t n_support = 0;
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_pod *format, *buffers_param;
	int32_t payload_size = 0;
	uint32_t cycle, id, i, second_pass = 0;
	int res;

	if (spa_streq(readiness, "timerfd")) {
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
	node = make_node(factory, test, readiness,
			support, n_support, &handle);
	spa_assert_se(spa_node_set_callbacks(node, &node_callbacks,
			&readiness_state) == 0);

	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	spa_assert_se(capture.readiness != NULL &&
			spa_streq(capture.readiness, readiness));
	spa_assert_se(capture.output_mode != NULL &&
			spa_streq(capture.output_mode, "frame"));
	spa_assert_se(capture.row_block_rows == NULL);
	spa_assert_se(capture.simulated_readout_time_ns == NULL);
	spa_assert_se((capture.node_flags & SPA_NODE_FLAG_POLL_DRIVER) ==
			(spa_streq(readiness, "poll") ?
					SPA_NODE_FLAG_POLL_DRIVER : 0));
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
	storage[0].buffer.n_metas = 0;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS) == -EINVAL);
	storage[0].buffer.n_metas = SPA_N_ELEMENTS(storage[0].metas);
	storage[0].metas[0].size = sizeof(storage[0].header) - 1u;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS) == -EINVAL);
	storage[0].metas[0].size = sizeof(storage[0].header);
	res = spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS);
	spa_assert_se(res == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	for (cycle = 0; cycle < N_BUFFERS * 2u; cycle++) {
		while (io.status != SPA_STATUS_HAVE_DATA) {
			if (loop != NULL)
				spa_assert_se(pw_loop_iterate(loop, 1000) >= 0);
			else {
				res = spa_node_process(node);
				spa_assert_se(res >= SPA_STATUS_OK);
			}
		}
		id = io.buffer_id;
		spa_assert_se(id < N_BUFFERS);
		if (cycle >= N_BUFFERS)
			second_pass |= 1u << id;
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
				spa_assert_se(values[i] ==
						frame * 100u + (i / 4u) * 10u + i % 4u);
		}
		io.status = SPA_STATUS_NEED_DATA;
	}
	spa_assert_se(second_pass == (1u << N_BUFFERS) - 1u);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);

	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	if (loop != NULL) {
		spa_assert_se(readiness_state.ready_calls >= N_BUFFERS * 2u);
		pw_loop_leave(loop);
		pw_loop_destroy(loop);
	}
	free(capture.storage);
	for (i = 0; i < N_BUFFERS; i++)
		free(storage[i].payload);
}

static void run_row_source(const struct spa_handle_factory *factory,
		const char *path, const char *readiness, bool force_overload,
		bool force_backlog)
{
	const struct source_case test = {
		.path = path,
		.rate = "20/1",
		.schema = SPA_CALCULON_SCHEMA_RAW_PIXEL_ROW_BLOCK,
		.profile = TEST_PROFILE,
		.output_mode = "row-block",
		.row_block_rows = "1",
		.simulated_readout_time_ns = "30000000",
		.sample_rank = 2,
	};
	struct test_buffer storage[N_BUFFERS] = { 0 };
	struct spa_buffer *buffers[N_BUFFERS];
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct param_result capture = { .expected = SPA_ID_INVALID };
	struct readiness_state readiness_state = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	struct spa_hook listener;
	struct pw_loop *loop = NULL;
	struct spa_support support[2];
	uint32_t n_support = 0;
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_pod *format, *buffers_param;
	int32_t payload_size = 0;
	int64_t previous_pts = SPA_TIME_INVALID;
	uint32_t cycle, cycles = force_overload ? 2u : force_backlog ? 4u : 6u;
	uint32_t id, i;
	int res;

	spa_assert_se(!(force_overload && force_backlog));
	spa_assert_se((!force_overload && !force_backlog) ||
			spa_streq(readiness, "poll"));
	if (spa_streq(readiness, "timerfd")) {
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
	node = make_node(factory, &test, readiness, support, n_support, &handle);
	spa_assert_se(spa_node_set_callbacks(node, &node_callbacks,
			&readiness_state) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	spa_assert_se(capture.output_mode != NULL &&
			spa_streq(capture.output_mode, "row-block"));
	spa_assert_se(capture.row_block_rows != NULL &&
			spa_streq(capture.row_block_rows, "1"));
	spa_assert_se(capture.simulated_readout_time_ns != NULL &&
			spa_streq(capture.simulated_readout_time_ns, "30000000"));

	format = enum_one(node, &capture, SPA_PARAM_EnumFormat, 0);
	spa_assert_se(spa_format_ndarray_parse(format, &ndarray) == 0);
	spa_assert_se(ndarray.element_type == SPA_ELEMENT_TYPE_U16_LE);
	spa_assert_se(ndarray.n_dimensions == 2 &&
			ndarray.shape[0] == 1 && ndarray.shape[1] == 4);
	spa_assert_se(ndarray.layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	spa_assert_se(ndarray.rate.num == 60 && ndarray.rate.denom == 1);
	spa_assert_se(spa_streq(format_string(format,
			SPA_FORMAT_NDARRAY_schema),
			SPA_CALCULON_SCHEMA_RAW_PIXEL_ROW_BLOCK));
	spa_assert_se(spa_streq(format_string(format,
			SPA_FORMAT_NDARRAY_profile), TEST_PROFILE));
	capture.expected = SPA_PARAM_EnumFormat;
	capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_EnumFormat, 1, 1, NULL) == 0);
	spa_assert_se(capture.param == NULL);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	buffers_param = enum_one(node, &capture, SPA_PARAM_Buffers, 0);
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size)) >= 0);
	spa_assert_se(payload_size == 4 * (int32_t)sizeof(uint16_t));
	for (i = 0; i < N_BUFFERS; i++) {
		init_buffer(&storage[i], (uint32_t)payload_size);
		buffers[i] = &storage[i].buffer;
	}
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers, N_BUFFERS) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	for (cycle = 0; cycle < cycles; cycle++) {
		const uint16_t *values;
		uint64_t sequence;
		uint32_t first_row;

		while (io.status != SPA_STATUS_HAVE_DATA) {
			if (loop != NULL)
				spa_assert_se(pw_loop_iterate(loop, 1000) >= 0);
			else {
				res = spa_node_process(node);
				spa_assert_se(res >= SPA_STATUS_OK);
			}
		}
		id = io.buffer_id;
		spa_assert_se(id < N_BUFFERS);
		sequence = storage[id].header.seq;
		first_row = storage[id].header.offset;
		if (force_backlog) {
			const uint64_t expected_sequences[] = { 0, 0, 0, 2 };
			const uint32_t expected_rows[] = { 0, 1, 2, 0 };
			const int64_t expected_deltas[] = {
					0, 10000000, 10000000, 80000000,
			};

			spa_assert_se(sequence == expected_sequences[cycle]);
			spa_assert_se(first_row == expected_rows[cycle]);
			if (previous_pts != SPA_TIME_INVALID)
				spa_assert_se(storage[id].header.pts - previous_pts ==
						expected_deltas[cycle]);
		} else if (!force_overload) {
			uint64_t expected_sequence = cycle / 3u;
			uint32_t expected_row = cycle % 3u;
			int64_t expected_delta = expected_row == 0 ?
					30000000 : 10000000;

			spa_assert_se(sequence == expected_sequence);
			spa_assert_se(first_row == expected_row);
			if (previous_pts != SPA_TIME_INVALID)
				spa_assert_se(storage[id].header.pts - previous_pts ==
						expected_delta);
		} else if (cycle == 0) {
			spa_assert_se(sequence == 0 && first_row == 0);
		} else {
			spa_assert_se(sequence > 0 && first_row == 0);
			spa_assert_se((storage[id].header.flags &
					SPA_META_HEADER_FLAG_DISCONT) != 0);
		}
		spa_assert_se(storage[id].chunk.offset == 0);
		spa_assert_se(storage[id].chunk.size == (uint32_t)payload_size);
		spa_assert_se(storage[id].chunk.stride ==
				4 * (int32_t)sizeof(uint16_t));
		spa_assert_se((storage[id].header.flags &
				SPA_META_HEADER_FLAG_MARKER) ==
				(first_row == 2 ? SPA_META_HEADER_FLAG_MARKER : 0));
		spa_assert_se((storage[id].header.flags &
				SPA_META_HEADER_FLAG_DISCONT) ==
				(cycle == 0 || (force_overload && cycle == 1) ||
				 (force_backlog && cycle == 3) ?
						SPA_META_HEADER_FLAG_DISCONT : 0));
		values = storage[id].payload;
		for (i = 0; i < 4; i++)
			spa_assert_se(values[i] ==
					(sequence % 2u) * 100u + first_row * 10u + i);
		previous_pts = storage[id].header.pts;
		if (force_overload && cycle == 0) {
			const struct timespec delay = {
				.tv_nsec = 25000000,
			};

			spa_assert_se(nanosleep(&delay, NULL) == 0);
			res = spa_node_process(node);
			spa_assert_se(res >= SPA_STATUS_OK);
			spa_assert_se(io.status == SPA_STATUS_HAVE_DATA &&
					io.buffer_id == id);
		}
		io.status = SPA_STATUS_NEED_DATA;
		if (force_backlog && (cycle == 0 || cycle == 2)) {
			const struct timespec delay = {
				.tv_nsec = cycle == 0 ? 25000000 : 80000000,
			};

			spa_assert_se(nanosleep(&delay, NULL) == 0);
		}
	}
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			NULL, 0) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	if (loop != NULL) {
		spa_assert_se(readiness_state.ready_calls >= cycles);
		pw_loop_leave(loop);
		pw_loop_destroy(loop);
	}
	free(capture.storage);
	for (i = 0; i < N_BUFFERS; i++)
		free(storage[i].payload);
}

static void assert_init_fails(const struct spa_handle_factory *factory,
		const struct source_case *test, int expected)
{
	struct spa_handle *handle = NULL;
	struct spa_node *node = NULL;

	spa_assert_se(init_node(factory, test, "poll", NULL, 0, &handle,
			&node) == expected);
	spa_assert_se(handle == NULL && node == NULL);
}

static void test_row_options(const struct spa_handle_factory *factory,
		const char *vector_path, const char *image_path)
{
	const struct source_case valid = {
		.path = image_path,
		.rate = "20/1",
		.schema = SPA_CALCULON_SCHEMA_RAW_PIXEL_ROW_BLOCK,
		.profile = TEST_PROFILE,
		.output_mode = "row-block",
		.row_block_rows = "1",
		.simulated_readout_time_ns = "30000000",
		.sample_rank = 2,
	};
	struct source_case test;

	test = valid;
	test.schema = TEST_SCHEMA;
	assert_init_fails(factory, &test, -EINVAL);
	test = valid;
	test.profile = NULL;
	assert_init_fails(factory, &test, -EINVAL);
	test = valid;
	test.row_block_rows = "2";
	assert_init_fails(factory, &test, -EINVAL);
	test = valid;
	test.simulated_readout_time_ns = "60000000";
	assert_init_fails(factory, &test, -EINVAL);
	test = valid;
	test.path = vector_path;
	test.sample_rank = 1;
	assert_init_fails(factory, &test, -EINVAL);
	test = valid;
	test.output_mode = "frame";
	assert_init_fails(factory, &test, -EINVAL);
}

int main(int argc, char *argv[])
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	char vector_path[256], image_path[256];
	uint32_t index = 0;
	void *library;

	spa_assert_se(argc == 2);
	pw_init(&argc, &argv);
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
	}, "poll");
	run_source(factory, &(const struct source_case) {
		.path = image_path,
		.sample_rank = 2,
		.format_index = 1,
		.expected_element = SPA_ELEMENT_TYPE_U16_LE,
		.expected_size = 4u * 3u * sizeof(uint16_t),
	}, "poll");
	run_source(factory, &(const struct source_case) {
		.path = vector_path,
		.sample_rank = 1,
		.expected_element = SPA_ELEMENT_TYPE_F64_LE,
		.expected_size = 4u * sizeof(double),
	}, "timerfd");
	run_row_source(factory, image_path, "poll", false, false);
	run_row_source(factory, image_path, "timerfd", false, false);
	run_row_source(factory, image_path, "poll", true, false);
	run_row_source(factory, image_path, "poll", false, true);
	test_row_options(factory, vector_path, image_path);
	spa_assert_se(dlclose(library) == 0);
	spa_assert_se(unlink(vector_path) == 0);
	spa_assert_se(unlink(image_path) == 0);
	pw_deinit();
	return 0;
}

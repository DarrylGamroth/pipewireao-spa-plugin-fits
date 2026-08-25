/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/alpao.h>
#include <pipewireao-plugins/calculon.h>

#define DEFAULT_WIDTH 4u
#define DEFAULT_HEIGHT 2u
#define DEFAULT_REGION_WIDTH 2u
#define DEFAULT_REGION_HEIGHT 2u
#define DEFAULT_ACTUATORS 8u
#define MAX_ACTUATORS 468u
#define MAX_DETECTOR_EXTENT 2048u

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

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
	uint8_t *payload;
};

struct benchmark_profile_gate {
	int ready_fd;
	int start_fd;
	int done_fd;
	int finish_fd;
	bool enabled;
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
	const size_t size = spa_handle_factory_get_size(factory, info);

	memset(instance, 0, sizeof(*instance));
	instance->capture.expected = SPA_ID_INVALID;
	instance->handle = calloc(1, size);
	spa_assert_se(instance->handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, instance->handle, info,
			NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(instance->handle,
			SPA_TYPE_INTERFACE_Node, (void **)&instance->node) == 0);
	spa_assert_se(spa_node_add_listener(instance->node, &instance->listener,
			&node_events, &instance->capture) == 0);
}

static struct spa_pod *enum_format(struct node_instance *instance,
		enum spa_direction direction, uint32_t port)
{
	instance->capture.expected = SPA_PARAM_EnumFormat;
	instance->capture.param = NULL;
	spa_assert_se(spa_node_port_enum_params(instance->node, 1, direction,
			port, SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_assert_se(instance->capture.param != NULL);
	return instance->capture.param;
}

static void configure_port(struct node_instance *instance,
		enum spa_direction direction, uint32_t port)
{
	struct spa_pod *format = enum_format(instance, direction, port);

	spa_assert_se(spa_node_port_set_param(instance->node, direction, port,
			SPA_PARAM_Format, 0, format) == 0);
}

static void negotiate_ports(struct node_instance *output, uint32_t output_port,
		struct node_instance *input, uint32_t input_port)
{
	uint8_t storage[8192];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			sizeof(storage));
	const struct spa_pod *output_format = enum_format(output,
			SPA_DIRECTION_OUTPUT, output_port);
	const struct spa_pod *input_format = enum_format(input,
			SPA_DIRECTION_INPUT, input_port);
	struct spa_pod *negotiated = NULL;

	spa_assert_se(spa_pod_filter(&builder, &negotiated, output_format,
			input_format) == 0);
	spa_assert_se(negotiated != NULL);
	spa_assert_se(spa_node_port_set_param(output->node, SPA_DIRECTION_OUTPUT,
			output_port, SPA_PARAM_Format, 0, negotiated) == 0);
	spa_assert_se(spa_node_port_set_param(input->node, SPA_DIRECTION_INPUT,
			input_port, SPA_PARAM_Format, 0, negotiated) == 0);
}

static void init_buffer(struct test_buffer *storage, uint32_t maxsize,
		uint32_t size, int32_t stride)
{
	memset(storage, 0, sizeof(*storage));
	storage->payload = calloc(1, maxsize);
	spa_assert_se(storage->payload != NULL);
	storage->meta.type = SPA_META_Header;
	storage->meta.size = sizeof(storage->header);
	storage->meta.data = &storage->header;
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = maxsize;
	storage->data.data = storage->payload;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = size;
	storage->chunk.stride = stride;
	storage->buffer.n_metas = 1;
	storage->buffer.metas = &storage->meta;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void clear_buffer(struct test_buffer *storage)
{
	free(storage->payload);
	storage->payload = NULL;
}

static void use_one_buffer(struct spa_node *node, enum spa_direction direction,
		uint32_t port, struct test_buffer *storage)
{
	struct spa_buffer *buffers[] = { &storage->buffer };

	spa_assert_se(spa_node_port_use_buffers(node, direction, port, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
}

static void use_two_buffers(struct spa_node *node, enum spa_direction direction,
		uint32_t port, struct test_buffer storage[static 2])
{
	struct spa_buffer *buffers[] = { &storage[0].buffer, &storage[1].buffer };

	spa_assert_se(spa_node_port_use_buffers(node, direction, port, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
}

static void set_standard_io(struct spa_node *node,
		enum spa_direction direction, uint32_t port,
		struct spa_io_buffers *io)
{
	spa_assert_se(spa_node_port_set_io(node, direction, port,
			SPA_IO_Buffers, io, sizeof(*io)) == 0);
}

static void write_matrix(char path[static 32], uint32_t actuator_count,
		uint32_t slope_count)
{
	float *matrix;
	size_t matrix_bytes;
	size_t offset = 0;
	ssize_t written;
	uint32_t actuator;
	int fd;

	matrix_bytes = (size_t)actuator_count * slope_count * sizeof(*matrix);
	matrix = calloc((size_t)actuator_count * slope_count, sizeof(*matrix));
	spa_assert_se(matrix != NULL);
	memcpy(path, "/tmp/pipewireao-matrix-XXXXXX",
			sizeof("/tmp/pipewireao-matrix-XXXXXX"));
	fd = mkstemp(path);
	spa_assert_se(fd >= 0);
	for (actuator = 0; actuator < actuator_count; actuator++)
		matrix[(size_t)actuator * slope_count + actuator % slope_count] = 0.01f;
	while (offset < matrix_bytes) {
		written = write(fd, (uint8_t *)matrix + offset,
				matrix_bytes - offset);
		if (written < 0 && errno == EINTR)
			continue;
		spa_assert_se(written > 0);
		offset += (size_t)written;
	}
	spa_assert_se(close(fd) == 0);
	free(matrix);
}

static uint32_t parse_u32_argument(const char *value, uint32_t maximum)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	spa_assert_se(errno == 0 && end != value && *end == '\0' && parsed > 0 &&
			parsed <= maximum);
	return (uint32_t)parsed;
}

static char *make_size_text(uint32_t width, uint32_t height)
{
	char *text = malloc(32);

	spa_assert_se(text != NULL);
	spa_assert_se(snprintf(text, 32, "%ux%u", width, height) > 0);
	return text;
}

static char *make_region_origins(uint32_t width, uint32_t height,
		uint32_t region_width, uint32_t region_height,
		uint32_t *region_count)
{
	const uint32_t columns = width / region_width;
	const uint32_t rows = height / region_height;
	const size_t count = (size_t)columns * rows;
	const size_t capacity = count * 32u + 1u;
	char *origins = malloc(capacity);
	size_t offset = 0;
	uint32_t row, column;

	spa_assert_se(width % region_width == 0 && height % region_height == 0);
	spa_assert_se(count > 0 && count <= UINT32_MAX / 2u);
	spa_assert_se(origins != NULL);
	for (row = 0; row < rows; row++) {
		for (column = 0; column < columns; column++) {
			const int written = snprintf(origins + offset, capacity - offset,
					"%s%u,%u", offset == 0 ? "" : ";",
					row * region_height, column * region_width);

			spa_assert_se(written > 0 && (size_t)written < capacity - offset);
			offset += (size_t)written;
		}
	}
	*region_count = (uint32_t)count;
	return origins;
}

static int profile_gate_fd(const char *name)
{
	const char *value = getenv(name);
	char *end = NULL;
	long fd;

	if (value == NULL)
		return -1;
	errno = 0;
	fd = strtol(value, &end, 10);
	spa_assert_se(errno == 0 && end != value && *end == '\0' &&
			fd >= 0 && fd <= INT_MAX);
	return (int)fd;
}

static void profile_gate_init(struct benchmark_profile_gate *gate)
{
	gate->ready_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_READY_FD");
	gate->start_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_START_FD");
	gate->done_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_DONE_FD");
	gate->finish_fd = profile_gate_fd("PW_BENCHMARK_PROFILE_FINISH_FD");
	gate->enabled = gate->ready_fd >= 0 || gate->start_fd >= 0 ||
			gate->done_fd >= 0 || gate->finish_fd >= 0;
	spa_assert_se(!gate->enabled || (gate->ready_fd >= 0 &&
			gate->start_fd >= 0 && gate->done_fd >= 0 &&
			gate->finish_fd >= 0));
}

static void profile_gate_write(int fd, const void *data, size_t size)
{
	const uint8_t *bytes = data;

	while (size > 0) {
		ssize_t written = write(fd, bytes, size);

		if (written < 0 && errno == EINTR)
			continue;
		spa_assert_se(written > 0);
		bytes += written;
		size -= (size_t)written;
	}
}

static void profile_gate_read(int fd, void *data, size_t size)
{
	uint8_t *bytes = data;

	while (size > 0) {
		ssize_t received = read(fd, bytes, size);

		if (received < 0 && errno == EINTR)
			continue;
		spa_assert_se(received > 0);
		bytes += received;
		size -= (size_t)received;
	}
}

static void profile_gate_begin(struct benchmark_profile_gate *gate)
{
	const pid_t tid = (pid_t)syscall(SYS_gettid);
	uint8_t token;

	if (!gate->enabled)
		return;
	spa_assert_se(tid > 0);
	profile_gate_write(gate->ready_fd, &tid, sizeof(tid));
	profile_gate_read(gate->start_fd, &token, sizeof(token));
}

static void profile_gate_end(struct benchmark_profile_gate *gate)
{
	const uint8_t token = 1;
	uint8_t reply;

	if (!gate->enabled)
		return;
	profile_gate_write(gate->done_fd, &token, sizeof(token));
	profile_gate_read(gate->finish_fd, &reply, sizeof(reply));
}

enum benchmark_memory_policy {
	MEMORY_DEFAULT,
	MEMORY_PREFAULT,
	MEMORY_LOCKED,
};

static enum benchmark_memory_policy benchmark_memory_policy(void)
{
	const char *value = getenv("PW_BENCHMARK_MEMORY");

	if (value == NULL || spa_streq(value, "default"))
		return MEMORY_DEFAULT;
	if (spa_streq(value, "prefault"))
		return MEMORY_PREFAULT;
	spa_assert_se(spa_streq(value, "locked"));
	return MEMORY_LOCKED;
}

static const char *memory_policy_name(enum benchmark_memory_policy policy)
{
	switch (policy) {
	case MEMORY_DEFAULT:
		return "default";
	case MEMORY_PREFAULT:
		return "prefault";
	case MEMORY_LOCKED:
		return "locked";
	}
	spa_assert_not_reached();
	return "invalid";
}

static void destroy_node(struct node_instance *instance)
{
	spa_hook_remove(&instance->listener);
	spa_assert_se(spa_handle_clear(instance->handle) == 0);
	free(instance->handle);
}

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	spa_assert_se(clock_gettime(CLOCK_MONOTONIC_RAW, &value) == 0);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
			(uint64_t)value.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static size_t percentile_index(size_t count, uint32_t numerator,
		uint32_t denominator)
{
	return ((count * numerator + denominator - 1u) / denominator) - 1u;
}

static void report_latency(const char *name, uint64_t *samples, size_t count)
{
	long double sum = 0.0;
	size_t i, p90, p99, p999;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (i = 0; i < count; i++)
		sum += samples[i];
	p90 = percentile_index(count, 90u, 100u);
	p99 = percentile_index(count, 99u, 100u);
	p999 = percentile_index(count, 999u, 1000u);
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64 " ns p99=%" PRIu64,
			name, count, sum / (long double)count,
			samples[count / 2u], samples[p90], samples[p99]);
	if (count >= 1000)
		printf(" ns p99.9=%" PRIu64, samples[p999]);
	printf(" ns max=%" PRIu64 " ns\n", samples[count - 1u]);
}

static void write_latency_csv(const char *path, const uint64_t *algorithm,
		const uint64_t *sink, const uint64_t *total, size_t count)
{
	FILE *output;
	size_t index;

	if (path == NULL)
		return;
	output = fopen(path, "we");
	spa_assert_se(output != NULL);
	spa_assert_se(fputs("iteration,algorithm_ns,sink_ns,total_ns\n", output) >= 0);
	for (index = 0; index < count; index++)
		spa_assert_se(fprintf(output, "%zu,%" PRIu64 ",%" PRIu64
				",%" PRIu64 "\n", index, algorithm[index], sink[index],
				total[index]) > 0);
	spa_assert_se(fclose(output) == 0);
}

static void prefault_latency_storage(uint64_t *algorithm, uint64_t *sink,
		uint64_t *total, size_t count)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	size_t index, step;
	volatile uint64_t *volatile_algorithm = algorithm;
	volatile uint64_t *volatile_sink = sink;
	volatile uint64_t *volatile_total = total;

	spa_assert_se(page_size > 0 && (size_t)page_size >= sizeof(uint64_t));
	step = (size_t)page_size / sizeof(uint64_t);
	for (index = 0; index < count; index += step) {
		volatile_algorithm[index] = 0;
		volatile_sink[index] = 0;
		volatile_total[index] = 0;
	}
	volatile_algorithm[count - 1u] = 0;
	volatile_sink[count - 1u] = 0;
	volatile_total[count - 1u] = 0;
}

int main(int argc, char **argv)
{
	struct loaded_plugin calculon, alpao;
	const struct spa_handle_factory *pixel_factory, *controller_factory,
			*normalizer_factory, *sink_factory;
	char matrix_path[32];
	struct spa_dict_item pixel_items[3];
	struct spa_dict pixel_info;
	struct spa_dict_item controller_items[14];
	struct spa_dict controller_info;
	struct spa_dict_item normalizer_items[4];
	struct spa_dict normalizer_info;
	struct spa_dict_item sink_items[4];
	struct spa_dict sink_info;
	const char *backend;
	const char *serial;
	char actuator_count_text[16];
	uint32_t actuator_count = DEFAULT_ACTUATORS;
	uint32_t width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
	uint32_t region_width = DEFAULT_REGION_WIDTH;
	uint32_t region_height = DEFAULT_REGION_HEIGHT;
	uint32_t region_count, slope_count;
	size_t pixel_count;
	char *detector_size_text, *region_size_text, *region_origins;
	struct node_instance pixel, controller, normalizer, sink;
	struct test_buffer raw, calibrated, physical, normalized[2];
	struct spa_io_buffers raw_input = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = SPA_ID_INVALID,
	};
	struct spa_io_buffers pixel_output = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = SPA_ID_INVALID,
	};
	struct spa_io_buffers controller_input = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = 0,
	};
	struct spa_io_buffers controller_output = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = SPA_ID_INVALID,
	};
	struct spa_io_buffers normalizer_input = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = 0,
	};
	struct spa_io_buffers sink_input = {
		.status = SPA_STATUS_NEED_DATA, .buffer_id = SPA_ID_INVALID,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint16_t *raw_values;
	float *physical_values;
	double *normalized_values = NULL;
	uint32_t index;
	bool nonzero = false;
	size_t benchmark_iterations = 0, benchmark_warmup = 0, iteration;
	uint64_t *algorithm_latency = NULL, *sink_latency = NULL,
			*total_latency = NULL;
	struct benchmark_profile_gate profile_gate;
	const enum benchmark_memory_policy memory_policy = benchmark_memory_policy();
	struct rusage first_before, first_after;
	uint64_t first_start_ns, first_algorithms_done_ns;
	uint64_t first_sink_start_ns, first_done_ns;

	spa_assert_se(argc >= 3 && (argc <= 7 || argc == 11));
	profile_gate_init(&profile_gate);
	backend = argc >= 4 ? argv[3] : "mock";
	if (getenv("PW_BENCHMARK_WARMUP") != NULL) {
		char *end = NULL;
		unsigned long long parsed;

		errno = 0;
		parsed = strtoull(getenv("PW_BENCHMARK_WARMUP"), &end, 10);
		spa_assert_se(errno == 0 && end != getenv("PW_BENCHMARK_WARMUP") &&
				*end == '\0' && parsed <= SIZE_MAX);
		benchmark_warmup = (size_t)parsed;
	}
	if (argc >= 5) {
		char *end = NULL;
		unsigned long parsed = strtoul(argv[4], &end, 10);
		spa_assert_se(end != argv[4] && *end == '\0' && parsed > 0);
		benchmark_iterations = (size_t)parsed;
	}
	if (argc >= 6)
		actuator_count = parse_u32_argument(argv[5], MAX_ACTUATORS);
	serial = argc >= 7 ? argv[6] :
			(spa_streq(backend, "asdk") ? "SIM001" : "");
	if (argc == 11) {
		width = parse_u32_argument(argv[7], MAX_DETECTOR_EXTENT);
		height = parse_u32_argument(argv[8], MAX_DETECTOR_EXTENT);
		region_width = parse_u32_argument(argv[9], width);
		region_height = parse_u32_argument(argv[10], height);
	}
	pixel_count = (size_t)width * height;
	spa_assert_se(pixel_count <= UINT32_MAX / sizeof(float));
	if (memory_policy == MEMORY_LOCKED)
		spa_assert_se(mlockall(MCL_CURRENT | MCL_FUTURE) == 0);
	detector_size_text = make_size_text(width, height);
	region_size_text = make_size_text(region_width, region_height);
	region_origins = make_region_origins(width, height, region_width,
			region_height, &region_count);
	slope_count = region_count * 2u;
	if (benchmark_iterations != 0) {
		algorithm_latency = calloc(benchmark_iterations,
				sizeof(*algorithm_latency));
		sink_latency = calloc(benchmark_iterations, sizeof(*sink_latency));
		total_latency = calloc(benchmark_iterations, sizeof(*total_latency));
		spa_assert_se(algorithm_latency != NULL && sink_latency != NULL &&
				total_latency != NULL);
		prefault_latency_storage(algorithm_latency, sink_latency, total_latency,
				benchmark_iterations);
	}
	pixel_items[0] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_SIZE, detector_size_text);
	pixel_items[1] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1");
	pixel_items[2] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile);
	pixel_info = SPA_DICT_INIT(pixel_items, SPA_N_ELEMENTS(pixel_items));
	spa_assert_se(snprintf(actuator_count_text, sizeof(actuator_count_text),
			"%u", actuator_count) > 0);
	sink_items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, backend);
	sink_items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_SERIAL, serial);
	sink_items[2] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_ALPAO_ACTUATOR_COUNT, actuator_count_text);
	sink_items[3] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, profile);
	sink_info = SPA_DICT_INIT(sink_items, SPA_N_ELEMENTS(sink_items));
	normalizer_items[0] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1");
	normalizer_items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE,
			profile);
	normalizer_items[2] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_ALPAO_ACTUATOR_COUNT, actuator_count_text);
	normalizer_items[3] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_ALPAO_COMMAND_SCALE, "1");
	normalizer_info = SPA_DICT_INIT(normalizer_items,
			SPA_N_ELEMENTS(normalizer_items));
	write_matrix(matrix_path, actuator_count, slope_count);
	controller_items[0] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_SIZE, detector_size_text);
	controller_items[1] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_RATE, "1000/1");
	controller_items[2] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_DETECTOR_PROFILE, profile);
	controller_items[3] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_REGION_SIZE, region_size_text);
	controller_items[4] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_REGION_ORIGINS, region_origins);
	controller_items[5] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_ACTUATOR_COUNT, actuator_count_text);
	controller_items[6] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_RECONSTRUCTION_MATRIX_PATH, matrix_path);
	controller_items[7] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_COORDINATE_SCALE, "1");
	controller_items[8] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_PIXEL_THRESHOLD, "0");
	controller_items[9] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_FLUX_THRESHOLD, "1");
	controller_items[10] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_CONTROLLER_GAIN, "1");
	controller_items[11] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_CONTROLLER_POLE, "0");
	controller_items[12] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_COMMAND_MINIMUM, "-1");
	controller_items[13] = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_CALCULON_COMMAND_MAXIMUM, "1");
	controller_info = SPA_DICT_INIT(controller_items,
			SPA_N_ELEMENTS(controller_items));

	calculon = load_plugin(argv[1]);
	alpao = load_plugin(argv[2]);
	pixel_factory = find_factory(&calculon,
			SPA_NAME_API_CALCULON_PIXEL_CALIBRATION);
	controller_factory = find_factory(&calculon,
			SPA_NAME_API_CALCULON_SHWFS_CONTROLLER);
	normalizer_factory = find_factory(&calculon,
			SPA_NAME_API_ALPAO_COMMAND_NORMALIZATION);
	sink_factory = find_factory(&alpao, SPA_NAME_API_ALPAO_SINK);
	spa_assert_se(pixel_factory != NULL && controller_factory != NULL &&
			normalizer_factory != NULL && sink_factory != NULL);
	make_node(&pixel, pixel_factory, &pixel_info);
	make_node(&controller, controller_factory, &controller_info);
	make_node(&normalizer, normalizer_factory, &normalizer_info);
	make_node(&sink, sink_factory, &sink_info);
	spa_assert_se(unlink(matrix_path) == 0);

	configure_port(&pixel, SPA_DIRECTION_INPUT, 0);
	negotiate_ports(&pixel, 0, &controller, 0);
	negotiate_ports(&controller, 0, &normalizer, 0);
	negotiate_ports(&normalizer, 0, &sink, 0);

	init_buffer(&raw, (uint32_t)(pixel_count * sizeof(uint16_t)),
			(uint32_t)(pixel_count * sizeof(uint16_t)),
			(int32_t)(width * sizeof(uint16_t)));
	init_buffer(&calibrated, (uint32_t)(pixel_count * sizeof(float)), 0,
			(int32_t)(width * sizeof(float)));
	init_buffer(&physical, actuator_count * sizeof(float), 0, sizeof(float));
	init_buffer(&normalized[0], actuator_count * sizeof(double), 0,
			sizeof(double));
	init_buffer(&normalized[1], actuator_count * sizeof(double), 0,
			sizeof(double));
	use_one_buffer(pixel.node, SPA_DIRECTION_INPUT, 0, &raw);
	use_one_buffer(pixel.node, SPA_DIRECTION_OUTPUT, 0, &calibrated);
	use_one_buffer(controller.node, SPA_DIRECTION_INPUT, 0, &calibrated);
	use_one_buffer(controller.node, SPA_DIRECTION_OUTPUT, 0, &physical);
	use_one_buffer(normalizer.node, SPA_DIRECTION_INPUT, 0, &physical);
	use_two_buffers(normalizer.node, SPA_DIRECTION_OUTPUT, 0, normalized);
	use_two_buffers(sink.node, SPA_DIRECTION_INPUT, 0, normalized);
	set_standard_io(pixel.node, SPA_DIRECTION_INPUT, 0, &raw_input);
	set_standard_io(pixel.node, SPA_DIRECTION_OUTPUT, 0, &pixel_output);
	set_standard_io(controller.node, SPA_DIRECTION_INPUT, 0,
			&controller_input);
	set_standard_io(controller.node, SPA_DIRECTION_OUTPUT, 0,
			&controller_output);
	set_standard_io(normalizer.node, SPA_DIRECTION_INPUT, 0,
			&normalizer_input);
	set_standard_io(normalizer.node, SPA_DIRECTION_OUTPUT, 0, &sink_input);
	set_standard_io(sink.node, SPA_DIRECTION_INPUT, 0, &sink_input);

	raw_values = (uint16_t *)raw.payload;
	for (uint32_t row = 0; row < height; row++)
		for (uint32_t column = 0; column < width; column++)
			raw_values[(size_t)row * width + column] = 1u +
					(column % region_width) + 2u * (row % region_height);
	if (memory_policy == MEMORY_PREFAULT) {
		spa_assert_se(mlockall(MCL_CURRENT) == 0);
		spa_assert_se(munlockall() == 0);
	}
	printf("workload: detector=%ux%u region=%ux%u regions=%u slopes=%u "
			"actuators=%u matrix-bytes=%zu memory=%s warmup=%zu\n",
			width, height, region_width, region_height, region_count,
			slope_count, actuator_count,
			(size_t)actuator_count * slope_count * sizeof(float),
			memory_policy_name(memory_policy), benchmark_warmup);
	raw.header.seq = 1;
	spa_assert_se(spa_node_send_command(pixel.node, &start) == 0);
	spa_assert_se(spa_node_send_command(controller.node, &start) == 0);
	spa_assert_se(spa_node_send_command(normalizer.node, &start) == 0);
	spa_assert_se(spa_node_send_command(sink.node, &start) == 0);
	spa_assert_se(getrusage(RUSAGE_SELF, &first_before) == 0);
	first_start_ns = monotonic_ns();
	raw_input.status = SPA_STATUS_HAVE_DATA;
	raw_input.buffer_id = 0;
	spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
	controller_input.status = SPA_STATUS_HAVE_DATA;
	controller_input.buffer_id = pixel_output.buffer_id;
	spa_assert_se(spa_node_process(controller.node) == SPA_STATUS_HAVE_DATA);
	normalizer_input.status = SPA_STATUS_HAVE_DATA;
	normalizer_input.buffer_id = controller_output.buffer_id;
	spa_assert_se(spa_node_process(normalizer.node) == SPA_STATUS_HAVE_DATA);
	first_algorithms_done_ns = monotonic_ns();
	physical_values = (float *)physical.payload;
	for (index = 0; index < SPA_N_ELEMENTS(normalized); index++)
		if (normalized[index].chunk.size == actuator_count * sizeof(double))
			normalized_values = (double *)normalized[index].payload;
	spa_assert_se(normalized_values != NULL);
	for (index = 0; index < actuator_count; index++) {
		spa_assert_se(physical_values[index] >= -1.0f &&
				physical_values[index] <= 1.0f);
		spa_assert_se(normalized_values[index] ==
				(double)physical_values[index]);
		nonzero |= physical_values[index] != 0.0f;
	}
	spa_assert_se(nonzero);
	first_sink_start_ns = monotonic_ns();
	spa_assert_se(spa_node_process(sink.node) == SPA_STATUS_NEED_DATA);
	first_done_ns = monotonic_ns();
	spa_assert_se(getrusage(RUSAGE_SELF, &first_after) == 0);
	printf("first-cycle: algorithms=%" PRIu64 " ns sink=%" PRIu64
			" ns minor-faults=%ld major-faults=%ld\n",
			first_algorithms_done_ns - first_start_ns,
			first_done_ns - first_sink_start_ns,
			first_after.ru_minflt - first_before.ru_minflt,
			first_after.ru_majflt - first_before.ru_majflt);

	/* The device-boundary conversion rejects an unsafe physical command and
	 * does not publish a normalized buffer. */
	physical_values[0] = 2.0f;
	normalizer_input.status = SPA_STATUS_HAVE_DATA;
	normalizer_input.buffer_id = 0;
	spa_assert_se(spa_node_process(normalizer.node) == -ERANGE);
	spa_assert_se(normalizer_input.status == -ERANGE);
	spa_assert_se(spa_node_process(sink.node) == SPA_STATUS_NEED_DATA);
	physical_values[0] = 0.0f;

	pixel_output.status = SPA_STATUS_NEED_DATA;
	pixel_output.buffer_id = 0;
	controller_output.status = SPA_STATUS_NEED_DATA;
	controller_output.buffer_id = 0;
	measured_allocations = 0;
	measure_allocations = true;
	raw.header.seq++;
	raw_input.status = SPA_STATUS_HAVE_DATA;
	raw_input.buffer_id = 0;
	spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
	controller_input.status = SPA_STATUS_HAVE_DATA;
	controller_input.buffer_id = pixel_output.buffer_id;
	spa_assert_se(spa_node_process(controller.node) == SPA_STATUS_HAVE_DATA);
	normalizer_input.status = SPA_STATUS_HAVE_DATA;
	normalizer_input.buffer_id = controller_output.buffer_id;
	spa_assert_se(spa_node_process(normalizer.node) == SPA_STATUS_HAVE_DATA);
	measure_allocations = false;
	spa_assert_se(measured_allocations == 0);
	spa_assert_se(spa_node_process(sink.node) == SPA_STATUS_NEED_DATA);

	for (iteration = 0; iteration < benchmark_warmup; iteration++) {
		pixel_output.status = SPA_STATUS_NEED_DATA;
		pixel_output.buffer_id = 0;
		controller_output.status = SPA_STATUS_NEED_DATA;
		controller_output.buffer_id = 0;
		raw.header.seq++;
		raw_input.status = SPA_STATUS_HAVE_DATA;
		raw_input.buffer_id = 0;
		spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
		controller_input.status = SPA_STATUS_HAVE_DATA;
		controller_input.buffer_id = pixel_output.buffer_id;
		spa_assert_se(spa_node_process(controller.node) ==
				SPA_STATUS_HAVE_DATA);
		normalizer_input.status = SPA_STATUS_HAVE_DATA;
		normalizer_input.buffer_id = controller_output.buffer_id;
		spa_assert_se(spa_node_process(normalizer.node) ==
				SPA_STATUS_HAVE_DATA);
		spa_assert_se(spa_node_process(sink.node) == SPA_STATUS_NEED_DATA);
	}
	if (benchmark_iterations != 0)
		profile_gate_begin(&profile_gate);
	for (iteration = 0; iteration < benchmark_iterations; iteration++) {
		uint64_t start_ns, algorithms_done_ns, done_ns;

		pixel_output.status = SPA_STATUS_NEED_DATA;
		pixel_output.buffer_id = 0;
		controller_output.status = SPA_STATUS_NEED_DATA;
		controller_output.buffer_id = 0;
		start_ns = monotonic_ns();
		raw.header.seq++;
		raw_input.status = SPA_STATUS_HAVE_DATA;
		raw_input.buffer_id = 0;
		spa_assert_se(spa_node_process(pixel.node) == SPA_STATUS_HAVE_DATA);
		controller_input.status = SPA_STATUS_HAVE_DATA;
		controller_input.buffer_id = pixel_output.buffer_id;
		spa_assert_se(spa_node_process(controller.node) ==
				SPA_STATUS_HAVE_DATA);
		normalizer_input.status = SPA_STATUS_HAVE_DATA;
		normalizer_input.buffer_id = controller_output.buffer_id;
		spa_assert_se(spa_node_process(normalizer.node) ==
				SPA_STATUS_HAVE_DATA);
		algorithms_done_ns = monotonic_ns();
		spa_assert_se(spa_node_process(sink.node) == SPA_STATUS_NEED_DATA);
		done_ns = monotonic_ns();
		algorithm_latency[iteration] = algorithms_done_ns - start_ns;
		sink_latency[iteration] = done_ns - algorithms_done_ns;
		total_latency[iteration] = done_ns - start_ns;
	}
	if (benchmark_iterations != 0) {
		profile_gate_end(&profile_gate);
		write_latency_csv(getenv("PW_BENCHMARK_LATENCY_CSV"),
				algorithm_latency, sink_latency, total_latency,
				benchmark_iterations);
		report_latency("pixel+controller+normalizer SPA callbacks", algorithm_latency,
				benchmark_iterations);
		report_latency("scheduled submit+ALPAO sink+ASDK", sink_latency,
				benchmark_iterations);
		report_latency("raw detector frame to ALPAO completion", total_latency,
				benchmark_iterations);
	}

	spa_assert_se(spa_node_send_command(sink.node, &pause) == 0);
	spa_assert_se(spa_node_send_command(normalizer.node, &pause) == 0);
	spa_assert_se(spa_node_send_command(controller.node, &pause) == 0);
	spa_assert_se(spa_node_send_command(pixel.node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(pixel.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_set_io(normalizer.node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_set_io(sink.node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	destroy_node(&sink);
	destroy_node(&normalizer);
	destroy_node(&controller);
	destroy_node(&pixel);
	clear_buffer(&normalized[1]);
	clear_buffer(&normalized[0]);
	clear_buffer(&physical);
	clear_buffer(&calibrated);
	clear_buffer(&raw);
	spa_assert_se(dlclose(alpao.library) == 0);
	spa_assert_se(dlclose(calculon.library) == 0);
	free(region_origins);
	free(region_size_text);
	free(detector_size_text);
	free(total_latency);
	free(sink_latency);
	free(algorithm_latency);
	if (memory_policy == MEMORY_LOCKED)
		spa_assert_se(munlockall() == 0);
	return 0;
}

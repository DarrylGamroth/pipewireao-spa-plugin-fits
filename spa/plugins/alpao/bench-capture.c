/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/ndarray-utils.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/alpao.h>

#define ACTUATOR_COUNT 468u
#define CAPTURE_HEADER_BYTES 56u
#define CAPTURE_RECORD_VERSION 1u
#define CAPTURE_RECORD_SEND 1u
#define DEFAULT_WARMUP 10000u
#define DEFAULT_SAMPLES 100000u
#define MAX_CAPTURE_PACKET (1024u * 1024u)
#define MAX_SCHEDULE_SPAN_NS (UINT64_C(24) * 60u * 60u * 1000000000u)

#ifdef __OPTIMIZE__
#define BUILD_OPTIMIZED "yes"
#else
#define BUILD_OPTIMIZED "no"
#endif

static const char profile[] =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct capture_header {
	char magic[4];
	uint16_t version;
	uint16_t type;
	uint32_t header_bytes;
	uint32_t flags;
	uint64_t sequence;
	uint64_t monotonic_ns;
	uint64_t dropped_before;
	uint32_t word_count;
	uint32_t payload_bytes;
	uint32_t repeat_count;
	uint32_t frame_bytes;
};

_Static_assert(sizeof(struct capture_header) == CAPTURE_HEADER_BYTES,
		"capture record header layout changed");

struct captured_frame {
	uint64_t sequence;
	uint64_t monotonic_ns;
	uint64_t dropped_before;
	uint32_t word_count;
};

struct receiver {
	int fd;
	int cpu;
	struct captured_frame *frames;
	size_t capacity;
	_Atomic size_t n_frames;
	_Atomic int error;
	uint64_t packets;
};

struct measurement {
	uint64_t scheduled_ns;
	uint64_t process_start_ns;
	uint64_t process_end_ns;
	uint64_t capture_ns;
};

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(double) double command[ACTUATOR_COUNT];
};

struct options {
	const char *plugin;
	const char *raw_output;
	uint64_t warmup;
	uint64_t samples;
	uint64_t period_ns;
	uint64_t burst_size;
	uint64_t seed;
	int producer_cpu;
	int collector_cpu;
	int rt_priority;
	bool lock_memory;
	bool allow_drops;
};

static uint64_t monotonic_ns(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
			(uint64_t)value.tv_nsec;
}

static void sleep_until(uint64_t deadline_ns)
{
	struct timespec deadline = {
		.tv_sec = (time_t)(deadline_ns / UINT64_C(1000000000)),
		.tv_nsec = (long)(deadline_ns % UINT64_C(1000000000)),
	};
	int result;

	do {
		result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				&deadline, NULL);
	} while (result == EINTR);
	if (result != 0) {
		errno = result;
		perror("clock_nanosleep");
		exit(EXIT_FAILURE);
	}
}

static int pin_current_thread(int cpu)
{
	cpu_set_t affinity;

	if (cpu < 0)
		return 0;
	if (cpu >= CPU_SETSIZE)
		return -EINVAL;
	CPU_ZERO(&affinity);
	CPU_SET((unsigned int)cpu, &affinity);
	return pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
}

static void *receive_capture(void *data)
{
	struct receiver *receiver = data;
	uint8_t packet[MAX_CAPTURE_PACKET];
	int affinity_result;

	affinity_result = pin_current_thread(receiver->cpu);
	if (affinity_result != 0) {
		atomic_store_explicit(&receiver->error, affinity_result,
				memory_order_release);
		return NULL;
	}
	for (;;) {
		struct capture_header header;
		uint16_t first_word;
		ssize_t size;
		size_t index;

		size = recv(receiver->fd, packet, sizeof(packet), 0);
		if (size == 0)
			break;
		if (size < 0) {
			if (errno == EINTR)
				continue;
			atomic_store_explicit(&receiver->error, errno,
					memory_order_release);
			break;
		}
		receiver->packets++;
		if ((size_t)size < sizeof(header)) {
			atomic_store_explicit(&receiver->error, EPROTO,
					memory_order_release);
			break;
		}
		memcpy(&header, packet, sizeof(header));
		if (memcmp(header.magic, "AITC", 4) != 0 ||
				header.version != CAPTURE_RECORD_VERSION ||
				header.header_bytes != sizeof(header) ||
				header.payload_bytes != header.word_count * 2u ||
				(size_t)size != sizeof(header) + header.payload_bytes) {
			atomic_store_explicit(&receiver->error, EPROTO,
					memory_order_release);
			break;
		}
		if (header.type != CAPTURE_RECORD_SEND || header.word_count <= 1)
			continue;
		memcpy(&first_word, packet + sizeof(header), sizeof(first_word));
		if (first_word != UINT16_C(0xf800) &&
				first_word != UINT16_C(0xf600))
			continue;

		index = atomic_load_explicit(&receiver->n_frames,
				memory_order_relaxed);
		if (index >= receiver->capacity) {
			atomic_store_explicit(&receiver->error, ENOSPC,
					memory_order_release);
			break;
		}
		receiver->frames[index] = (struct captured_frame) {
			.sequence = header.sequence,
			.monotonic_ns = header.monotonic_ns,
			.dropped_before = header.dropped_before,
			.word_count = header.word_count,
		};
		atomic_store_explicit(&receiver->n_frames, index + 1,
				memory_order_release);
	}
	return NULL;
}

static int wait_for_initial_frame(struct receiver *receiver)
{
	const struct timespec interval = { .tv_nsec = 1000000 };
	unsigned int attempt;

	for (attempt = 0; attempt < 5000; attempt++) {
		if (atomic_load_explicit(&receiver->error,
				memory_order_acquire) != 0)
			return -1;
		if (atomic_load_explicit(&receiver->n_frames,
				memory_order_acquire) != 0)
			return 0;
		(void)nanosleep(&interval, NULL);
	}
	errno = ETIMEDOUT;
	return -1;
}

static void init_test_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = sizeof(storage->command);
	storage->data.data = storage->command;
	storage->data.chunk = &storage->chunk;
	storage->chunk.size = sizeof(storage->command);
	storage->chunk.stride = sizeof(double);
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static struct spa_pod *build_format(uint8_t *storage, size_t size)
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
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_profile, SPA_POD_String(profile));
}

static uint64_t next_random(uint64_t *state)
{
	uint64_t value = *state;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	*state = value;
	return value;
}

static void fill_command(double *command, uint64_t *state)
{
	uint32_t index;

	for (index = 0; index < ACTUATOR_COUNT; index++) {
		const uint64_t random = next_random(state);
		const double unit = (double)(random >> 11) * 0x1.0p-53;

		command[index] = unit * 2.0 - 1.0;
	}
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *values, size_t count,
		double requested)
{
	uint64_t *sorted;
	size_t rank;
	uint64_t result;

	if (count == 0)
		return 0;
	sorted = malloc(count * sizeof(*sorted));
	if (sorted == NULL) {
		perror("malloc percentile scratch");
		exit(EXIT_FAILURE);
	}
	memcpy(sorted, values, count * sizeof(*sorted));
	qsort(sorted, count, sizeof(*sorted), compare_u64);
	rank = (size_t)ceil(requested * (double)count / 100.0);
	if (rank == 0)
		rank = 1;
	if (rank > count)
		rank = count;
	result = sorted[rank - 1];
	free(sorted);
	return result;
}

static void print_distribution(const char *name, const uint64_t *values,
		size_t count)
{
	printf("%s samples=%zu p50_ns=%" PRIu64, name, count,
			percentile(values, count, 50.0));
	if (count >= 10)
		printf(" p90_ns=%" PRIu64, percentile(values, count, 90.0));
	if (count >= 100)
		printf(" p99_ns=%" PRIu64, percentile(values, count, 99.0));
	if (count >= 1000)
		printf(" p99_9_ns=%" PRIu64,
				percentile(values, count, 99.9));
	printf(" max_ns=%" PRIu64 "\n", percentile(values, count, 100.0));
}

static uint64_t parse_u64(const char *text, const char *option)
{
	char *end = NULL;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0') {
		fprintf(stderr, "invalid %s value: %s\n", option, text);
		exit(EXIT_FAILURE);
	}
	return (uint64_t)value;
}

static int parse_int(const char *text, const char *option)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' ||
			value < INT32_MIN || value > INT32_MAX) {
		fprintf(stderr, "invalid %s value: %s\n", option, text);
		exit(EXIT_FAILURE);
	}
	return (int)value;
}

static void usage(const char *program)
{
	fprintf(stderr,
			"usage: %s [options] /path/to/libspa-alpao.so\n"
			"  --warmup N          untimed warmed calls (default %u)\n"
			"  --samples N         measured calls (default %u)\n"
			"  --period-ns N       fixed open-loop period; zero is closed loop\n"
			"  --burst-size N      simultaneous arrivals per period-sized burst\n"
			"  --seed N            deterministic command seed\n"
			"  --producer-cpu N    pin the SPA processing thread\n"
			"  --collector-cpu N   pin the capture receiver thread\n"
			"  --rt-priority N     run SPA processing as SCHED_FIFO\n"
			"  --mlock             lock current and future mappings\n"
			"  --raw-output PATH   write per-sample CSV after measurement\n"
			"  --allow-drops       report capture loss without failing\n",
			program, DEFAULT_WARMUP, DEFAULT_SAMPLES);
}

static struct options parse_options(int argc, char **argv)
{
	struct options options = {
		.warmup = DEFAULT_WARMUP,
		.samples = DEFAULT_SAMPLES,
		.burst_size = 1,
		.seed = UINT64_C(0x243f6a8885a308d3),
		.producer_cpu = -1,
		.collector_cpu = -1,
	};
	int index;
	uint64_t schedule_span = 0;

	for (index = 1; index < argc; index++) {
		const char *argument = argv[index];

		if (strcmp(argument, "--mlock") == 0) {
			options.lock_memory = true;
			continue;
		}
		if (strcmp(argument, "--allow-drops") == 0) {
			options.allow_drops = true;
			continue;
		}
		if (argument[0] != '-') {
			if (options.plugin != NULL) {
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			options.plugin = argument;
			continue;
		}
		if (index + 1 >= argc) {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
		if (strcmp(argument, "--warmup") == 0)
			options.warmup = parse_u64(argv[++index], argument);
		else if (strcmp(argument, "--samples") == 0)
			options.samples = parse_u64(argv[++index], argument);
		else if (strcmp(argument, "--period-ns") == 0)
			options.period_ns = parse_u64(argv[++index], argument);
		else if (strcmp(argument, "--burst-size") == 0)
			options.burst_size = parse_u64(argv[++index], argument);
		else if (strcmp(argument, "--seed") == 0)
			options.seed = parse_u64(argv[++index], argument);
		else if (strcmp(argument, "--producer-cpu") == 0)
			options.producer_cpu = parse_int(argv[++index], argument);
		else if (strcmp(argument, "--collector-cpu") == 0)
			options.collector_cpu = parse_int(argv[++index], argument);
		else if (strcmp(argument, "--rt-priority") == 0)
			options.rt_priority = parse_int(argv[++index], argument);
		else if (strcmp(argument, "--raw-output") == 0)
			options.raw_output = argv[++index];
		else {
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}
	if (options.samples != 0 && options.period_ns != 0 &&
			options.burst_size != 0 && options.burst_size <=
					UINT64_MAX / options.period_ns) {
		const uint64_t interval = options.period_ns * options.burst_size;
		const uint64_t bursts = (options.samples - 1) / options.burst_size;

		schedule_span = bursts > MAX_SCHEDULE_SPAN_NS / interval ?
				MAX_SCHEDULE_SPAN_NS + 1 : bursts * interval;
	}
	if (options.plugin == NULL || options.samples == 0 || options.seed == 0 ||
			options.burst_size == 0 ||
			(options.burst_size != 1 && options.period_ns == 0) ||
			(options.period_ns != 0 && options.burst_size >
					UINT64_MAX / options.period_ns) ||
			schedule_span > MAX_SCHEDULE_SPAN_NS ||
			options.samples > SIZE_MAX - 1 ||
			options.warmup > SIZE_MAX - (size_t)options.samples - 1 ||
			options.rt_priority < 0 || options.rt_priority > 99) {
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	return options;
}

static int set_realtime(int priority)
{
	struct sched_param parameters = { .sched_priority = priority };

	if (priority == 0)
		return 0;
	return pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters);
}

static int leave_realtime(void)
{
	const struct sched_param parameters = { .sched_priority = 0 };

	return pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameters);
}

static int write_raw_results(const char *path,
		const struct measurement *measurements, size_t warmup, size_t count,
		uint64_t period_ns)
{
	FILE *output;
	size_t index;

	if (path == NULL)
		return 0;
	output = fopen(path, "wx");
	if (output == NULL) {
		perror(path);
		return -1;
	}
	fprintf(output, "sample,scheduled_ns,process_start_ns,capture_ns,"
			"process_end_ns,service_ns,to_capture_ns,scheduled_to_capture_ns\n");
	for (index = 0; index < count; index++) {
		const struct measurement *item = &measurements[1 + warmup + index];
		const uint64_t scheduled = period_ns == 0 ?
				item->process_start_ns : item->scheduled_ns;

		fprintf(output, "%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64
				",%" PRIu64 ",%" PRIu64 ",",
				index, scheduled, item->process_start_ns, item->capture_ns,
				item->process_end_ns,
				item->process_end_ns - item->process_start_ns);
		if (item->capture_ns != 0)
			fprintf(output, "%" PRIu64 ",%" PRIu64,
					item->capture_ns - item->process_start_ns,
					item->capture_ns - scheduled);
		fprintf(output, "\n");
	}
	if (fclose(output) != 0) {
		perror(path);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const struct options options = parse_options(argc, argv);
	const size_t attempts = (size_t)(1 + options.warmup + options.samples);
	struct measurement *measurements = NULL;
	struct captured_frame *captured = NULL;
	uint64_t *service = NULL, *to_capture = NULL, *scheduled_to_capture = NULL;
	uint64_t *lateness = NULL;
	struct receiver receiver = { .cpu = options.collector_cpu };
	int sockets[2] = { -1, -1 };
	pthread_t receiver_thread;
	bool receiver_started = false;
	bool handle_initialized = false;
	bool realtime_active = false;
	void *library = NULL, *symbol;
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t factory_index = 0;
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_BACKEND, "asdk"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_SERIAL, "CAP468"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_ACTUATOR_COUNT, "468"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_ALPAO_PROFILE, profile),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	struct spa_handle *handle = NULL;
	struct spa_node *node = NULL;
	uint8_t format_storage[1024];
	struct spa_pod *format;
	struct test_buffer storage[2];
	struct spa_buffer *buffers[] = {
		&storage[0].buffer,
		&storage[1].buffer,
	};
	struct spa_io_buffers io = {
		.status = SPA_STATUS_NEED_DATA,
		.buffer_id = SPA_ID_INVALID,
	};
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	uint64_t random_state = options.seed;
	uint64_t base_sequence, schedule_origin = 0, timer_overhead = UINT64_MAX;
	uint64_t deadline_misses = 0;
	size_t index, captured_samples = 0;
	int result = EXIT_FAILURE;
	struct utsname system;
	int setup_result;

	measurements = calloc(attempts, sizeof(*measurements));
	captured = calloc(attempts + 16, sizeof(*captured));
	service = malloc((size_t)options.samples * sizeof(*service));
	to_capture = malloc((size_t)options.samples * sizeof(*to_capture));
	scheduled_to_capture = malloc((size_t)options.samples *
			sizeof(*scheduled_to_capture));
	lateness = malloc((size_t)options.samples * sizeof(*lateness));
	if (measurements == NULL || captured == NULL || service == NULL ||
			to_capture == NULL || scheduled_to_capture == NULL ||
			lateness == NULL) {
		perror("allocate benchmark storage");
		goto cleanup;
	}
	receiver.frames = captured;
	receiver.capacity = attempts + 16;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) {
		perror("socketpair");
		goto cleanup;
	}
	{
		const int socket_buffer = 4 * 1024 * 1024;
		char descriptor[32];

		(void)setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF,
				&socket_buffer, sizeof(socket_buffer));
		snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]);
		if (setenv("ALPAO_CAPTURE_FD", descriptor, 1) != 0) {
			perror("setenv ALPAO_CAPTURE_FD");
			goto cleanup;
		}
	}
	receiver.fd = sockets[0];
	{
		const int thread_result = pthread_create(&receiver_thread, NULL,
				receive_capture, &receiver);

		if (thread_result != 0) {
			errno = thread_result;
			perror("pthread_create");
			goto cleanup;
		}
	}
	receiver_started = true;

	library = dlopen(options.plugin, RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		fprintf(stderr, "dlopen %s: %s\n", options.plugin, dlerror());
		goto cleanup;
	}
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	if (symbol == NULL || sizeof(enumerate) != sizeof(symbol)) {
		fprintf(stderr, "ALPAO SPA factory symbol is unavailable\n");
		goto cleanup;
	}
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	if (enumerate(&factory, &factory_index) != 1 || factory == NULL) {
		fprintf(stderr, "ALPAO SPA factory enumeration failed\n");
		goto cleanup;
	}
	handle = calloc(1, spa_handle_factory_get_size(factory, &info));
	if (handle == NULL) {
		perror("allocate ALPAO SPA handle");
		goto cleanup;
	}
	setup_result = spa_handle_factory_init(factory, handle, &info, NULL, 0);
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA factory initialization failed: %d\n",
				setup_result);
		goto cleanup;
	}
	handle_initialized = true;
	setup_result = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node);
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA node lookup failed: %d\n", setup_result);
		goto cleanup;
	}
	format = build_format(format_storage, sizeof(format_storage));
	init_test_buffer(&storage[0]);
	init_test_buffer(&storage[1]);
	if (format == NULL) {
		fprintf(stderr, "ALPAO SPA format construction failed\n");
		goto cleanup;
	}
	setup_result = spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, format);
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA format setup failed: %d\n", setup_result);
		goto cleanup;
	}
	setup_result = spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers));
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA buffer setup failed: %d\n", setup_result);
		goto cleanup;
	}
	setup_result = spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io));
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA buffer I/O setup failed: %d\n",
				setup_result);
		goto cleanup;
	}
	setup_result = spa_node_send_command(node, &start);
	if (setup_result != 0) {
		fprintf(stderr, "ALPAO SPA Start failed: %d\n", setup_result);
		goto cleanup;
	}
	if (wait_for_initial_frame(&receiver) != 0) {
		perror("waiting for capture-interface startup reset");
		goto cleanup;
	}
	base_sequence = captured[0].sequence + 1;
	close(sockets[1]);
	sockets[1] = -1;

	if (options.lock_memory && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		perror("mlockall");
		goto cleanup;
	}
	{
		const int affinity_result = pin_current_thread(options.producer_cpu);
		const int realtime_result = set_realtime(options.rt_priority);

		if (affinity_result != 0) {
			errno = affinity_result;
			perror("producer affinity");
			goto cleanup;
		}
		if (realtime_result != 0) {
			errno = realtime_result;
			perror("SCHED_FIFO");
			goto cleanup;
		}
		realtime_active = options.rt_priority != 0;
	}
	for (index = 0; index < 10000; index++) {
		const uint64_t before = monotonic_ns();
		const uint64_t after = monotonic_ns();

		if (after - before < timer_overhead)
			timer_overhead = after - before;
	}

	for (index = 0; index < attempts; index++) {
		struct measurement *item = &measurements[index];
		const uint32_t buffer_id = (uint32_t)(index % SPA_N_ELEMENTS(storage));
		int process_result;

		fill_command(storage[buffer_id].command, &random_state);
		if (index == 1 + options.warmup && options.period_ns != 0)
			schedule_origin = monotonic_ns() + UINT64_C(100000000);
		if (index >= 1 + options.warmup && options.period_ns != 0) {
			const uint64_t measured_index = index - 1 - options.warmup;
			const uint64_t burst = measured_index / options.burst_size;

			item->scheduled_ns = schedule_origin + burst *
					options.period_ns * options.burst_size;
			sleep_until(item->scheduled_ns);
		}
		io.buffer_id = buffer_id;
		io.status = SPA_STATUS_HAVE_DATA;
		item->process_start_ns = monotonic_ns();
		if (options.period_ns == 0)
			item->scheduled_ns = item->process_start_ns;
		process_result = spa_node_process(node);
		item->process_end_ns = monotonic_ns();
		if (process_result != SPA_STATUS_NEED_DATA ||
				io.status != SPA_STATUS_NEED_DATA ||
				io.buffer_id != buffer_id) {
			fprintf(stderr, "SPA processing failed at %zu: %d\n",
					index, process_result);
			goto cleanup;
		}
	}
	if (options.rt_priority != 0) {
		const int scheduler_result = leave_realtime();

		if (scheduler_result != 0) {
			errno = scheduler_result;
			perror("restore SCHED_OTHER");
			goto cleanup;
		}
		realtime_active = false;
	}

	if (spa_node_send_command(node, &pause) != 0) {
		fprintf(stderr, "ALPAO SPA Pause failed\n");
		goto cleanup;
	}
	(void)spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, NULL, 0);
	(void)spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0, NULL, 0);
	(void)spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, NULL);
	if (spa_handle_clear(handle) != 0) {
		fprintf(stderr, "ALPAO SPA handle clear failed\n");
		goto cleanup;
	}
	handle_initialized = false;
	free(handle);
	handle = NULL;
	if (dlclose(library) != 0) {
		fprintf(stderr, "ALPAO SPA library close failed\n");
		library = NULL;
		goto cleanup;
	}
	library = NULL;
	if (receiver_started) {
		if (pthread_join(receiver_thread, NULL) != 0) {
			fprintf(stderr, "capture receiver join failed\n");
			goto cleanup;
		}
		receiver_started = false;
	}
	if (atomic_load_explicit(&receiver.error, memory_order_acquire) != 0) {
		fprintf(stderr, "capture receiver failed: %s\n",
				strerror(atomic_load_explicit(&receiver.error,
						memory_order_relaxed)));
		goto cleanup;
	}

	for (index = 0;
			index < atomic_load_explicit(&receiver.n_frames,
					memory_order_acquire); index++) {
		const struct captured_frame *frame = &captured[index];
		size_t command_index;

		if (frame->sequence < base_sequence)
			continue;
		command_index = (size_t)(frame->sequence - base_sequence);
		if (command_index >= attempts)
			continue;
		if (frame->word_count != captured[0].word_count) {
			fprintf(stderr, "capture frame size changed at sequence %" PRIu64
					"\n", frame->sequence);
			goto cleanup;
		}
		measurements[command_index].capture_ns = frame->monotonic_ns;
	}

	for (index = 0; index < (size_t)options.samples; index++) {
		const struct measurement *item =
				&measurements[1 + (size_t)options.warmup + index];
		const uint64_t scheduled = options.period_ns == 0 ?
				item->process_start_ns : item->scheduled_ns;

		service[index] = item->process_end_ns - item->process_start_ns;
		lateness[index] = item->process_start_ns - scheduled;
		if (options.period_ns != 0 &&
				item->process_end_ns > scheduled + options.period_ns)
			deadline_misses++;
		if (item->capture_ns == 0)
			continue;
		if (item->capture_ns < item->process_start_ns ||
				item->capture_ns > item->process_end_ns) {
			fprintf(stderr, "capture timestamp is outside SPA call at %zu\n",
					index);
			goto cleanup;
		}
		to_capture[captured_samples] =
				item->capture_ns - item->process_start_ns;
		scheduled_to_capture[captured_samples] =
				item->capture_ns - scheduled;
		captured_samples++;
	}

	if (uname(&system) == 0)
		printf("environment sysname=%s release=%s machine=%s compiler=%s "
				"optimized=%s\n", system.sysname, system.release,
				system.machine, __VERSION__, BUILD_OPTIMIZED);
	printf("workload actuators=%u warmup=%" PRIu64 " samples=%" PRIu64
			" period_ns=%" PRIu64 " burst_size=%" PRIu64
			" seed=%" PRIu64
			" producer_cpu=%d collector_cpu=%d rt_priority=%d mlock=%s\n",
			ACTUATOR_COUNT, options.warmup, options.samples,
			options.period_ns, options.burst_size, options.seed,
			options.producer_cpu,
			options.collector_cpu, options.rt_priority,
			options.lock_memory ? "yes" : "no");
	printf("boundary capture_timestamp=ASDK-packed-before-interface-sendmsg "
			"clock=CLOCK_MONOTONIC timer_min_pair_ns=%" PRIu64 "\n",
			timer_overhead);
	printf("correctness expected_frames=%zu captured_frames=%zu "
			"missing_frames=%zu receiver_packets=%" PRIu64
			" deadline_misses=%" PRIu64 "\n",
			(size_t)options.samples, captured_samples,
			(size_t)options.samples - captured_samples, receiver.packets,
			deadline_misses);
	printf("first_use service_ns=%" PRIu64,
			measurements[0].process_end_ns - measurements[0].process_start_ns);
	if (measurements[0].capture_ns != 0)
		printf(" to_capture_ns=%" PRIu64,
				measurements[0].capture_ns - measurements[0].process_start_ns);
	printf("\n");
	print_distribution("service", service, (size_t)options.samples);
	print_distribution("process_to_capture", to_capture, captured_samples);
	if (options.period_ns != 0) {
		print_distribution("schedule_lateness", lateness,
				(size_t)options.samples);
		print_distribution("scheduled_to_capture", scheduled_to_capture,
				captured_samples);
	}
	if (write_raw_results(options.raw_output, measurements,
			(size_t)options.warmup, (size_t)options.samples,
			options.period_ns) != 0)
		goto cleanup;
	if (!options.allow_drops && captured_samples != (size_t)options.samples) {
		fprintf(stderr, "capture loss detected; use --allow-drops only for "
				"intentional overload runs\n");
		goto cleanup;
	}
	result = EXIT_SUCCESS;

cleanup:
	if (realtime_active)
		(void)leave_realtime();
	if (handle_initialized) {
		(void)spa_handle_clear(handle);
	}
	free(handle);
	if (library != NULL)
		(void)dlclose(library);
	if (sockets[1] >= 0)
		close(sockets[1]);
	if (receiver_started) {
		if (sockets[0] >= 0)
			shutdown(sockets[0], SHUT_RDWR);
		(void)pthread_join(receiver_thread, NULL);
	}
	if (sockets[0] >= 0)
		close(sockets[0]);
	free(lateness);
	free(scheduled_to_capture);
	free(to_capture);
	free(service);
	free(captured);
	free(measurements);
	return result;
}

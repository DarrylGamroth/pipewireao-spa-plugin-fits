/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/utils/string.h>

#define N_BUFFERS 4u
#define DEFAULT_ITERATIONS 200000u
#define DEFAULT_QUEUE_ITERATIONS 1000u
#define QUEUE_WARMUP_FRAMES 32u

enum benchmark_operation {
	BENCHMARK_CALLBACK,
	BENCHMARK_POLLING,
	BENCHMARK_QUEUE,
};

struct slot {
	BGAPI2_Buffer *buffer;
	void *memory;
};

static uint64_t monotonic_raw_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *samples, size_t count,
		double quantile)
{
	size_t index;

	if (count == 0)
		return 0;
	index = (size_t)(quantile * (double)(count - 1));
	return samples[index];
}

static void report(const char *name, uint64_t *samples, size_t count)
{
	long double total = 0.0;
	size_t i;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (i = 0; i < count; i++)
		total += samples[i];
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64 " ns p99=%" PRIu64
			" ns p99.9=%" PRIu64 " ns max=%" PRIu64 " ns\n",
			name, count, count == 0 ? 0.0L : total / count,
			percentile(samples, count, 0.50),
			percentile(samples, count, 0.90),
			percentile(samples, count, 0.99),
			percentile(samples, count, 0.999),
			percentile(samples, count, 1.0));
}

static void release_slots(struct bgapi2_camera *camera,
		struct slot slots[N_BUFFERS])
{
	uint32_t i;

	(void)bgapi2_camera_stop(camera);
	(void)bgapi2_camera_discard_buffers(camera);
	for (i = 0; i < N_BUFFERS; i++) {
		if (slots[i].buffer != NULL)
			(void)bgapi2_camera_revoke(camera, &slots[i].buffer);
		free(slots[i].memory);
	}
}

static int wait_for_completion(struct bgapi2_camera *camera,
		struct bgapi2_camera_completion *completion)
{
	uint64_t deadline = monotonic_raw_nsec() + 3000000000u;
	int res;

	do {
		res = bgapi2_camera_try_get_completion(camera, completion);
		if (res == 0)
			sched_yield();
	} while (res == 0 && monotonic_raw_nsec() < deadline);
	return res;
}

int main(int argc, char *argv[])
{
	struct bgapi2_camera_options options = {
		.interface_index = BGAPI2_CAMERA_ANY_INTERFACE,
		.interface_timeout_ms = 100,
		.device_timeout_ms = 200,
	};
	struct bgapi2_camera_completion completion;
	struct bgapi2_camera *camera = NULL;
	const struct bgapi2_camera_info *info;
	struct slot slots[N_BUFFERS] = { 0 };
	uint64_t *clock_samples = NULL, *operation_samples = NULL;
	uint64_t before, after;
	uint32_t iterations, i;
	size_t sample_count = 0;
	uint32_t frames = 0;
	enum benchmark_operation operation;
	const char *operation_name;
	int call_res = -EIO, exit_status = EXIT_FAILURE;
	const char *stage = "argument validation";

	if (argc < 3 || argc > 4) {
		fprintf(stderr, "usage: %s PRODUCER.cti callback|polling|queue [ITERATIONS]\n",
				argv[0]);
		return EXIT_FAILURE;
	}
	options.producer_path = argv[1];
	operation_name = argv[2];
	if (strcmp(argv[2], "callback") == 0) {
		operation = BENCHMARK_CALLBACK;
		options.completion_mode = BGAPI2_CAMERA_COMPLETION_CALLBACK;
	} else if (strcmp(argv[2], "polling") == 0) {
		operation = BENCHMARK_POLLING;
		options.completion_mode = BGAPI2_CAMERA_COMPLETION_POLLING;
	} else if (strcmp(argv[2], "queue") == 0) {
		operation = BENCHMARK_QUEUE;
		options.completion_mode = BGAPI2_CAMERA_COMPLETION_CALLBACK;
	} else {
		return EXIT_FAILURE;
	}
	iterations = operation == BENCHMARK_QUEUE ? DEFAULT_QUEUE_ITERATIONS :
			DEFAULT_ITERATIONS;
	if (argc == 4 && (!spa_atou32(argv[3], &iterations, 0) ||
			iterations == 0))
		return EXIT_FAILURE;
	clock_samples = malloc((size_t)iterations * sizeof(*clock_samples));
	operation_samples = malloc((size_t)iterations * sizeof(*operation_samples));
	stage = "sample allocation";
	if (clock_samples == NULL || operation_samples == NULL)
		goto done;
	for (i = 0; i < iterations; i++) {
		before = monotonic_raw_nsec();
		after = monotonic_raw_nsec();
		clock_samples[i] = after - before;
	}
	stage = "camera open";
	if ((call_res = bgapi2_camera_open(&camera, &options)) < 0)
		goto done;
	stage = "camera information";
	info = bgapi2_camera_get_info(camera);
	if (info == NULL || info->payload_size == 0)
		goto done;
	for (i = 0; i < N_BUFFERS; i++) {
		stage = "buffer preparation";
		slots[i].memory = malloc(info->payload_size);
		if (slots[i].memory == NULL ||
				bgapi2_camera_announce(camera, slots[i].memory,
				info->payload_size, &slots[i], &slots[i].buffer) < 0 ||
				bgapi2_camera_queue(camera, slots[i].buffer) < 0)
			goto done;
	}
	stage = "acquisition start";
	if ((call_res = bgapi2_camera_start(camera)) < 0)
		goto done;
	stage = "first completion";
	call_res = wait_for_completion(camera, &completion);
	if (call_res != 1 || completion.result < 0 ||
			bgapi2_camera_queue(camera, completion.buffer) < 0)
		goto done;
	if (operation == BENCHMARK_QUEUE) {
		stage = "queue warmup";
		for (i = 0; i < QUEUE_WARMUP_FRAMES; i++) {
			call_res = wait_for_completion(camera, &completion);
			if (call_res != 1 || completion.result < 0 ||
					bgapi2_camera_queue(camera, completion.buffer) < 0)
				goto done;
		}
		stage = "queue measurement";
		for (i = 0; i < iterations; i++) {
			call_res = wait_for_completion(camera, &completion);
			if (call_res != 1 || completion.result < 0)
				goto done;
			before = monotonic_raw_nsec();
			call_res = bgapi2_camera_queue(camera, completion.buffer);
			after = monotonic_raw_nsec();
			if (call_res < 0)
				goto done;
			operation_samples[sample_count++] = after - before;
		}
		printf("producer=%s mode=%s cpu=%d iterations=%u warmup=%u\n",
				argv[1], operation_name, sched_getcpu(), iterations,
				QUEUE_WARMUP_FRAMES);
		report("clock-pair baseline", clock_samples, iterations);
		report("QueueBuffer service time", operation_samples, sample_count);
		exit_status = EXIT_SUCCESS;
		goto done;
	}
	for (i = 0; i < iterations; i++) {
		stage = "measurement";
		before = monotonic_raw_nsec();
		call_res = bgapi2_camera_try_get_completion(camera, &completion);
		after = monotonic_raw_nsec();
		if (call_res < 0)
			goto done;
		if (call_res == 0)
			operation_samples[sample_count++] = after - before;
		else {
			if (completion.result < 0 ||
					bgapi2_camera_queue(camera, completion.buffer) < 0)
				goto done;
			frames++;
		}
	}
	printf("producer=%s mode=%s cpu=%d iterations=%u empty=%zu frames=%u\n",
			argv[1], operation_name, sched_getcpu(), iterations, sample_count,
			frames);
	report("clock-pair baseline", clock_samples, iterations);
	report("empty completion poll", operation_samples, sample_count);
	exit_status = EXIT_SUCCESS;

done:
	if (exit_status != EXIT_SUCCESS)
		fprintf(stderr, "completion benchmark failed during %s (result %d)\n",
				stage, call_res);
	if (camera != NULL) {
		release_slots(camera, slots);
		bgapi2_camera_close(camera);
	}
	free(clock_samples);
	free(operation_samples);
	return exit_status;
}

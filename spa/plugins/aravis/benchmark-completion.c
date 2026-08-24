/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <spa/utils/string.h>

#define N_BUFFERS 8u
#define DEFAULT_ITERATIONS 1000000u

struct slot {
	ArvBuffer *buffer;
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
	return count == 0 ? 0 : samples[(size_t)(quantile * (double)(count - 1))];
}

static void report(const char *name, uint64_t *samples, size_t count)
{
	long double total = 0.0;
	size_t i;

	qsort(samples, count, sizeof(*samples), compare_u64);
	for (i = 0; i < count; i++)
		total += samples[i];
	printf("%s: n=%zu mean=%.1Lf ns p50=%" PRIu64
			" ns p90=%" PRIu64,
			name, count, count == 0 ? 0.0L : total / count,
			percentile(samples, count, 0.50),
			percentile(samples, count, 0.90));
	if (count >= 100)
		printf(" ns p99=%" PRIu64, percentile(samples, count, 0.99));
	if (count >= 1000)
		printf(" ns p99.9=%" PRIu64, percentile(samples, count, 0.999));
	printf(" ns max=%" PRIu64 " ns\n", percentile(samples, count, 1.0));
}

static void release_slots(struct aravis_camera *camera,
		struct slot slots[N_BUFFERS])
{
	uint32_t i;

	(void)aravis_camera_stop(camera);
	for (i = 0; i < N_BUFFERS; i++) {
		if (slots[i].buffer != NULL)
			(void)aravis_camera_revoke(camera, &slots[i].buffer);
		free(slots[i].memory);
	}
}

int main(int argc, char *argv[])
{
	struct aravis_camera_options options = { 0 };
	struct aravis_camera_completion completion;
	struct aravis_camera *camera = NULL;
	const struct aravis_camera_info *info;
	struct slot slots[N_BUFFERS] = { 0 };
	uint64_t *clock_samples = NULL, *empty_samples = NULL, *frame_samples = NULL;
	uint64_t deadline, before, after;
	uint32_t iterations = DEFAULT_ITERATIONS, i;
	size_t empty_count = 0, frame_count = 0;
	int call_res = -EIO, exit_status = EXIT_FAILURE;
	const char *stage = "argument validation";

	if (argc < 2 || argc > 3 ||
			(argc == 3 && (!spa_atou32(argv[2], &iterations, 0) ||
			iterations == 0))) {
		fprintf(stderr, "usage: %s DEVICE-ID [ITERATIONS]\n", argv[0]);
		return EXIT_FAILURE;
	}
	options.device_id = argv[1];
	clock_samples = malloc((size_t)iterations * sizeof(*clock_samples));
	empty_samples = malloc((size_t)iterations * sizeof(*empty_samples));
	frame_samples = malloc((size_t)iterations * sizeof(*frame_samples));
	stage = "sample allocation";
	if (clock_samples == NULL || empty_samples == NULL || frame_samples == NULL)
		goto done;
	for (i = 0; i < iterations; i++) {
		before = monotonic_raw_nsec();
		after = monotonic_raw_nsec();
		clock_samples[i] = after - before;
	}
	stage = "camera open";
	if ((call_res = aravis_camera_open(&camera, &options)) < 0)
		goto done;
	stage = "camera information";
	info = aravis_camera_get_info(camera);
	if (info == NULL || info->payload_size == 0)
		goto done;
	for (i = 0; i < N_BUFFERS; i++) {
		stage = "buffer preparation";
		slots[i].memory = malloc(info->payload_size);
		if (slots[i].memory == NULL || aravis_camera_announce(camera,
				slots[i].memory, info->payload_size, &slots[i],
				&slots[i].buffer) < 0 ||
				aravis_camera_queue(camera, slots[i].buffer) < 0)
			goto done;
	}
	stage = "acquisition start";
	if ((call_res = aravis_camera_start(camera)) < 0)
		goto done;
	stage = "first completion";
	deadline = monotonic_raw_nsec() + 3000000000u;
	do {
		call_res = aravis_camera_try_get_completion(camera, &completion);
		if (call_res == 0)
			sched_yield();
	} while (call_res == 0 && monotonic_raw_nsec() < deadline);
	if (call_res != 1 || completion.result < 0 ||
			aravis_camera_queue(camera, completion.buffer) < 0)
		goto done;
	for (i = 0; i < iterations; i++) {
		stage = "measurement";
		before = monotonic_raw_nsec();
		call_res = aravis_camera_try_get_completion(camera, &completion);
		after = monotonic_raw_nsec();
		if (call_res < 0)
			goto done;
		if (call_res == 0)
			empty_samples[empty_count++] = after - before;
		else {
			frame_samples[frame_count++] = after - before;
			if (completion.result < 0 ||
					aravis_camera_queue(camera, completion.buffer) < 0)
				goto done;
		}
	}
	printf("device=%s cpu=%d iterations=%u empty=%zu frames=%zu\n",
			options.device_id, sched_getcpu(), iterations, empty_count,
			frame_count);
	report("clock-pair baseline", clock_samples, iterations);
	report("empty completion poll", empty_samples, empty_count);
	if (frame_count != 0)
		report("completed-frame poll", frame_samples, frame_count);
	exit_status = EXIT_SUCCESS;

done:
	if (exit_status != EXIT_SUCCESS)
		fprintf(stderr, "completion benchmark failed during %s (result %d)\n",
				stage, call_res);
	if (camera != NULL) {
		release_slots(camera, slots);
		aravis_camera_close(camera);
	}
	free(clock_samples);
	free(empty_samples);
	free(frame_samples);
	return exit_status;
}

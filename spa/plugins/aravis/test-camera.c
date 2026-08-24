/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N_BUFFERS 8u
#define N_FRAMES 16u

struct test_buffer {
	ArvBuffer *buffer;
	void *memory;
};

static int64_t monotonic_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	return (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
}

int main(int argc, char **argv)
{
	struct aravis_camera_options options = {
		.device_id = argc > 1 ? argv[1] : NULL,
	};
	struct test_buffer buffers[N_BUFFERS] = { 0 };
	struct aravis_camera *camera = NULL;
	const struct aravis_camera_info *info;
	uint64_t empty_polls = 0;
	uint32_t completed = 0;
	int64_t deadline;
	int res = EXIT_FAILURE;

	if (options.device_id == NULL || aravis_camera_open(&camera, &options) < 0)
		goto out;
	info = aravis_camera_get_info(camera);
	if (info == NULL)
		goto out;
	for (uint32_t i = 0; i < N_BUFFERS; i++) {
		buffers[i].memory = malloc(info->payload_size);
		if (buffers[i].memory == NULL || aravis_camera_announce(camera,
				buffers[i].memory, info->payload_size, &buffers[i],
				&buffers[i].buffer) < 0 ||
				aravis_camera_queue(camera, buffers[i].buffer) < 0)
			goto out;
	}
	if (aravis_camera_start(camera) < 0)
		goto out;
	deadline = monotonic_nsec() + 10 * 1000000000ll;
	while (completed < N_FRAMES) {
		struct aravis_camera_completion completion;

		int status = aravis_camera_try_get_completion(camera, &completion);
		if (status < 0)
			goto out;
		if (status == 0) {
			empty_polls++;
			if ((empty_polls & 0xffff) == 0 && monotonic_nsec() >= deadline)
				goto out;
			continue;
		}
		if (completion.result < 0 || completion.user_data == NULL ||
				aravis_camera_queue(camera, completion.buffer) < 0)
			goto out;
		completed++;
	}
	res = EXIT_SUCCESS;

out:
	if (camera != NULL)
		(void)aravis_camera_stop(camera);
	for (uint32_t i = 0; i < N_BUFFERS; i++) {
		if (camera != NULL && buffers[i].buffer != NULL)
			(void)aravis_camera_revoke(camera, &buffers[i].buffer);
		free(buffers[i].memory);
	}
	aravis_camera_close(camera);
	if (res == EXIT_SUCCESS)
		printf("Received %u direct GenTL frames after %" PRIu64
				" empty polls\n", completed, empty_polls);
	return res;
}

/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int capture_frames(struct aravis_camera *camera, uint32_t requested,
		uint64_t *empty_polls)
{
	int64_t deadline = monotonic_nsec() + 10 * 1000000000ll;
	uint32_t completed = 0;

	while (completed < requested) {
		struct aravis_camera_completion completion;
		int status = aravis_camera_try_get_completion(
				camera, &completion);

		if (status < 0)
			return status;
		if (status == 0) {
			(*empty_polls)++;
			if (((*empty_polls) & 0xffff) == 0 &&
					monotonic_nsec() >= deadline)
				return -1;
			continue;
		}
		if (completion.result < 0 || completion.user_data == NULL ||
				aravis_camera_queue(camera, completion.buffer) < 0)
			return -1;
		completed++;
	}
	return 0;
}

static enum aravis_transport parse_transport(const char *name)
{
	if (name == NULL)
		return ARAVIS_TRANSPORT_AUTO;
	if (strcmp(name, "gentl") == 0)
		return ARAVIS_TRANSPORT_GENTL;
	if (strcmp(name, "native-gv") == 0)
		return ARAVIS_TRANSPORT_NATIVE_GV;
	if (strcmp(name, "native-uv") == 0)
		return ARAVIS_TRANSPORT_NATIVE_UV;
	return ARAVIS_TRANSPORT_AUTO;
}

static const char *transport_name(enum aravis_transport transport)
{
	switch (transport) {
	case ARAVIS_TRANSPORT_GENTL:
		return "GenTL";
	case ARAVIS_TRANSPORT_NATIVE_GV:
		return "native GV";
	case ARAVIS_TRANSPORT_NATIVE_UV:
		return "native UV";
	case ARAVIS_TRANSPORT_AUTO:
		return "auto";
	}
	return "unknown";
}

int main(int argc, char **argv)
{
	struct aravis_camera_options options = {
		.device_id = argc > 1 ? argv[1] : NULL,
		.transport = parse_transport(argc > 2 ? argv[2] : NULL),
	};
	struct test_buffer buffers[N_BUFFERS] = { 0 };
	struct aravis_camera *camera = NULL;
	const struct aravis_camera_info *info;
	uint64_t empty_polls = 0;
	enum aravis_transport transport = ARAVIS_TRANSPORT_AUTO;
	int res = EXIT_FAILURE;

	if (options.device_id == NULL ||
			aravis_camera_open(&camera, &options) < 0)
		goto out;
	info = aravis_camera_get_info(camera);
	if (info == NULL)
		goto out;
	transport = aravis_camera_get_transport(camera);
	for (uint32_t i = 0; i < N_BUFFERS; i++) {
		buffers[i].memory = malloc(info->payload_size);
		if (buffers[i].memory == NULL ||
				aravis_camera_announce(camera,
						buffers[i].memory,
						info->payload_size, &buffers[i],
						&buffers[i].buffer) < 0 ||
				aravis_camera_queue(camera, buffers[i].buffer) <
						0)
			goto out;
	}
	if (aravis_camera_start(camera) < 0)
		goto out;
	if (capture_frames(camera, N_FRAMES, &empty_polls) < 0 ||
			aravis_camera_stop(camera) < 0)
		goto out;
	for (uint32_t i = 0; i < N_BUFFERS; i++)
		if (aravis_camera_queue(camera, buffers[i].buffer) < 0)
			goto out;
	if (aravis_camera_start(camera) < 0 ||
			capture_frames(camera, 1, &empty_polls) < 0)
		goto out;
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
		printf("Received %u %s frames after %" PRIu64 " empty polls\n",
				N_FRAMES + 1, transport_name(transport),
				empty_polls);
	return res;
}

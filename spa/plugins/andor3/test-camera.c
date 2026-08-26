/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/utils/defs.h>

#include "camera.h"

int main(void)
{
	struct andor3_camera_options options = { .device_index = 0 };
	struct andor3_camera_completion completion;
	struct andor3_camera_buffer *camera_buffers[2] = { NULL, NULL };
	struct andor3_feature_info feature_info;
	struct andor3_feature_value feature_value;
	const struct andor3_camera_info *info;
	struct andor3_camera *camera = NULL;
	void *memory[2] = { NULL, NULL };
	struct timespec delay = { .tv_nsec = 1000000 };
	uint32_t exposure_index;
	unsigned int attempts;
	int i;

	spa_assert_se(andor3_camera_open(&camera, &options) == 0);
	info = andor3_camera_get_info(camera);
	spa_assert_se(info != NULL && info->width > 0 && info->height > 0 &&
			info->stride > 0 && info->image_size > 0 &&
			info->payload_size >= info->image_size &&
			info->pixel_encoding[0] != '\0');
	spa_assert_se(andor3_camera_find_feature(camera, "andor3.ExposureTime",
			&exposure_index) == 0);
	spa_assert_se(andor3_camera_get_feature_info(camera, exposure_index,
			&feature_info) == 0 && feature_info.readable);
	spa_assert_se(andor3_camera_get_feature_value(camera, exposure_index,
			&feature_value) == 0 &&
			feature_value.kind == ANDOR3_FEATURE_FLOATING);
	if (feature_info.writable)
		spa_assert_se(andor3_camera_set_feature_value(camera, exposure_index,
				&feature_value) == 0);
	for (i = 0; i < 2; i++) {
		spa_assert_se(posix_memalign(&memory[i], 8,
				(size_t)info->payload_size) == 0);
		spa_assert_se(andor3_camera_announce(camera, memory[i],
				info->payload_size, &memory[i], &camera_buffers[i]) == 0);
		spa_assert_se(andor3_camera_queue(camera, camera_buffers[i]) == 0);
	}
	spa_assert_se(andor3_camera_start(camera) == 0);
	for (attempts = 0; attempts < 5000; attempts++) {
		int result = andor3_camera_try_get_completion(camera, &completion);
		spa_assert_se(result >= 0);
		if (result == 1)
			break;
		nanosleep(&delay, NULL);
	}
	spa_assert_se(attempts < 5000 && completion.buffer != NULL &&
			completion.user_data != NULL && completion.size_filled > 0 &&
			completion.size_filled <= info->payload_size);
	spa_assert_se(andor3_camera_stop(camera) == 0);
	for (i = 0; i < 2; i++) {
		spa_assert_se(andor3_camera_revoke(camera, &camera_buffers[i]) == 0);
		free(memory[i]);
	}
	andor3_camera_close(camera);
	return 0;
}

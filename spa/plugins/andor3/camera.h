/* SPDX-License-Identifier: MIT */
#ifndef SPA_ANDOR3_CAMERA_H
#define SPA_ANDOR3_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define ANDOR3_CAMERA_MAX_BUFFERS 64u

#ifdef __cplusplus
extern "C" {
#endif

enum andor3_feature_kind {
	ANDOR3_FEATURE_BOOLEAN,
	ANDOR3_FEATURE_INTEGER,
	ANDOR3_FEATURE_FLOATING,
	ANDOR3_FEATURE_ENUMERATION,
	ANDOR3_FEATURE_STRING,
	ANDOR3_FEATURE_COMMAND,
};

struct andor3_camera_options {
	uint32_t device_index;
};

struct andor3_camera_info {
	uint64_t payload_size;
	uint64_t image_size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	char pixel_encoding[64];
	char model[128];
	char serial[128];
};

struct andor3_feature_info {
	const char *name;
	const char *property_name;
	const char *description;
	enum andor3_feature_kind kind;
	uint32_t n_enum_entries;
	bool available;
	bool readable;
	bool writable;
	bool changes_layout;
};

struct andor3_feature_value {
	enum andor3_feature_kind kind;
	union {
		bool boolean;
		int64_t integer;
		double floating;
		int32_t enumeration;
		const char *string;
	};
};

struct andor3_camera_buffer;
struct andor3_camera;

struct andor3_camera_completion {
	struct andor3_camera_buffer *buffer;
	void *user_data;
	uint64_t frame_id;
	uint64_t size_filled;
	bool incomplete;
};

int andor3_camera_open(struct andor3_camera **camera,
		const struct andor3_camera_options *options);
void andor3_camera_close(struct andor3_camera *camera);
const struct andor3_camera_info *andor3_camera_get_info(
		const struct andor3_camera *camera);

uint32_t andor3_camera_get_feature_count(const struct andor3_camera *camera);
int andor3_camera_get_feature_info(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_info *info);
const char *andor3_camera_get_feature_enum_entry(
		const struct andor3_camera *camera, uint32_t index,
		uint32_t entry_index);
int andor3_camera_get_feature_value(struct andor3_camera *camera,
		uint32_t index, struct andor3_feature_value *value);
int andor3_camera_get_feature_integer_range(struct andor3_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum);
int andor3_camera_get_feature_float_range(struct andor3_camera *camera,
		uint32_t index, double *minimum, double *maximum);
int andor3_camera_find_feature(const struct andor3_camera *camera,
		const char *property_name, uint32_t *index);
int andor3_camera_set_feature_value(struct andor3_camera *camera,
		uint32_t index, const struct andor3_feature_value *value);
int andor3_camera_refresh_info(struct andor3_camera *camera);

int andor3_camera_announce(struct andor3_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct andor3_camera_buffer **buffer);
int andor3_camera_revoke(struct andor3_camera *camera,
		struct andor3_camera_buffer **buffer);
int andor3_camera_queue(struct andor3_camera *camera,
		struct andor3_camera_buffer *buffer);

int andor3_camera_start(struct andor3_camera *camera);
int andor3_camera_stop(struct andor3_camera *camera);
int andor3_camera_try_get_completion(struct andor3_camera *camera,
		struct andor3_camera_completion *completion);

#ifdef __cplusplus
}
#endif

#endif

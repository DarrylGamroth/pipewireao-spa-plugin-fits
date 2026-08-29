/* SPDX-License-Identifier: MIT */
#ifndef SPA_HAMAMATSU_CAMERA_H
#define SPA_HAMAMATSU_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#define HAMAMATSU_CAMERA_MAX_BUFFERS 64u

#ifdef __cplusplus
extern "C" {
#endif

enum hamamatsu_feature_kind {
	HAMAMATSU_FEATURE_BOOLEAN,
	HAMAMATSU_FEATURE_INTEGER,
	HAMAMATSU_FEATURE_FLOATING,
	HAMAMATSU_FEATURE_ENUMERATION,
	HAMAMATSU_FEATURE_STRING,
	HAMAMATSU_FEATURE_COMMAND,
};

enum hamamatsu_capture_mode {
	HAMAMATSU_CAPTURE_MODE_COPY,
	HAMAMATSU_CAPTURE_MODE_PHOENIX_ZERO_COPY,
};

struct hamamatsu_camera_options {
	uint32_t device_index;
	enum hamamatsu_capture_mode capture_mode;
};

struct hamamatsu_camera_info {
	uint64_t payload_size;
	uint64_t image_size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	char pixel_encoding[64];
	char model[128];
	char serial[128];
};

struct hamamatsu_feature_info {
	const char *name;
	const char *property_name;
	const char *description;
	enum hamamatsu_feature_kind kind;
	uint32_t n_enum_entries;
	bool available;
	bool readable;
	bool writable;
	bool changes_layout;
};

struct hamamatsu_feature_value {
	enum hamamatsu_feature_kind kind;
	union {
		bool boolean;
		int64_t integer;
		double floating;
		int32_t enumeration;
		const char *string;
	};
};

struct hamamatsu_camera_buffer;
struct hamamatsu_camera;

struct hamamatsu_camera_completion {
	struct hamamatsu_camera_buffer *buffer;
	void *user_data;
	uint64_t frame_id;
	uint64_t size_filled;
	bool incomplete;
};

int hamamatsu_camera_open(struct hamamatsu_camera **camera,
		const struct hamamatsu_camera_options *options);
void hamamatsu_camera_close(struct hamamatsu_camera *camera);
const struct hamamatsu_camera_info *hamamatsu_camera_get_info(
		const struct hamamatsu_camera *camera);

uint32_t hamamatsu_camera_get_feature_count(const struct hamamatsu_camera *camera);
int hamamatsu_camera_get_feature_info(struct hamamatsu_camera *camera,
		uint32_t index, struct hamamatsu_feature_info *info);
const char *hamamatsu_camera_get_feature_enum_entry(
		const struct hamamatsu_camera *camera, uint32_t index,
		uint32_t entry_index);
int hamamatsu_camera_get_feature_value(struct hamamatsu_camera *camera,
		uint32_t index, struct hamamatsu_feature_value *value);
int hamamatsu_camera_get_feature_integer_range(struct hamamatsu_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum);
int hamamatsu_camera_get_feature_float_range(struct hamamatsu_camera *camera,
		uint32_t index, double *minimum, double *maximum);
int hamamatsu_camera_find_feature(const struct hamamatsu_camera *camera,
		const char *property_name, uint32_t *index);
int hamamatsu_camera_set_feature_value(struct hamamatsu_camera *camera,
		uint32_t index, const struct hamamatsu_feature_value *value);
int hamamatsu_camera_refresh_info(struct hamamatsu_camera *camera);

int hamamatsu_camera_announce(struct hamamatsu_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct hamamatsu_camera_buffer **buffer);
int hamamatsu_camera_revoke(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_buffer **buffer);
int hamamatsu_camera_queue(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_buffer *buffer);

int hamamatsu_camera_start(struct hamamatsu_camera *camera);
int hamamatsu_camera_stop(struct hamamatsu_camera *camera);
int hamamatsu_camera_try_get_completion(struct hamamatsu_camera *camera,
		struct hamamatsu_camera_completion *completion);

#ifdef __cplusplus
}
#endif

#endif

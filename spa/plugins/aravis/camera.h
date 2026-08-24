/* SPDX-License-Identifier: MIT */
#ifndef SPA_ARAVIS_CAMERA_H
#define SPA_ARAVIS_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arv.h>

struct aravis_camera_options {
	const char *device_id;
};

struct aravis_camera_info {
	uint64_t payload_size;
	uint32_t width;
	uint32_t height;
	uint32_t offset_x;
	uint32_t offset_y;
	char pixel_format[64];
	char model[128];
	char serial[128];
};

struct aravis_frame_info {
	uint64_t frame_id;
	uint64_t camera_timestamp_ns;
	uint64_t size_filled;
	uint32_t width;
	uint32_t height;
	uint32_t offset_x;
	uint32_t offset_y;
	uint32_t x_padding;
	uint32_t y_padding;
	uint32_t image_offset;
	uint32_t image_size;
	bool incomplete;
};

struct aravis_camera_completion {
	ArvBuffer *buffer;
	void *user_data;
	struct aravis_frame_info frame;
	int result;
};

enum aravis_feature_kind {
	ARAVIS_FEATURE_BOOLEAN,
	ARAVIS_FEATURE_INTEGER,
	ARAVIS_FEATURE_FLOATING,
	ARAVIS_FEATURE_ENUMERATION,
	ARAVIS_FEATURE_STRING,
	ARAVIS_FEATURE_COMMAND,
};

struct aravis_feature_info {
	const char *name;
	const char *property_name;
	const char *description;
	enum aravis_feature_kind kind;
	uint32_t n_enum_entries;
	bool available;
	bool readable;
	bool writable;
	bool changes_layout;
};

struct aravis_feature_value {
	enum aravis_feature_kind kind;
	union {
		bool boolean;
		int64_t integer;
		double floating;
		int32_t enumeration;
		const char *string;
	};
};

struct aravis_camera;

/* Discovery, GenICam access, and teardown are stopped control-path work. */
int aravis_camera_open(struct aravis_camera **camera,
		const struct aravis_camera_options *options);
void aravis_camera_close(struct aravis_camera *camera);
const struct aravis_camera_info *aravis_camera_get_info(
		const struct aravis_camera *camera);

/* GenICam discovery and feature access are stopped control-path operations. */
uint32_t aravis_camera_get_feature_count(const struct aravis_camera *camera);
int aravis_camera_get_feature_info(struct aravis_camera *camera,
		uint32_t index, struct aravis_feature_info *info);
const char *aravis_camera_get_feature_enum_entry(
		const struct aravis_camera *camera, uint32_t index,
		uint32_t entry_index);
int aravis_camera_get_feature_value(struct aravis_camera *camera,
		uint32_t index, struct aravis_feature_value *value);
int aravis_camera_get_feature_integer_range(struct aravis_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum);
int aravis_camera_get_feature_float_range(struct aravis_camera *camera,
		uint32_t index, double *minimum, double *maximum);
int aravis_camera_find_feature(const struct aravis_camera *camera,
		const char *property_name, uint32_t *index);
int aravis_camera_set_feature_value(struct aravis_camera *camera,
		uint32_t index, const struct aravis_feature_value *value);
int aravis_camera_refresh_info(struct aravis_camera *camera);

/* Buffer announcement and revocation are pool-lifecycle operations. */
int aravis_camera_announce(struct aravis_camera *camera, void *memory,
		uint64_t size, void *user_data, ArvBuffer **buffer);
int aravis_camera_revoke(struct aravis_camera *camera, ArvBuffer **buffer);

int aravis_camera_start(struct aravis_camera *camera);
int aravis_camera_stop(struct aravis_camera *camera);

/* RTC-owner operations: one direct GenTL call is made by each poll or queue. */
int aravis_camera_queue(struct aravis_camera *camera, ArvBuffer *buffer);
int aravis_camera_try_get_completion(struct aravis_camera *camera,
		struct aravis_camera_completion *completion);

#endif

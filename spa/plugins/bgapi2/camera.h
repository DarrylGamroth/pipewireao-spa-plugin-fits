/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_CAMERA_H
#define SPA_BGAPI2_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bgapi2_genicam/bgapi2_genicam.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BGAPI2_CAMERA_ANY_INTERFACE UINT32_MAX

enum bgapi2_camera_completion_mode {
	BGAPI2_CAMERA_COMPLETION_CALLBACK,
	BGAPI2_CAMERA_COMPLETION_POLLING,
};

struct bgapi2_camera_options {
	const char *producer_path;
	uint32_t interface_index;
	uint32_t device_index;
	uint32_t stream_index;
	uint64_t interface_timeout_ms;
	uint64_t device_timeout_ms;
	enum bgapi2_camera_completion_mode completion_mode;
};

struct bgapi2_camera_info {
	uint32_t interface_index;
	uint32_t device_index;
	uint32_t stream_index;
	uint64_t payload_size;
	uint64_t width;
	uint64_t height;
	uint64_t offset_x;
	uint64_t offset_y;
	char pixel_format[64];
	char model[128];
	char serial[128];
};

struct bgapi2_frame_info {
	uint64_t frame_id;
	uint64_t size_filled;
	uint64_t width;
	uint64_t height;
	uint64_t offset_x;
	uint64_t offset_y;
	uint64_t x_padding;
	uint64_t image_offset;
	uint64_t image_length;
	bool incomplete;
};

struct bgapi2_camera_completion {
	BGAPI2_Buffer *buffer;
	void *user_data;
	struct bgapi2_frame_info frame;
	int result;
};

enum bgapi2_feature_kind {
	BGAPI2_FEATURE_BOOLEAN,
	BGAPI2_FEATURE_INTEGER,
	BGAPI2_FEATURE_FLOATING,
	BGAPI2_FEATURE_ENUMERATION,
	BGAPI2_FEATURE_STRING,
	BGAPI2_FEATURE_COMMAND,
};

struct bgapi2_feature_info {
	const char *name;
	const char *property_name;
	const char *description;
	enum bgapi2_feature_kind kind;
	uint32_t n_enum_entries;
	bool available;
	bool readable;
	bool writable;
	bool changes_layout;
};

struct bgapi2_feature_value {
	enum bgapi2_feature_kind kind;
	union {
		bool boolean;
		int64_t integer;
		double floating;
		int32_t enumeration;
		const char *string;
	};
};

struct bgapi2_camera;

/* Discovery, feature lookup, and teardown are control-path operations. */
int bgapi2_camera_open(struct bgapi2_camera **camera,
		const struct bgapi2_camera_options *options);
void bgapi2_camera_close(struct bgapi2_camera *camera);
const struct bgapi2_camera_info *bgapi2_camera_get_info(
		const struct bgapi2_camera *camera);

/* GenICam discovery and feature access are stopped control-path operations. */
uint32_t bgapi2_camera_get_feature_count(const struct bgapi2_camera *camera);
int bgapi2_camera_get_feature_info(struct bgapi2_camera *camera,
		uint32_t index, struct bgapi2_feature_info *info);
const char *bgapi2_camera_get_feature_enum_entry(
		const struct bgapi2_camera *camera, uint32_t index,
		uint32_t entry_index);
int bgapi2_camera_get_feature_value(struct bgapi2_camera *camera,
		uint32_t index, struct bgapi2_feature_value *value);
int bgapi2_camera_get_feature_integer_range(struct bgapi2_camera *camera,
		uint32_t index, int64_t *minimum, int64_t *maximum);
int bgapi2_camera_get_feature_float_range(struct bgapi2_camera *camera,
		uint32_t index, double *minimum, double *maximum);
int bgapi2_camera_find_feature(const struct bgapi2_camera *camera,
		const char *property_name, uint32_t *index);
int bgapi2_camera_set_feature_value(struct bgapi2_camera *camera,
		uint32_t index, const struct bgapi2_feature_value *value);
int bgapi2_camera_refresh_info(struct bgapi2_camera *camera);

/* Buffer creation and revocation are pool-lifecycle operations. */
int bgapi2_camera_announce(struct bgapi2_camera *camera, void *memory,
		uint64_t size, void *user_data, BGAPI2_Buffer **buffer);
int bgapi2_camera_revoke(struct bgapi2_camera *camera,
		BGAPI2_Buffer **buffer);
int bgapi2_camera_discard_buffers(struct bgapi2_camera *camera);

int bgapi2_camera_start(struct bgapi2_camera *camera);
int bgapi2_camera_stop(struct bgapi2_camera *camera);

/*
 * RTC owner operations. The source's callback profile consumes only the local
 * SPSC; the polling profile exists for adapter benchmarks and calls BGAPI2.
 * Queueing returns ownership to the GenTL producer, so the producer's queue
 * behavior remains part of the measured RTC contract.
 */
int bgapi2_camera_queue(struct bgapi2_camera *camera,
		BGAPI2_Buffer *buffer);
int bgapi2_camera_try_get_completion(struct bgapi2_camera *camera,
		struct bgapi2_camera_completion *completion);

#ifdef __cplusplus
}
#endif

#endif

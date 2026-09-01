/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PWAO_PYRTC_STREAM_H
#define PWAO_PYRTC_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/param/format.h>

#define PYRTC_METADATA_VALUES 10u
/* pyRTC readers require one zero shape terminator within the metadata array. */
#define PYRTC_MAX_DIMENSIONS (PYRTC_METADATA_VALUES - 5u)

struct pyrtc_format {
	enum spa_element_type element_type;
	uint32_t dtype_index;
	uint32_t rank;
	uint32_t shape[PYRTC_MAX_DIMENSIONS];
	uint32_t element_size;
	uint32_t stride;
	size_t bytes;
};

struct pyrtc_stream;

int pyrtc_format_from_element(enum spa_element_type element_type,
		uint32_t rank, const uint32_t shape[PYRTC_MAX_DIMENSIONS],
		struct pyrtc_format *format);

int pyrtc_stream_open(const char *name, bool source,
		struct pyrtc_stream **result, struct pyrtc_format *format);
int pyrtc_stream_create(const char *name, const struct pyrtc_format *format,
		struct pyrtc_stream **result);
void pyrtc_stream_close(struct pyrtc_stream *stream);

int pyrtc_stream_has_update(struct pyrtc_stream *stream, bool initial,
		bool *available);
int pyrtc_stream_read(struct pyrtc_stream *stream, void *destination,
		size_t size, uint64_t *sequence);
int pyrtc_stream_write(struct pyrtc_stream *stream, const void *source,
		size_t size);

#endif /* PWAO_PYRTC_STREAM_H */

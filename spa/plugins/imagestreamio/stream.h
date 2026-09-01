/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PWAO_IMAGESTREAMIO_STREAM_H
#define PWAO_IMAGESTREAMIO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/param/format.h>

struct isio_format {
	enum spa_element_type element_type;
	uint8_t datatype;
	uint32_t rank;
	uint32_t shape[2];
	uint32_t element_size;
	uint32_t stride;
	size_t bytes;
};

struct isio_stream;

int isio_format_from_element(enum spa_element_type element_type,
		uint32_t rank, const uint32_t shape[2], struct isio_format *format);

int isio_stream_open(const char *name, bool source,
		struct isio_stream **result, struct isio_format *format);
int isio_stream_create(const char *name, const struct isio_format *format,
		struct isio_stream **result);
void isio_stream_close(struct isio_stream *stream);

int isio_stream_claim_semaphore(struct isio_stream *stream, int preferred,
		int *claimed);
void isio_stream_release_semaphore(struct isio_stream *stream);
int isio_stream_flush(struct isio_stream *stream);

int isio_stream_has_update(struct isio_stream *stream, bool initial,
		bool *available);
int isio_stream_read(struct isio_stream *stream, void *destination,
		size_t size, uint64_t *sequence);
int isio_stream_write(struct isio_stream *stream, const void *source,
		size_t size);

#endif /* PWAO_IMAGESTREAMIO_STREAM_H */

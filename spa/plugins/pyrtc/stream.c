/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "stream.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "pyRTC ndarray transport currently requires little endian"
#endif

#define COPY_ATTEMPTS 4u
#define SHM_NAME_SIZE 256u
#define METADATA_BYTES (PYRTC_METADATA_VALUES * sizeof(double))

enum metadata_index {
	METADATA_COUNT = 0,
	METADATA_WRITE_TIME = 1,
	METADATA_SIZE = 2,
	METADATA_DTYPE = 3,
	METADATA_SHAPE = 4,
};

struct metadata_update {
	uint64_t count_bits;
	uint64_t write_time_bits;
};

struct pyrtc_stream {
	struct pyrtc_format format;
	char data_name[SHM_NAME_SIZE];
	char metadata_name[SHM_NAME_SIZE];
	void *data;
	double *metadata;
	uint64_t last_count;
	uint64_t last_write_time_bits;
	int data_fd;
	int metadata_fd;
	bool source;
	bool data_owner;
	bool metadata_owner;
};

/* Exact supported prefix of pyRTC.utils.NP_DATA_TYPES. */
static const enum spa_element_type pyrtc_element_types[] = {
	SPA_ELEMENT_TYPE_I8,
	SPA_ELEMENT_TYPE_I16_LE,
	SPA_ELEMENT_TYPE_I32_LE,
	SPA_ELEMENT_TYPE_I64_LE,
	SPA_ELEMENT_TYPE_U8,
	SPA_ELEMENT_TYPE_U16_LE,
	SPA_ELEMENT_TYPE_U32_LE,
	SPA_ELEMENT_TYPE_U64_LE,
	SPA_ELEMENT_TYPE_F16_LE,
	SPA_ELEMENT_TYPE_F32_LE,
	SPA_ELEMENT_TYPE_F64_LE,
	SPA_ELEMENT_TYPE_COMPLEX_F32_LE,
	SPA_ELEMENT_TYPE_COMPLEX_F64_LE,
	SPA_ELEMENT_TYPE_BOOL8,
};

static int dtype_from_element(enum spa_element_type element_type,
		uint32_t *dtype_index)
{
	uint32_t i;

	for (i = 0; i < SPA_N_ELEMENTS(pyrtc_element_types); i++) {
		if (pyrtc_element_types[i] == element_type) {
			*dtype_index = i;
			return 0;
		}
	}
	return -ENOTSUP;
}

static int element_from_dtype(uint32_t dtype_index,
		enum spa_element_type *element_type)
{
	if (dtype_index >= SPA_N_ELEMENTS(pyrtc_element_types))
		return -ENOTSUP;
	*element_type = pyrtc_element_types[dtype_index];
	return 0;
}

int pyrtc_format_from_element(enum spa_element_type element_type,
		uint32_t rank, const uint32_t shape[PYRTC_MAX_DIMENSIONS],
		struct pyrtc_format *format)
{
	uint32_t dtype_index, element_size, i;
	size_t elements = 1;
	int res;

	if (shape == NULL || format == NULL || rank == 0 ||
			rank > PYRTC_MAX_DIMENSIONS)
		return -EINVAL;
	if ((res = dtype_from_element(element_type, &dtype_index)) < 0)
		return res;
	element_size = spa_element_type_size(element_type);
	if (element_size == 0)
		return -ENOTSUP;
	for (i = 0; i < rank; i++) {
		if (shape[i] == 0 || shape[i] > INT32_MAX)
			return -EINVAL;
		if (elements > SIZE_MAX / shape[i])
			return -EOVERFLOW;
		elements *= shape[i];
	}
	if (elements > SIZE_MAX / element_size ||
			shape[rank - 1u] > UINT32_MAX / element_size)
		return -EOVERFLOW;
	memset(format, 0, sizeof(*format));
	format->element_type = element_type;
	format->dtype_index = dtype_index;
	format->rank = rank;
	memcpy(format->shape, shape, rank * sizeof(shape[0]));
	format->element_size = element_size;
	format->stride = shape[rank - 1u] * element_size;
	format->bytes = elements * element_size;
	return format->bytes > INT32_MAX ? -EOVERFLOW : 0;
}

static int make_names(const char *name, char data_name[SHM_NAME_SIZE],
		char metadata_name[SHM_NAME_SIZE])
{
	const char *base;
	size_t length;

	if (name == NULL || name[0] == '\0')
		return -EINVAL;
	base = name;
	if (strchr(base, '/') != NULL)
		return -EINVAL;
	length = strlen(base);
	if (length + sizeof("/_meta") > SHM_NAME_SIZE)
		return -ENAMETOOLONG;
	(void)snprintf(data_name, SHM_NAME_SIZE, "/%s", base);
	(void)snprintf(metadata_name, SHM_NAME_SIZE, "/%s_meta", base);
	return 0;
}

static void load_update(const double *metadata, struct metadata_update *update)
{
	memcpy(&update->count_bits, &metadata[METADATA_COUNT], sizeof(uint64_t));
	memcpy(&update->write_time_bits, &metadata[METADATA_WRITE_TIME],
			sizeof(uint64_t));
	atomic_thread_fence(memory_order_acquire);
}

static int count_from_bits(uint64_t bits, uint64_t *count)
{
	double value;

	memcpy(&value, &bits, sizeof(value));
	if (!isfinite(value) || value < 0.0 || value > 9007199254740992.0 ||
			floor(value) != value)
		return -EPROTO;
	*count = (uint64_t)value;
	return 0;
}

static int read_format(const double metadata[PYRTC_METADATA_VALUES],
		struct pyrtc_format *format)
{
	uint32_t dtype_index, rank = 0;
	uint32_t shape[PYRTC_MAX_DIMENSIONS] = { 0 };
	enum spa_element_type element_type;
	double value;
	int res;

	value = metadata[METADATA_DTYPE];
	if (!isfinite(value) || value < 0.0 || value > UINT32_MAX ||
			floor(value) != value)
		return -EPROTO;
	dtype_index = (uint32_t)value;
	if ((res = element_from_dtype(dtype_index, &element_type)) < 0)
		return res;
	while (rank < PYRTC_MAX_DIMENSIONS) {
		value = metadata[METADATA_SHAPE + rank];
		if (!isfinite(value) || value < 0.0 || value > INT32_MAX ||
			floor(value) != value)
			return -EPROTO;
		if (value == 0.0)
			break;
		shape[rank++] = (uint32_t)value;
	}
	if (rank == 0)
		return -EPROTO;
	if ((res = pyrtc_format_from_element(element_type, rank, shape, format)) < 0)
		return res;
	value = metadata[METADATA_SIZE];
	return isfinite(value) && value == (double)format->bytes ? 0 : -EPROTO;
}

static void unmap_stream(struct pyrtc_stream *stream)
{
	if (stream->metadata != NULL)
		(void)munmap(stream->metadata, METADATA_BYTES);
	if (stream->data != NULL)
		(void)munmap(stream->data, stream->format.bytes);
	if (stream->metadata_fd >= 0)
		(void)close(stream->metadata_fd);
	if (stream->data_fd >= 0)
		(void)close(stream->data_fd);
	stream->metadata = NULL;
	stream->data = NULL;
	stream->metadata_fd = -1;
	stream->data_fd = -1;
}

int pyrtc_stream_open(const char *name, bool source,
		struct pyrtc_stream **result, struct pyrtc_format *format)
{
	struct pyrtc_stream *stream;
	struct metadata_update update;
	struct stat status;
	double metadata[PYRTC_METADATA_VALUES];
	int protection, res;

	if (result == NULL || format == NULL)
		return -EINVAL;
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return -ENOMEM;
	stream->data_fd = -1;
	stream->metadata_fd = -1;
	stream->source = source;
	if ((res = make_names(name, stream->data_name,
			stream->metadata_name)) < 0)
		goto error;
	stream->metadata_fd = shm_open(stream->metadata_name,
			source ? O_RDONLY : O_RDWR, 0);
	if (stream->metadata_fd < 0) {
		res = -errno;
		goto error;
	}
	if (fstat(stream->metadata_fd, &status) < 0) {
		res = -errno;
		goto error;
	}
	if (status.st_size != (off_t)METADATA_BYTES) {
		res = -EPROTO;
		goto error;
	}
	protection = source ? PROT_READ : PROT_READ | PROT_WRITE;
	stream->metadata = mmap(NULL, METADATA_BYTES, protection, MAP_SHARED,
			stream->metadata_fd, 0);
	if (stream->metadata == MAP_FAILED) {
		stream->metadata = NULL;
		res = -errno;
		goto error;
	}
	memcpy(metadata, stream->metadata, sizeof(metadata));
	if ((res = read_format(metadata, &stream->format)) < 0)
		goto error;
	stream->data_fd = shm_open(stream->data_name,
			source ? O_RDONLY : O_RDWR, 0);
	if (stream->data_fd < 0) {
		res = -errno;
		goto error;
	}
	if (fstat(stream->data_fd, &status) < 0) {
		res = -errno;
		goto error;
	}
	if (status.st_size != (off_t)stream->format.bytes) {
		res = -EPROTO;
		goto error;
	}
	stream->data = mmap(NULL, stream->format.bytes, protection, MAP_SHARED,
			stream->data_fd, 0);
	if (stream->data == MAP_FAILED) {
		stream->data = NULL;
		res = -errno;
		goto error;
	}
	load_update(stream->metadata, &update);
	if ((res = count_from_bits(update.count_bits, &stream->last_count)) < 0)
		goto error;
	stream->last_write_time_bits = update.write_time_bits;
	*format = stream->format;
	*result = stream;
	return 0;

error:
	unmap_stream(stream);
	free(stream);
	return res;
}

int pyrtc_stream_create(const char *name, const struct pyrtc_format *format,
		struct pyrtc_stream **result)
{
	struct pyrtc_stream *stream;
	uint32_t i;
	int res;

	if (format == NULL || result == NULL || format->rank == 0 ||
			format->rank > PYRTC_MAX_DIMENSIONS || format->bytes == 0)
		return -EINVAL;
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return -ENOMEM;
	stream->data_fd = -1;
	stream->metadata_fd = -1;
	stream->format = *format;
	if ((res = make_names(name, stream->data_name,
			stream->metadata_name)) < 0)
		goto error;
	stream->data_fd = shm_open(stream->data_name, O_CREAT | O_EXCL | O_RDWR,
			0600);
	if (stream->data_fd < 0) {
		res = -errno;
		goto error;
	}
	stream->data_owner = true;
	if (ftruncate(stream->data_fd, (off_t)format->bytes) < 0) {
		res = -errno;
		goto error;
	}
	stream->metadata_fd = shm_open(stream->metadata_name,
			O_CREAT | O_EXCL | O_RDWR, 0600);
	if (stream->metadata_fd < 0) {
		res = -errno;
		goto error;
	}
	stream->metadata_owner = true;
	if (ftruncate(stream->metadata_fd, (off_t)METADATA_BYTES) < 0) {
		res = -errno;
		goto error;
	}
	stream->data = mmap(NULL, format->bytes, PROT_READ | PROT_WRITE,
			MAP_SHARED, stream->data_fd, 0);
	if (stream->data == MAP_FAILED) {
		stream->data = NULL;
		res = -errno;
		goto error;
	}
	stream->metadata = mmap(NULL, METADATA_BYTES, PROT_READ | PROT_WRITE,
			MAP_SHARED, stream->metadata_fd, 0);
	if (stream->metadata == MAP_FAILED) {
		stream->metadata = NULL;
		res = -errno;
		goto error;
	}
	memset(stream->data, 0, format->bytes);
	memset(stream->metadata, 0, METADATA_BYTES);
	stream->metadata[METADATA_SIZE] = (double)format->bytes;
	stream->metadata[METADATA_DTYPE] = (double)format->dtype_index;
	for (i = 0; i < format->rank; i++)
		stream->metadata[METADATA_SHAPE + i] = (double)format->shape[i];
	*result = stream;
	return 0;

error:
	unmap_stream(stream);
	if (stream->metadata_owner)
		(void)shm_unlink(stream->metadata_name);
	if (stream->data_owner)
		(void)shm_unlink(stream->data_name);
	free(stream);
	return res;
}

void pyrtc_stream_close(struct pyrtc_stream *stream)
{
	if (stream == NULL)
		return;
	unmap_stream(stream);
	if (stream->metadata_owner)
		(void)shm_unlink(stream->metadata_name);
	if (stream->data_owner)
		(void)shm_unlink(stream->data_name);
	free(stream);
}

int pyrtc_stream_has_update(struct pyrtc_stream *stream, bool initial,
		bool *available)
{
	struct metadata_update update;

	if (stream == NULL || !stream->source || available == NULL)
		return -EINVAL;
	load_update(stream->metadata, &update);
	*available = initial ||
			update.write_time_bits != stream->last_write_time_bits;
	return 0;
}

int pyrtc_stream_read(struct pyrtc_stream *stream, void *destination,
		size_t size, uint64_t *sequence)
{
	struct metadata_update before, after;
	uint64_t count;
	uint32_t attempt;
	int res;

	if (stream == NULL || !stream->source || destination == NULL ||
			sequence == NULL || size != stream->format.bytes)
		return -EINVAL;
	for (attempt = 0; attempt < COPY_ATTEMPTS; attempt++) {
		load_update(stream->metadata, &before);
		memcpy(destination, stream->data, size);
		atomic_thread_fence(memory_order_acquire);
		load_update(stream->metadata, &after);
		if (before.count_bits != after.count_bits ||
				before.write_time_bits != after.write_time_bits)
			continue;
		if ((res = count_from_bits(after.count_bits, &count)) < 0)
			return res;
		stream->last_count = count;
		stream->last_write_time_bits = after.write_time_bits;
		*sequence = count;
		return 0;
	}
	return -EAGAIN;
}

int pyrtc_stream_write(struct pyrtc_stream *stream, const void *source,
		size_t size)
{
	struct timespec now;
	double count, write_time;

	if (stream == NULL || stream->source || source == NULL ||
			size != stream->format.bytes)
		return -EINVAL;
	if (stream->last_count >= 9007199254740992u)
		return -EOVERFLOW;
	if (clock_gettime(CLOCK_REALTIME, &now) < 0)
		return -errno;
	memcpy(stream->data, source, size);
	atomic_thread_fence(memory_order_release);
	stream->last_count++;
	count = (double)stream->last_count;
	write_time = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
	memcpy(&stream->metadata[METADATA_COUNT], &count, sizeof(count));
	memcpy(&stream->metadata[METADATA_WRITE_TIME], &write_time,
			sizeof(write_time));
	memcpy(&stream->last_write_time_bits, &write_time, sizeof(write_time));
	return 0;
}

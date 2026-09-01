/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ImageStreamIO/ImageStreamIO.h>

#include "stream.h"

#define COPY_ATTEMPTS 4u
#define SEMAPHORE_DRAIN_LIMIT 64u

struct isio_stream {
	IMAGE image;
	struct isio_format format;
	uint64_t last_count;
	int semaphore;
	bool owner;
	bool open;
};

static int datatype_from_element(enum spa_element_type element_type,
		uint8_t *datatype)
{
	switch (element_type) {
	case SPA_ELEMENT_TYPE_U8:
		*datatype = _DATATYPE_UINT8;
		return 0;
	case SPA_ELEMENT_TYPE_I8:
		*datatype = _DATATYPE_INT8;
		return 0;
	case SPA_ELEMENT_TYPE_U16_LE:
		*datatype = _DATATYPE_UINT16;
		return 0;
	case SPA_ELEMENT_TYPE_I16_LE:
		*datatype = _DATATYPE_INT16;
		return 0;
	case SPA_ELEMENT_TYPE_U32_LE:
		*datatype = _DATATYPE_UINT32;
		return 0;
	case SPA_ELEMENT_TYPE_I32_LE:
		*datatype = _DATATYPE_INT32;
		return 0;
	case SPA_ELEMENT_TYPE_U64_LE:
		*datatype = _DATATYPE_UINT64;
		return 0;
	case SPA_ELEMENT_TYPE_I64_LE:
		*datatype = _DATATYPE_INT64;
		return 0;
	case SPA_ELEMENT_TYPE_F16_LE:
		*datatype = _DATATYPE_HALF;
		return 0;
	case SPA_ELEMENT_TYPE_F32_LE:
		*datatype = _DATATYPE_FLOAT;
		return 0;
	case SPA_ELEMENT_TYPE_F64_LE:
		*datatype = _DATATYPE_DOUBLE;
		return 0;
	case SPA_ELEMENT_TYPE_COMPLEX_F32_LE:
		*datatype = _DATATYPE_COMPLEX_FLOAT;
		return 0;
	case SPA_ELEMENT_TYPE_COMPLEX_F64_LE:
		*datatype = _DATATYPE_COMPLEX_DOUBLE;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int element_from_datatype(uint8_t datatype,
		enum spa_element_type *element_type)
{
	switch (datatype) {
	case _DATATYPE_UINT8:
		*element_type = SPA_ELEMENT_TYPE_U8;
		return 0;
	case _DATATYPE_INT8:
		*element_type = SPA_ELEMENT_TYPE_I8;
		return 0;
	case _DATATYPE_UINT16:
		*element_type = SPA_ELEMENT_TYPE_U16_LE;
		return 0;
	case _DATATYPE_INT16:
		*element_type = SPA_ELEMENT_TYPE_I16_LE;
		return 0;
	case _DATATYPE_UINT32:
		*element_type = SPA_ELEMENT_TYPE_U32_LE;
		return 0;
	case _DATATYPE_INT32:
		*element_type = SPA_ELEMENT_TYPE_I32_LE;
		return 0;
	case _DATATYPE_UINT64:
		*element_type = SPA_ELEMENT_TYPE_U64_LE;
		return 0;
	case _DATATYPE_INT64:
		*element_type = SPA_ELEMENT_TYPE_I64_LE;
		return 0;
	case _DATATYPE_HALF:
		*element_type = SPA_ELEMENT_TYPE_F16_LE;
		return 0;
	case _DATATYPE_FLOAT:
		*element_type = SPA_ELEMENT_TYPE_F32_LE;
		return 0;
	case _DATATYPE_DOUBLE:
		*element_type = SPA_ELEMENT_TYPE_F64_LE;
		return 0;
	case _DATATYPE_COMPLEX_FLOAT:
		*element_type = SPA_ELEMENT_TYPE_COMPLEX_F32_LE;
		return 0;
	case _DATATYPE_COMPLEX_DOUBLE:
		*element_type = SPA_ELEMENT_TYPE_COMPLEX_F64_LE;
		return 0;
	default:
		return -ENOTSUP;
	}
}

int isio_format_from_element(enum spa_element_type element_type,
		uint32_t rank, const uint32_t shape[2], struct isio_format *format)
{
	uint8_t datatype;
	uint32_t element_size;
	size_t elements;
	int res;

	if (shape == NULL || format == NULL || rank == 0 || rank > 2 ||
			shape[0] == 0 || shape[0] > INT32_MAX ||
			(rank == 2 && (shape[1] == 0 || shape[1] > INT32_MAX)))
		return -EINVAL;
	if ((res = datatype_from_element(element_type, &datatype)) < 0)
		return res;
	element_size = spa_element_type_size(element_type);
	if (element_size == 0)
		return -ENOTSUP;
	elements = shape[0];
	if (rank == 2) {
		if (shape[1] > SIZE_MAX / elements)
			return -EOVERFLOW;
		elements *= shape[1];
	}
	if (elements > SIZE_MAX / element_size ||
			shape[rank - 1u] > UINT32_MAX / element_size)
		return -EOVERFLOW;
	*format = (struct isio_format) {
		.element_type = element_type,
		.datatype = datatype,
		.rank = rank,
		.shape = { shape[0], rank == 2 ? shape[1] : 0 },
		.element_size = element_size,
		.stride = shape[rank - 1u] * element_size,
		.bytes = elements * element_size,
	};
	return format->bytes > INT32_MAX ? -EOVERFLOW : 0;
}

static int format_from_image(const IMAGE *image, bool source,
		struct isio_format *format)
{
	uint32_t shape[2];
	uint32_t rank;
	enum spa_element_type element_type;

	if (image == NULL || image->md == NULL || image->array.raw == NULL ||
			image->md->location != -1)
		return -ENOTSUP;
	if (image->md->naxis == 1) {
		rank = 1;
		shape[0] = image->md->size[0];
		shape[1] = 0;
	} else if (image->md->naxis == 2) {
		rank = 2;
		shape[0] = image->md->size[1];
		shape[1] = image->md->size[0];
	} else if (source && image->md->naxis == 3 &&
			((image->md->imagetype & CIRCULAR_BUFFER) != 0 ||
			 (image->md->imagetype & 0xf0000u) == ZAXIS_TEMPORAL)) {
		rank = 2;
		shape[0] = image->md->size[1];
		shape[1] = image->md->size[0];
	} else {
		return -ENOTSUP;
	}
	if (element_from_datatype(image->md->datatype, &element_type) < 0)
		return -ENOTSUP;
	return isio_format_from_element(element_type, rank, shape, format);
}

int isio_stream_open(const char *name, bool source,
		struct isio_stream **result, struct isio_format *format)
{
	struct isio_stream *stream;
	int res;

	if (name == NULL || name[0] == '\0' || result == NULL || format == NULL)
		return -EINVAL;
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return -ENOMEM;
	stream->semaphore = -1;
	if (ImageStreamIO_openIm(&stream->image, name) != IMAGESTREAMIO_SUCCESS) {
		free(stream);
		return -ENOENT;
	}
	stream->open = true;
	if ((res = format_from_image(&stream->image, source, &stream->format)) < 0) {
		isio_stream_close(stream);
		return res;
	}
	stream->last_count = __atomic_load_n(&stream->image.md->cnt0,
			__ATOMIC_ACQUIRE);
	*format = stream->format;
	*result = stream;
	return 0;
}

int isio_stream_create(const char *name, const struct isio_format *format,
		struct isio_stream **result)
{
	static _Atomic uint32_t temporary_sequence;
	struct isio_stream *stream;
	char filename[STRINGMAXLEN_FILE_NAME], temporary_filename[STRINGMAXLEN_FILE_NAME];
	char temporary_name[STRINGMAXLEN_IMAGE_NAME];
	uint32_t dimensions[3] = { 0 };
	uint32_t sequence;
	int naxis;

	if (name == NULL || name[0] == '\0' || format == NULL || result == NULL ||
			format->rank == 0 || format->rank > 2 ||
			strlen(name) >= STRINGMAXLEN_IMAGE_NAME)
		return -EINVAL;
	if (ImageStreamIO_filename(filename, sizeof(filename), name) !=
			IMAGESTREAMIO_SUCCESS)
		return -EINVAL;
	sequence = atomic_fetch_add_explicit(&temporary_sequence, 1,
			memory_order_relaxed);
	if (snprintf(temporary_name, sizeof(temporary_name), ".pwao-%ld-%u",
			(long)getpid(), sequence) >= (int)sizeof(temporary_name) ||
			ImageStreamIO_filename(temporary_filename, sizeof(temporary_filename),
				temporary_name) != IMAGESTREAMIO_SUCCESS)
		return -ENAMETOOLONG;
	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return -ENOMEM;
	stream->semaphore = -1;
	stream->format = *format;
	if (format->rank == 1) {
		naxis = 1;
		dimensions[0] = format->shape[0];
	} else {
		naxis = 2;
		dimensions[0] = format->shape[1];
		dimensions[1] = format->shape[0];
	}
	if (ImageStreamIO_createIm(&stream->image, temporary_name, naxis, dimensions,
			format->datatype, 1, 0, 0) != IMAGESTREAMIO_SUCCESS) {
		free(stream);
		return -EIO;
	}
	stream->owner = true;
	stream->open = true;
	if (renameat2(AT_FDCWD, temporary_filename, AT_FDCWD, filename,
			RENAME_NOREPLACE) < 0) {
		int error = errno == EEXIST ? -EEXIST : -errno;

		isio_stream_close(stream);
		return error;
	}
	memcpy(stream->image.name, name, strlen(name) + 1u);
	memcpy(stream->image.md->name, name, strlen(name) + 1u);
	*result = stream;
	return 0;
}

void isio_stream_close(struct isio_stream *stream)
{
	if (stream == NULL)
		return;
	if (stream->open) {
		isio_stream_release_semaphore(stream);
		if (stream->owner)
			(void)ImageStreamIO_destroyIm(&stream->image);
		else
			(void)ImageStreamIO_closeIm(&stream->image);
	}
	free(stream);
}

static bool process_alive(pid_t process)
{
	if (process <= 0)
		return false;
	if (process == getpid() || kill(process, 0) == 0)
		return true;
	return errno == EPERM;
}

int isio_stream_claim_semaphore(struct isio_stream *stream, int preferred,
		int *claimed)
{
	uint32_t begin, count, offset;
	pid_t process = getpid();

	if (stream == NULL || !stream->open || stream->image.md == NULL ||
			claimed == NULL || stream->semaphore >= 0)
		return -EINVAL;
	count = stream->image.md->sem;
	if (count == 0)
		return -ENOSPC;
	if (preferred >= 0 && (uint32_t)preferred >= count)
		return -ERANGE;
	begin = preferred >= 0 ? (uint32_t)preferred : 0;
	for (offset = 0; offset < count; offset++) {
		uint32_t index = (begin + offset) % count;
		pid_t owner;

		if (preferred >= 0 && offset > 0)
			break;
		owner = __atomic_load_n(&stream->image.semReadPID[index],
				__ATOMIC_ACQUIRE);
		if (process_alive(owner))
			continue;
		if (!__atomic_compare_exchange_n(&stream->image.semReadPID[index],
				&owner, process, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			continue;
		stream->semaphore = (int)index;
		*claimed = (int)index;
		return 0;
	}
	return -EBUSY;
}

void isio_stream_release_semaphore(struct isio_stream *stream)
{
	pid_t process;

	if (stream == NULL || stream->semaphore < 0 || stream->image.md == NULL)
		return;
	process = __atomic_load_n(
			&stream->image.semReadPID[stream->semaphore], __ATOMIC_ACQUIRE);
	if (process == getpid())
		__atomic_store_n(&stream->image.semReadPID[stream->semaphore], 0,
				__ATOMIC_RELEASE);
	stream->semaphore = -1;
}

int isio_stream_flush(struct isio_stream *stream)
{
	if (stream == NULL || stream->semaphore < 0)
		return -EINVAL;
	return ImageStreamIO_semflush(&stream->image, stream->semaphore) ==
			IMAGESTREAMIO_SUCCESS ? 0 : -EIO;
}

int isio_stream_has_update(struct isio_stream *stream, bool initial,
		bool *available)
{
	uint64_t count;
	uint32_t attempts;
	int wait_result;

	if (stream == NULL || stream->semaphore < 0 || available == NULL)
		return -EINVAL;
	for (attempts = 0; attempts < SEMAPHORE_DRAIN_LIMIT; attempts++) {
		wait_result = ImageStreamIO_semtrywait(&stream->image,
				stream->semaphore);
		if (wait_result == 0 || errno == EINTR)
			continue;
		if (errno != EAGAIN)
			return -errno;
		break;
	}
	count = __atomic_load_n(&stream->image.md->cnt0, __ATOMIC_ACQUIRE);
	*available = initial || count != stream->last_count;
	return 0;
}

int isio_stream_read(struct isio_stream *stream, void *destination,
		size_t size, uint64_t *sequence)
{
	uint32_t attempt;

	if (stream == NULL || destination == NULL || sequence == NULL ||
			size != stream->format.bytes)
		return -EINVAL;
	for (attempt = 0; attempt < COPY_ATTEMPTS; attempt++) {
		void *source = stream->image.array.raw;
		uint64_t before, after, slice_before = 0, slice_after = 0;

		if (__atomic_load_n(&stream->image.md->write, __ATOMIC_ACQUIRE) != 0)
			continue;
		before = __atomic_load_n(&stream->image.md->cnt0, __ATOMIC_ACQUIRE);
		if (stream->image.md->naxis == 3) {
			slice_before = __atomic_load_n(&stream->image.md->cnt1,
					__ATOMIC_ACQUIRE);
			if (slice_before >= stream->image.md->size[2] ||
					ImageStreamIO_readBufferAt(&stream->image,
							(unsigned int)slice_before, &source) !=
							IMAGESTREAMIO_SUCCESS)
				return -EIO;
		}
		memcpy(destination, source, size);
		atomic_signal_fence(memory_order_seq_cst);
		after = __atomic_load_n(&stream->image.md->cnt0, __ATOMIC_ACQUIRE);
		if (stream->image.md->naxis == 3)
			slice_after = __atomic_load_n(&stream->image.md->cnt1,
					__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&stream->image.md->write, __ATOMIC_ACQUIRE) == 0 &&
				before == after && slice_before == slice_after) {
			stream->last_count = after;
			*sequence = after;
			return 0;
		}
	}
	return -EAGAIN;
}

int isio_stream_write(struct isio_stream *stream, const void *source,
		size_t size)
{
	if (stream == NULL || source == NULL || size != stream->format.bytes ||
			stream->image.md == NULL || stream->image.array.raw == NULL)
		return -EINVAL;
	if (__atomic_exchange_n(&stream->image.md->write, 1, __ATOMIC_ACQ_REL) != 0)
		return -EAGAIN;
	memcpy(stream->image.array.raw, source, size);
	if (ImageStreamIO_UpdateIm(&stream->image) != IMAGESTREAMIO_SUCCESS) {
		__atomic_store_n(&stream->image.md->write, 0, __ATOMIC_RELEASE);
		return -EIO;
	}
	return 0;
}

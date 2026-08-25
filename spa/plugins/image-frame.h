/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_IMAGE_FRAME_H
#define PIPEWIREAO_IMAGE_FRAME_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

enum pwao_image_frame_requirement {
	PWAO_IMAGE_FRAME_REQUIRE_HEADER = (1u << 0),
	PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION = (1u << 1),
};

struct pwao_image_frame {
	uint32_t data_index;
	uint32_t header_flags;
	uint32_t chunk_flags;
	uint32_t offset;
	uint32_t size;
	int32_t stride;
	uint32_t header_offset;
	uint64_t sequence;
	int64_t pts;
	const struct spa_meta_acquisition *acquisition;
};

static inline bool pwao_image_frame_acquisition_valid(
		const struct spa_meta_acquisition *acquisition)
{
	struct spa_meta meta = {
		.type = SPA_META_Acquisition,
		.size = sizeof(*acquisition),
		.data = (void *) acquisition,
	};

	return acquisition != NULL && spa_meta_acquisition_is_valid(&meta);
}

static inline int pwao_image_frame_write(struct spa_buffer *buffer,
		const struct pwao_image_frame *frame, uint32_t requirements)
{
	struct spa_meta_acquisition *acquisition;
	struct spa_meta_header *header;
	struct spa_data *data;
	uint64_t sequence, end;
	int64_t pts;
	const uint32_t valid_header_flags = SPA_META_HEADER_FLAG_DISCONT |
		SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_MARKER |
		SPA_META_HEADER_FLAG_HEADER | SPA_META_HEADER_FLAG_GAP |
		SPA_META_HEADER_FLAG_DELTA_UNIT;
	const uint32_t valid_chunk_flags = SPA_CHUNK_FLAG_CORRUPTED |
		SPA_CHUNK_FLAG_EMPTY;
	const uint32_t valid_requirements = PWAO_IMAGE_FRAME_REQUIRE_HEADER |
		PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION;

	if (buffer == NULL || frame == NULL ||
			(requirements & ~valid_requirements) != 0 ||
			(frame->header_flags & ~valid_header_flags) != 0 ||
			(frame->chunk_flags & ~valid_chunk_flags) != 0 ||
			(frame->pts < 0 && frame->pts != SPA_TIME_INVALID) ||
			frame->data_index >= buffer->n_datas)
		return -EINVAL;
	data = &buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + frame->size;
	if (data->chunk == NULL || end > data->maxsize)
		return -ENOSPC;
	if (frame->acquisition != NULL &&
			!pwao_image_frame_acquisition_valid(frame->acquisition))
		return -EINVAL;

	header = (struct spa_meta_header *) spa_buffer_find_meta_data(buffer,
			SPA_META_Header, sizeof(*header));
	if (header == NULL &&
			(requirements & PWAO_IMAGE_FRAME_REQUIRE_HEADER) != 0)
		return -ENOTSUP;
	acquisition = (struct spa_meta_acquisition *) spa_buffer_find_meta_data(
			buffer, SPA_META_Acquisition, sizeof(*acquisition));
	if (acquisition == NULL &&
			((requirements & PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION) != 0 ||
			 frame->acquisition != NULL))
		return -ENOTSUP;
	if (acquisition != NULL && !SPA_IS_ALIGNED(acquisition, 8))
		return -EINVAL;

	data->chunk->offset = frame->offset;
	data->chunk->size = frame->size;
	data->chunk->stride = frame->stride;
	data->chunk->flags = frame->chunk_flags;

	if (acquisition != NULL) {
		if (frame->acquisition == NULL) {
			if (!spa_meta_acquisition_init(acquisition))
				return -EINVAL;
		} else if (acquisition != frame->acquisition) {
			memcpy(acquisition, frame->acquisition, sizeof(*acquisition));
		}
	}

	sequence = frame->sequence;
	pts = frame->pts;
	if (frame->acquisition != NULL) {
		if (SPA_FLAG_IS_SET(frame->acquisition->flags,
				SPA_META_ACQUISITION_FLAG_IDENTITY_VALID))
			sequence = frame->acquisition->sequence;
		if (SPA_FLAG_IS_SET(frame->acquisition->flags,
				SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID))
			pts = frame->acquisition->exposure_start_nsec;
	}
	if (header != NULL) {
		header->flags = frame->header_flags;
		header->offset = frame->header_offset;
		header->seq = sequence;
		header->pts = pts;
		header->dts_offset = 0;
	}
	return 0;
}

#endif /* PIPEWIREAO_IMAGE_FRAME_H */

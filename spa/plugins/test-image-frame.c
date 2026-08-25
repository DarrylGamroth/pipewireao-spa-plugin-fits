/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>

#include "image-frame.h"

#define PAYLOAD_SIZE 64u

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	struct spa_meta_acquisition acquisition;
	uint8_t payload[PAYLOAD_SIZE];
};

static void init_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.data = storage->payload;
	storage->data.maxsize = sizeof(storage->payload);
	storage->data.chunk = &storage->chunk;
	storage->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	storage->metas[1] = (struct spa_meta) {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage->acquisition),
		.data = &storage->acquisition,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void test_write(void)
{
	struct test_buffer storage;
	struct spa_meta_acquisition acquisition;
	uint8_t domain[SPA_META_ACQUISITION_DOMAIN_SIZE] = { 1 };
	struct pwao_image_frame frame = {
		.data_index = 0,
		.header_flags = SPA_META_HEADER_FLAG_MARKER,
		.chunk_flags = SPA_CHUNK_FLAG_CORRUPTED,
		.offset = 4,
		.size = 32,
		.stride = 8,
		.header_offset = 12,
		.sequence = 3,
		.pts = 17,
	};

	init_buffer(&storage);
	spa_assert_se(spa_meta_acquisition_init(&acquisition));
	spa_assert_se(spa_meta_acquisition_set_identity(&acquisition, domain, 4, 9));
	spa_assert_se(spa_meta_acquisition_set_exposure_start(&acquisition, 101, 0));
	frame.acquisition = &acquisition;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER |
			PWAO_IMAGE_FRAME_REQUIRE_ACQUISITION) == 0);
	spa_assert_se(storage.chunk.offset == frame.offset);
	spa_assert_se(storage.chunk.size == frame.size);
	spa_assert_se(storage.chunk.stride == frame.stride);
	spa_assert_se((uint32_t)storage.chunk.flags == frame.chunk_flags);
	spa_assert_se(storage.header.flags == frame.header_flags);
	spa_assert_se(storage.header.offset == frame.header_offset);
	spa_assert_se(storage.header.seq == acquisition.sequence);
	spa_assert_se(storage.header.pts == acquisition.exposure_start_nsec);
	spa_assert_se(memcmp(&storage.acquisition, &acquisition,
			sizeof(acquisition)) == 0);
}

static void test_validation(void)
{
	struct test_buffer storage;
	struct pwao_image_frame frame = {
		.data_index = 0,
		.size = PAYLOAD_SIZE,
		.pts = SPA_TIME_INVALID,
	};

	init_buffer(&storage);
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER) == 0);
	frame.size++;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -ENOSPC);
	frame.size--;
	frame.header_flags = UINT32_MAX;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	frame.header_flags = 0;
	storage.buffer.n_metas = 0;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER) == -ENOTSUP);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	test_write();
	test_validation();
	return 0;
}

/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <ImageStreamIO/ImageStreamIO.h>

#include <spa/support/plugin.h>

#include "stream.h"

int main(void)
{
	const uint32_t shape[] = { 3, 4 };
	const uint16_t expected[] = {
		1, 2, 3, 4,
		5, 6, 7, 8,
		9, 10, 11, 12,
	};
	uint16_t observed[SPA_N_ELEMENTS(expected)] = { 0 };
	struct isio_format format, opened;
	struct isio_stream *writer = NULL, *reader = NULL;
	IMAGE temporal = { 0 };
	char directory[] = "/tmp/pwao-isio-XXXXXX";
	uint32_t temporal_dimensions[] = { 4, 3, 2 };
	void *temporal_write = NULL;
	uint64_t sequence = 0;
	bool available = false;
	int semaphore;

	spa_assert_se(mkdtemp(directory) != NULL);
	spa_assert_se(setenv("MILK_SHM_DIR", directory, 1) == 0);
	spa_assert_se(isio_format_from_element(SPA_ELEMENT_TYPE_U16_LE, 2,
			shape, &format) == 0);
	spa_assert_se(format.bytes == sizeof(expected));
	spa_assert_se(format.stride == 4u * sizeof(uint16_t));
	spa_assert_se(isio_stream_create("roundtrip", &format, &writer) == 0);
	spa_assert_se(isio_stream_create("roundtrip", &format, &reader) ==
			-EEXIST);
	spa_assert_se(isio_stream_open("roundtrip", true, &reader, &opened) == 0);
	spa_assert_se(opened.rank == 2 && opened.shape[0] == 3 &&
			opened.shape[1] == 4 &&
			opened.element_type == SPA_ELEMENT_TYPE_U16_LE);
	spa_assert_se(isio_stream_claim_semaphore(reader, -1, &semaphore) == 0);
	spa_assert_se(semaphore >= 0);
	spa_assert_se(isio_stream_flush(reader) == 0);
	spa_assert_se(isio_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(!available);
	spa_assert_se(isio_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(isio_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(available);
	spa_assert_se(isio_stream_read(reader, observed, sizeof(observed),
			&sequence) == 0);
	spa_assert_se(sequence == 1);
	spa_assert_se(memcmp(observed, expected, sizeof(expected)) == 0);
	spa_assert_se(isio_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(!available);
	spa_assert_se(isio_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(isio_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(isio_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(available);
	spa_assert_se(isio_stream_read(reader, observed, sizeof(observed),
			&sequence) == 0);
	spa_assert_se(sequence == 3);
	spa_assert_se(isio_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(!available);
	isio_stream_close(reader);
	isio_stream_close(writer);

	spa_assert_se(ImageStreamIO_createIm_gpu(&temporal, "temporal", 3,
			temporal_dimensions, _DATATYPE_UINT16, -1, 1, 2, 0,
			CIRCULAR_BUFFER | ZAXIS_TEMPORAL, 0) == IMAGESTREAMIO_SUCCESS);
	spa_assert_se(ImageStreamIO_readBufferAt(&temporal, 1, &temporal_write) ==
			IMAGESTREAMIO_SUCCESS);
	memcpy(temporal_write, expected, sizeof(expected));
	temporal.md->cnt1 = 1;
	spa_assert_se(ImageStreamIO_UpdateIm(&temporal) == IMAGESTREAMIO_SUCCESS);
	reader = NULL;
	spa_assert_se(isio_stream_open("temporal", true, &reader, &opened) == 0);
	spa_assert_se(opened.rank == 2 && opened.shape[0] == 3 &&
			opened.shape[1] == 4 && opened.bytes == sizeof(expected));
	spa_assert_se(isio_stream_read(reader, observed, sizeof(observed),
			&sequence) == 0);
	spa_assert_se(sequence == 1 &&
			memcmp(observed, expected, sizeof(expected)) == 0);
	isio_stream_close(reader);
	spa_assert_se(ImageStreamIO_destroyIm(&temporal) == IMAGESTREAMIO_SUCCESS);
	spa_assert_se(rmdir(directory) == 0);
	return 0;
}

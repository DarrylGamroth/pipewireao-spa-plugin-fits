/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>
#include <spa/utils/defs.h>

#include "cube.h"

static void make_cube(char *path, size_t path_size)
{
	char temporary[] = "/tmp/pipewireao-fits-cube-XXXXXX";
	uint16_t pixels[3][3][4];
	LONGLONG axes[] = { 4, 3, 3 };
	fitsfile *file = NULL;
	int descriptor, status = 0;
	uint32_t frame, row, column;

	descriptor = mkstemp(temporary);
	spa_assert_se(descriptor >= 0);
	spa_assert_se(close(descriptor) == 0);
	spa_assert_se(unlink(temporary) == 0);
	spa_assert_se(snprintf(path, path_size, "!%s", temporary) > 0);
	fits_create_file(&file, path, &status);
	spa_assert_se(status == 0);
	fits_create_imgll(file, USHORT_IMG, SPA_N_ELEMENTS(axes), axes, &status);
	spa_assert_se(status == 0);
	for (frame = 0; frame < 3; frame++)
		for (row = 0; row < 3; row++)
			for (column = 0; column < 4; column++)
				pixels[frame][row][column] =
						(uint16_t)(frame * 100 + row * 10 + column);
	fits_write_img(file, TUSHORT, 1,
			sizeof(pixels) / sizeof(pixels[0][0][0]), pixels, &status);
	spa_assert_se(status == 0);
	fits_close_file(file, &status);
	spa_assert_se(status == 0);
	memmove(path, path + 1, strlen(path));
}

static void test_mode(const char *path, enum fits_cube_io_mode mode,
		bool prefault)
{
	struct fits_cube_options options = {
		.path = path,
		.hdu = 1,
		.frame_rank = 2,
		.io_mode = mode,
		.prefault = prefault,
	};
	struct fits_cube *cube = NULL;
	const struct fits_cube_info *info;
	uint16_t output[3][4];
	char message[256];
	uint32_t row, column;

	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == 0);
	info = fits_cube_get_info(cube);
	spa_assert_se(info != NULL);
	spa_assert_se(info->frame_rank == 2);
	spa_assert_se(info->shape[0] == 4 && info->shape[1] == 3);
	spa_assert_se(info->width == 4);
	spa_assert_se(info->height == 3);
	spa_assert_se(info->frames == 3);
	spa_assert_se(info->element_type == SPA_ELEMENT_TYPE_U16_LE);
	spa_assert_se(info->element_size == sizeof(uint16_t));
	spa_assert_se(info->plane_elements ==
			sizeof(output) / sizeof(output[0][0]));
	spa_assert_se(info->plane_size == sizeof(output));
	spa_assert_se(fits_cube_read_plane(cube, 2, FITS_CUBE_OUTPUT_NATIVE,
			output, sizeof(output)) == 0);
	for (row = 0; row < 3; row++)
		for (column = 0; column < 4; column++)
			spa_assert_se(output[row][column] == 200 + row * 10 + column);
	spa_assert_se(fits_cube_read_plane(cube, 1, FITS_CUBE_OUTPUT_GRAY16,
			output, sizeof(output)) == 0);
	for (row = 0; row < 3; row++)
		for (column = 0; column < 4; column++)
			spa_assert_se(output[row][column] == 100 + row * 10 + column);
	spa_assert_se(fits_cube_read_plane(cube, 3, FITS_CUBE_OUTPUT_NATIVE,
			output, sizeof(output)) == -EINVAL);
	spa_assert_se(fits_cube_read_plane(cube, 0, FITS_CUBE_OUTPUT_NATIVE,
			output, sizeof(output) - 1) == -ENOSPC);
	fits_cube_close(cube);
}

int main(void)
{
	char path[256];

	make_cube(path, sizeof(path));
	test_mode(path, FITS_CUBE_IO_FILE, false);
	test_mode(path, FITS_CUBE_IO_MMAP, false);
	test_mode(path, FITS_CUBE_IO_MMAP, true);
	spa_assert_se(unlink(path) == 0);
	return 0;
}

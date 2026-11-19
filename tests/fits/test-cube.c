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
	fits_create_tbl(file, BINARY_TBL, 0, 0, NULL, NULL, NULL, "metadata", &status);
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
		.sample_rank = 2,
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
	spa_assert_se(info->sample_rank == 2);
	spa_assert_se(info->shape[0] == 4 && info->shape[1] == 3);
	spa_assert_se(info->width == 4);
	spa_assert_se(info->height == 3);
	spa_assert_se(info->samples == 3);
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
	spa_assert_se(fits_cube_read_plane(cube, 0, (enum fits_cube_output)-1,
			output, sizeof(output)) == -EINVAL);
	fits_cube_close(cube);
}

static void test_invalid_options(const char *path)
{
	struct fits_cube_options options = {
		.path = path,
		.hdu = 1,
		.sample_rank = 1,
		.io_mode = FITS_CUBE_IO_FILE,
	};
	struct fits_cube *cube = NULL;
	char message[256];

	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -EINVAL);
	spa_assert_se(cube == NULL && strstr(message, "sample rank") != NULL);
	options.sample_rank = 2;
	options.hdu = 2;
	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -EINVAL);
	spa_assert_se(cube == NULL && strstr(message, "not an image") != NULL);
	options.hdu = 3;
	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -EBADMSG);
	spa_assert_se(cube == NULL && strstr(message, "select FITS HDU") != NULL);
	options.hdu = 0;
	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -EINVAL);
	spa_assert_se(cube == NULL && strstr(message, "invalid FITS cube options") != NULL);
	options.hdu = 1;
	options.path = "";
	spa_assert_se(fits_cube_open(&cube, &options, NULL, 0) == -EINVAL);
	spa_assert_se(fits_cube_get_info(cube) == NULL);
	fits_cube_close(cube);
}

static void test_missing_and_empty_file(void)
{
	char path[] = "/tmp/pipewireao-fits-empty-XXXXXX";
	struct fits_cube_options options = {
		.path = path,
		.hdu = 1,
		.sample_rank = 2,
		.io_mode = FITS_CUBE_IO_MMAP,
	};
	struct fits_cube *cube = NULL;
	char message[256];
	int fd = mkstemp(path);

	spa_assert_se(fd >= 0);
	spa_assert_se(close(fd) == 0);
	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -EFBIG);
	spa_assert_se(cube == NULL && strstr(message, "unsupported size") != NULL);
	spa_assert_se(unlink(path) == 0);
	spa_assert_se(fits_cube_open(&cube, &options, message, sizeof(message)) == -ENOENT);
	spa_assert_se(cube == NULL && strstr(message, "could not open") != NULL);
}

static void test_image_types(void)
{
	static const struct {
		int bitpix;
		enum spa_element_type type;
		size_t size;
	} types[] = {
		{ BYTE_IMG, SPA_ELEMENT_TYPE_U8, 1 },
		{ SBYTE_IMG, SPA_ELEMENT_TYPE_I8, 1 },
		{ SHORT_IMG, SPA_ELEMENT_TYPE_I16_LE, 2 },
		{ LONG_IMG, SPA_ELEMENT_TYPE_I32_LE, 4 },
		{ ULONG_IMG, SPA_ELEMENT_TYPE_U32_LE, 4 },
		{ LONGLONG_IMG, SPA_ELEMENT_TYPE_I64_LE, 8 },
		{ FLOAT_IMG, SPA_ELEMENT_TYPE_F32_LE, 4 },
		{ DOUBLE_IMG, SPA_ELEMENT_TYPE_F64_LE, 8 },
	};
	char path[] = "/tmp/pipewireao-fits-types-XXXXXX";
	double pixels[] = { 1.0, 2.0, 3.0 };
	LONGLONG axis = SPA_N_ELEMENTS(pixels);
	struct fits_cube_options options = {
		.path = path,
		.sample_rank = 1,
		.io_mode = FITS_CUBE_IO_FILE,
	};
	fitsfile *file = NULL;
	int status = 0, fd = mkstemp(path);

	spa_assert_se(fd >= 0);
	spa_assert_se(close(fd) == 0 && unlink(path) == 0);
	fits_create_file(&file, path, &status);
	for (size_t i = 0; i < SPA_N_ELEMENTS(types); i++) {
		fits_create_imgll(file, types[i].bitpix, 1, &axis, &status);
		fits_write_img(file, TDOUBLE, 1, axis, pixels, &status);
		spa_assert_se(status == 0);
	}
	fits_close_file(file, &status);
	spa_assert_se(status == 0);

	for (size_t i = 0; i < SPA_N_ELEMENTS(types); i++) {
		struct fits_cube *cube = NULL;
		const struct fits_cube_info *info;
		uint16_t output[SPA_N_ELEMENTS(pixels)];

		options.hdu = i + 1;
		spa_assert_se(fits_cube_open(&cube, &options, NULL, 0) == 0);
		info = fits_cube_get_info(cube);
		spa_assert_se(info->samples == 1 && info->sample_rank == 1);
		spa_assert_se(info->plane_size == types[i].size * SPA_N_ELEMENTS(pixels));
		spa_assert_se(info->element_type == types[i].type);
		spa_assert_se(fits_cube_read_plane(cube, 0, FITS_CUBE_OUTPUT_GRAY16,
				output, sizeof(output)) == 0);
		for (size_t j = 0; j < SPA_N_ELEMENTS(output); j++)
			spa_assert_se(output[j] == (uint16_t)pixels[j]);
		fits_cube_close(cube);
	}
	spa_assert_se(unlink(path) == 0);
}

int main(void)
{
	char path[256];

	make_cube(path, sizeof(path));
	test_mode(path, FITS_CUBE_IO_FILE, false);
	test_mode(path, FITS_CUBE_IO_MMAP, false);
	test_mode(path, FITS_CUBE_IO_MMAP, true);
	test_invalid_options(path);
	test_missing_and_empty_file();
	test_image_types();
	spa_assert_se(unlink(path) == 0);
	return 0;
}

/* SPDX-License-Identifier: MIT */
#include "cube.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fitsio.h>

struct fits_cube {
	fitsfile *file;
	void *mapping;
	size_t mapping_size;
	int fd;
	int native_datatype;
	struct fits_cube_info info;
};

static int fail(char *message, size_t size, int error,
		const char *format, ...)
{
	va_list args;

	if (message != NULL && size != 0) {
		va_start(args, format);
		vsnprintf(message, size, format, args);
		va_end(args);
	}
	return error;
}

static int fits_fail(char *message, size_t size, int status,
		const char *operation)
{
	char detail[FLEN_STATUS] = { 0 };

	fits_get_errstatus(status, detail);
	return fail(message, size, -EBADMSG, "%s: %s", operation, detail);
}

static int image_type(int type, enum spa_element_type *element_type,
		int *datatype, size_t *element_size)
{
	switch (type) {
	case BYTE_IMG:
		*element_type = SPA_ELEMENT_TYPE_U8;
		*datatype = TBYTE;
		*element_size = sizeof(uint8_t);
		return 0;
	case SBYTE_IMG:
		*element_type = SPA_ELEMENT_TYPE_I8;
		*datatype = TSBYTE;
		*element_size = sizeof(int8_t);
		return 0;
	case SHORT_IMG:
		*element_type = SPA_ELEMENT_TYPE_I16_LE;
		*datatype = TSHORT;
		*element_size = sizeof(int16_t);
		return 0;
	case USHORT_IMG:
		*element_type = SPA_ELEMENT_TYPE_U16_LE;
		*datatype = TUSHORT;
		*element_size = sizeof(uint16_t);
		return 0;
	case LONG_IMG:
		*element_type = SPA_ELEMENT_TYPE_I32_LE;
		*datatype = TINT;
		*element_size = sizeof(int32_t);
		return 0;
	case ULONG_IMG:
		*element_type = SPA_ELEMENT_TYPE_U32_LE;
		*datatype = TUINT;
		*element_size = sizeof(uint32_t);
		return 0;
	case LONGLONG_IMG:
		*element_type = SPA_ELEMENT_TYPE_I64_LE;
		*datatype = TLONGLONG;
		*element_size = sizeof(int64_t);
		return 0;
	case ULONGLONG_IMG:
		*element_type = SPA_ELEMENT_TYPE_U64_LE;
		*datatype = TULONGLONG;
		*element_size = sizeof(uint64_t);
		return 0;
	case FLOAT_IMG:
		*element_type = SPA_ELEMENT_TYPE_F32_LE;
		*datatype = TFLOAT;
		*element_size = sizeof(float);
		return 0;
	case DOUBLE_IMG:
		*element_type = SPA_ELEMENT_TYPE_F64_LE;
		*datatype = TDOUBLE;
		*element_size = sizeof(double);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int map_file(struct fits_cube *cube, const char *path, bool prefault,
		char *message, size_t message_size)
{
	struct stat attributes;

	cube->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (cube->fd < 0)
		return fail(message, message_size, -errno,
				"could not open %s: %s", path, strerror(errno));
	if (fstat(cube->fd, &attributes) < 0)
		return fail(message, message_size, -errno,
				"could not stat %s: %s", path, strerror(errno));
	if (attributes.st_size <= 0 || (uintmax_t)attributes.st_size > SIZE_MAX)
		return fail(message, message_size, -EFBIG,
				"FITS file has an unsupported size");
	cube->mapping_size = (size_t)attributes.st_size;
	cube->mapping = mmap(NULL, cube->mapping_size, PROT_READ, MAP_PRIVATE,
			cube->fd, 0);
	if (cube->mapping == MAP_FAILED) {
		cube->mapping = NULL;
		return fail(message, message_size, -errno,
				"could not map %s: %s", path, strerror(errno));
	}
	if (prefault) {
		const volatile uint8_t *bytes = cube->mapping;
		long page_size = sysconf(_SC_PAGESIZE);
		size_t step = page_size > 0 ? (size_t)page_size : 4096u;
		volatile uint8_t touched = 0;
		size_t offset;

		(void)madvise(cube->mapping, cube->mapping_size, MADV_WILLNEED);
		for (offset = 0; offset < cube->mapping_size; offset += step)
			touched ^= bytes[offset];
		touched ^= bytes[cube->mapping_size - 1];
		(void)touched;
	}
	return 0;
}

static int open_fits(struct fits_cube *cube,
		const struct fits_cube_options *options,
		char *message, size_t message_size)
{
	int status = 0, res;

	if (options->io_mode == FITS_CUBE_IO_MMAP) {
		if ((res = map_file(cube, options->path, options->prefault,
				message, message_size)) < 0)
			return res;
		fits_open_memfile(&cube->file, options->path, READONLY,
				&cube->mapping, &cube->mapping_size, 0, NULL, &status);
	} else {
		fits_open_diskfile(&cube->file, options->path, READONLY, &status);
	}
	return status == 0 ? 0 : fits_fail(message, message_size, status,
			"could not open FITS file");
}

int fits_cube_open(struct fits_cube **result,
		const struct fits_cube_options *options,
		char *message, size_t message_size)
{
	struct fits_cube *cube;
	LONGLONG axes[3] = { 1, 1, 1 };
	int equivalent_type, hdu_type, dimensions, status = 0, res;
	uint64_t plane_elements;

	if (result == NULL || options == NULL || options->path == NULL ||
			options->path[0] == '\0' || options->hdu == 0 ||
			options->hdu > INT_MAX ||
			(options->io_mode != FITS_CUBE_IO_FILE &&
			 options->io_mode != FITS_CUBE_IO_MMAP))
		return fail(message, message_size, -EINVAL,
				"invalid FITS cube options");
	*result = NULL;
	cube = calloc(1, sizeof(*cube));
	if (cube == NULL)
		return fail(message, message_size, -errno,
				"could not allocate FITS cube state");
	cube->fd = -1;
	if ((res = open_fits(cube, options, message, message_size)) < 0)
		goto error;
	fits_movabs_hdu(cube->file, (int)options->hdu, &hdu_type, &status);
	if (status != 0) {
		res = fits_fail(message, message_size, status,
				"could not select FITS HDU");
		goto error;
	}
	if (hdu_type != IMAGE_HDU) {
		res = fail(message, message_size, -EINVAL,
				"selected FITS HDU is not an image");
		goto error;
	}
	fits_get_img_dim(cube->file, &dimensions, &status);
	if (status == 0)
		fits_get_img_sizell(cube->file, SPA_N_ELEMENTS(axes), axes, &status);
	if (status == 0)
		fits_get_img_equivtype(cube->file, &equivalent_type, &status);
	if (status != 0) {
		res = fits_fail(message, message_size, status,
				"could not inspect FITS image");
		goto error;
	}
	if (dimensions != 2 && dimensions != 3) {
		res = fail(message, message_size, -EINVAL,
				"FITS image must have two or three axes");
		goto error;
	}
	if (axes[0] <= 0 || axes[0] > UINT32_MAX ||
			axes[1] <= 0 || axes[1] > UINT32_MAX ||
			axes[2] <= 0) {
		res = fail(message, message_size, -EOVERFLOW,
				"FITS image dimensions are unsupported");
		goto error;
	}
	if ((res = image_type(equivalent_type, &cube->info.element_type,
			&cube->native_datatype, &cube->info.element_size)) < 0) {
		res = fail(message, message_size, res,
				"FITS image element type is unsupported");
		goto error;
	}
	plane_elements = (uint64_t)axes[0] * (uint64_t)axes[1];
	if (plane_elements > SIZE_MAX ||
			plane_elements > SIZE_MAX / cube->info.element_size ||
			plane_elements > LONGLONG_MAX) {
		res = fail(message, message_size, -EOVERFLOW,
				"FITS image plane is too large");
		goto error;
	}
	cube->info.width = (uint32_t)axes[0];
	cube->info.height = (uint32_t)axes[1];
	cube->info.frames = dimensions == 3 ? (uint64_t)axes[2] : 1u;
	if (plane_elements > (uint64_t)LONGLONG_MAX / cube->info.frames) {
		res = fail(message, message_size, -EOVERFLOW,
				"FITS image cube is too large");
		goto error;
	}
	cube->info.plane_elements = (size_t)plane_elements;
	cube->info.plane_size = (size_t)plane_elements * cube->info.element_size;
	*result = cube;
	return 0;

error:
	fits_cube_close(cube);
	return res;
}

void fits_cube_close(struct fits_cube *cube)
{
	if (cube == NULL)
		return;
	if (cube->file != NULL) {
		int status = 0;

		fits_close_file(cube->file, &status);
	}
	if (cube->mapping != NULL)
		(void)munmap(cube->mapping, cube->mapping_size);
	if (cube->fd >= 0)
		(void)close(cube->fd);
	free(cube);
}

const struct fits_cube_info *fits_cube_get_info(const struct fits_cube *cube)
{
	return cube == NULL ? NULL : &cube->info;
}

int fits_cube_read_plane(struct fits_cube *cube, uint64_t frame,
		enum fits_cube_output output, void *destination,
		size_t destination_size)
{
	LONGLONG first_element, elements;
	size_t required;
	int any_null = 0, datatype, status = 0;

	if (cube == NULL || destination == NULL || frame >= cube->info.frames)
		return -EINVAL;
	if (output == FITS_CUBE_OUTPUT_NATIVE) {
		datatype = cube->native_datatype;
		required = cube->info.plane_size;
	} else if (output == FITS_CUBE_OUTPUT_GRAY16) {
		datatype = TUSHORT;
		if (cube->info.plane_elements > SIZE_MAX / sizeof(uint16_t))
			return -EOVERFLOW;
		required = cube->info.plane_elements * sizeof(uint16_t);
	} else {
		return -EINVAL;
	}
	if (destination_size < required)
		return -ENOSPC;
	first_element = (LONGLONG)(frame * cube->info.plane_elements + 1u);
	elements = (LONGLONG)cube->info.plane_elements;
	fits_read_img(cube->file, datatype, first_element, elements, NULL,
			destination, &any_null, &status);
	return status == 0 ? 0 : -ERANGE;
}

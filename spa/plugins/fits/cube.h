/* SPDX-License-Identifier: MIT */
#ifndef SPA_FITS_CUBE_H
#define SPA_FITS_CUBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spa/param/format.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fits_cube_io_mode {
	FITS_CUBE_IO_FILE,
	FITS_CUBE_IO_MMAP,
};

enum fits_cube_output {
	FITS_CUBE_OUTPUT_NATIVE,
	FITS_CUBE_OUTPUT_GRAY16,
};

struct fits_cube_options {
	const char *path;
	uint32_t hdu;
	enum fits_cube_io_mode io_mode;
	bool prefault;
};

struct fits_cube_info {
	uint32_t width;
	uint32_t height;
	uint64_t frames;
	enum spa_element_type element_type;
	size_t element_size;
	size_t plane_elements;
	size_t plane_size;
};

struct fits_cube;

int fits_cube_open(struct fits_cube **cube,
		const struct fits_cube_options *options,
		char *message, size_t message_size);
void fits_cube_close(struct fits_cube *cube);

const struct fits_cube_info *fits_cube_get_info(const struct fits_cube *cube);

int fits_cube_read_plane(struct fits_cube *cube, uint64_t frame,
		enum fits_cube_output output, void *destination,
		size_t destination_size);

#ifdef __cplusplus
}
#endif

#endif

/* SPDX-License-Identifier: MIT */
#include "camera.h"

#include <errno.h>
#include <elf.h>
#include <limits.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Hermes_SDK.h>

#include "hermes.h"

/*
 * The HERMES SDK does not expose its FrontPanel handle. These offsets are for
 * MPD's Linux x86-64 SDK whose libHermes.so build ID is
 * 2ce43284a3064e9f69d4e42ea1044a04cc4e48b3. Keep this compatibility shim
 * isolated: all streaming below uses the public FrontPanel C ABI and caller
 * memory, while libHermes remains responsible for camera configuration.
 */
#define HERMES_INTERFACE_OFFSET 0u
#define HERMES_FRONTPANEL_OFFSET 0xe0u

static const uint8_t supported_build_id[] = {
	0x2c, 0xe4, 0x32, 0x84, 0xa3, 0x06, 0x4e, 0x9f, 0x69, 0xd4,
	0xe4, 0x2e, 0xa1, 0x04, 0x4a, 0x04, 0xcc, 0x4e, 0x48, 0xb3,
};

typedef void *okFrontPanel_HANDLE;

extern long okFrontPanel_ReadFromBlockPipeOut(okFrontPanel_HANDLE handle,
		int endpoint, int block_size, long length, unsigned char *data);
extern int okFrontPanel_UpdateWireOuts(okFrontPanel_HANDLE handle);
extern unsigned long okFrontPanel_GetWireOutValue(okFrontPanel_HANDLE handle,
		int endpoint);

struct hermes_camera_buffer {
	void *memory;
	uint64_t size;
	void *user_data;
	bool queued;
};

struct hermes_camera {
	Hermes_H hermes;
	okFrontPanel_HANDLE frontpanel;
	struct hermes_camera_info info;
	struct hermes_camera_buffer *buffers[HERMES_CAMERA_MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	uint64_t batch_id;
	bool started;
};

struct build_id_search {
	void *base;
	bool matches;
};

static size_t note_align(size_t value)
{
	return (value + 3u) & ~(size_t)3u;
}

static int inspect_build_id(struct dl_phdr_info *info, size_t size SPA_UNUSED,
		void *data)
{
	struct build_id_search *search = data;
	uint16_t i;

	if ((void *)info->dlpi_addr != search->base)
		return 0;
	for (i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *program = &info->dlpi_phdr[i];
		const uint8_t *cursor, *end;

		if (program->p_type != PT_NOTE || program->p_memsz < sizeof(ElfW(Nhdr)))
			continue;
		cursor = (const uint8_t *)(info->dlpi_addr + program->p_vaddr);
		end = cursor + program->p_memsz;
		while ((size_t)(end - cursor) >= sizeof(ElfW(Nhdr))) {
			const ElfW(Nhdr) *note = (const ElfW(Nhdr) *)cursor;
			size_t name_size = note_align(note->n_namesz);
			size_t desc_size = note_align(note->n_descsz);
			const uint8_t *name, *description;

			cursor += sizeof(*note);
			if (name_size > (size_t)(end - cursor))
				break;
			name = cursor;
			cursor += name_size;
			if (desc_size > (size_t)(end - cursor))
				break;
			description = cursor;
			cursor += desc_size;
			if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
					note->n_descsz == sizeof(supported_build_id) &&
					memcmp(name, "GNU", 4) == 0) {
				search->matches = memcmp(description, supported_build_id,
						sizeof(supported_build_id)) == 0;
				return 1;
			}
		}
	}
	return 1;
}

static bool supported_hermes_library(void)
{
	struct build_id_search search = { 0 };
	Dl_info info;

	if (dladdr((void *)HermesConstr, &info) == 0 || info.dli_fbase == NULL)
		return false;
	search.base = info.dli_fbase;
	(void)dl_iterate_phdr(inspect_build_id, &search);
	return search.matches;
}

static int sdk_error(HermesReturn result)
{
	switch (result) {
	case OK:
		return 0;
	case NULL_POINTER:
		return -EINVAL;
	case NOT_EN_MEMORY:
		return -ENOMEM;
	case OUT_OF_BOUND:
		return -ERANGE;
	case COMMUNICATION_ERROR:
		return -EIO;
	case HERMES_MEMORY_FULL:
		return -ENOBUFS;
	case INVALID_OP:
		return -EBUSY;
	default:
		return -EIO;
	}
}

static const char *profile_for(bool half_array, uint32_t bits_per_pixel)
{
	if (half_array)
		return bits_per_pixel == 8 ? SPA_HERMES_HALF_U8_PROFILE :
				SPA_HERMES_HALF_U16_PROFILE;
	return bits_per_pixel == 8 ? SPA_HERMES_FULL_U8_PROFILE :
			SPA_HERMES_FULL_U16_PROFILE;
}

static okFrontPanel_HANDLE frontpanel_from_hermes(Hermes_H hermes)
{
	uint8_t *interface;

	if (hermes == NULL)
		return NULL;
	interface = *(uint8_t **)((uint8_t *)hermes + HERMES_INTERFACE_OFFSET);
	if (interface == NULL)
		return NULL;
	return *(okFrontPanel_HANDLE *)(interface + HERMES_FRONTPANEL_OFFSET);
}

static int check_options(const struct hermes_camera_options *options,
		uint64_t *payload_size)
{
	uint64_t pixels, plane_bytes;

	if (options == NULL || options->device_id == NULL ||
			options->exposure_clocks < 1 ||
			options->exposure_clocks > UINT16_MAX - 1u ||
			options->integrated_frames < 1 ||
			options->integrated_frames > UINT16_MAX - 1u ||
			options->counters < 1 || options->counters > 3 ||
			options->frames_per_buffer == 0 ||
			(options->bits_per_pixel != 8 && options->bits_per_pixel != 16))
		return -EINVAL;
	pixels = options->half_array ? 32u * 32u : 64u * 32u;
	plane_bytes = pixels * (options->bits_per_pixel / 8u);
	*payload_size = plane_bytes * options->counters *
			options->frames_per_buffer;
	if (*payload_size == 0 || *payload_size > LONG_MAX ||
			*payload_size > UINT32_MAX ||
			*payload_size % HERMES_FRONT_PANEL_BLOCK_SIZE != 0)
		return -EINVAL;
	return 0;
}

int hermes_camera_open(struct hermes_camera **camera_ptr,
		const struct hermes_camera_options *options)
{
	struct hermes_camera *camera;
	char device_id[64], camera_id[16] = { 0 }, serial[64] = { 0 };
	uint64_t payload_size;
	short is_16_bit = 0;
	HermesReturn result;
	int res;

	if (camera_ptr == NULL || (res = check_options(options, &payload_size)) < 0)
		return res < 0 ? res : -EINVAL;
	*camera_ptr = NULL;
	if (!supported_hermes_library())
		return -ELIBBAD;
	if (strlen(options->device_id) >= sizeof(device_id))
		return -ENAMETOOLONG;
	memcpy(device_id, options->device_id, strlen(options->device_id) + 1u);
	camera = calloc(1, sizeof(*camera));
	if (camera == NULL)
		return -errno;
	result = HermesConstr(&camera->hermes,
			options->advanced_mode ? Advanced : Normal, device_id);
	if ((res = sdk_error(result)) < 0)
		goto error;
	result = HermesSetCameraPar(camera->hermes,
			(UInt16)options->exposure_clocks, 1u,
			(UInt16)options->integrated_frames,
			(UInt16)options->counters,
			options->force_8bit ? Enabled : Disabled,
			options->half_array ? Enabled : Disabled,
			options->signed_data ? Enabled : Disabled);
	if ((res = sdk_error(result)) < 0)
		goto error;
	if ((res = sdk_error(HermesApplySettings(camera->hermes))) < 0 ||
			(res = sdk_error(HermesIs16Bit(camera->hermes,
					&is_16_bit))) < 0)
		goto error;
	if ((is_16_bit ? 16u : 8u) != options->bits_per_pixel) {
		res = -EINVAL;
		goto error;
	}
	camera->frontpanel = frontpanel_from_hermes(camera->hermes);
	if (camera->frontpanel == NULL) {
		res = -ENODEV;
		goto error;
	}
	(void)HermesGetSerial(camera->hermes, camera_id, serial);
	camera->info = (struct hermes_camera_info) {
		.payload_size = payload_size,
		.width = options->half_array ? 32u : 64u,
		.height = 32u,
		.counters = options->counters,
		.frames_per_buffer = options->frames_per_buffer,
		.bits_per_pixel = options->bits_per_pixel,
		.raw_plane_bytes = (options->half_array ? 32u * 32u : 64u * 32u) *
				(options->bits_per_pixel / 8u),
	};
	(void)snprintf(camera->info.model, sizeof(camera->info.model),
			"MPD HERMES 64x32");
	(void)snprintf(camera->info.serial, sizeof(camera->info.serial), "%s",
			serial[0] != '\0' ? serial : camera_id);
	(void)snprintf(camera->info.profile, sizeof(camera->info.profile), "%s",
			profile_for(options->half_array, options->bits_per_pixel));
	*camera_ptr = camera;
	return 0;

error:
	if (camera->hermes != NULL)
		(void)HermesDestr(camera->hermes);
	free(camera);
	return res;
}

void hermes_camera_close(struct hermes_camera *camera)
{
	uint32_t i;

	if (camera == NULL)
		return;
	(void)hermes_camera_stop(camera);
	for (i = 0; i < camera->n_buffers; i++)
		free(camera->buffers[i]);
	if (camera->hermes != NULL)
		(void)HermesDestr(camera->hermes);
	free(camera);
}

const struct hermes_camera_info *hermes_camera_get_info(
		const struct hermes_camera *camera)
{
	return camera == NULL ? NULL : &camera->info;
}

int hermes_camera_announce(struct hermes_camera *camera, void *memory,
		uint64_t size, void *user_data,
		struct hermes_camera_buffer **buffer_ptr)
{
	struct hermes_camera_buffer *buffer;

	if (camera == NULL || memory == NULL || buffer_ptr == NULL ||
			size < camera->info.payload_size ||
			camera->n_buffers >= HERMES_CAMERA_MAX_BUFFERS)
		return -EINVAL;
	buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return -errno;
	buffer->memory = memory;
	buffer->size = size;
	buffer->user_data = user_data;
	camera->buffers[camera->n_buffers++] = buffer;
	*buffer_ptr = buffer;
	return 0;
}

int hermes_camera_revoke(struct hermes_camera *camera,
		struct hermes_camera_buffer **buffer_ptr)
{
	struct hermes_camera_buffer *buffer;
	uint32_t i;

	if (camera == NULL || buffer_ptr == NULL ||
			(buffer = *buffer_ptr) == NULL || camera->started)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] != buffer)
			continue;
		memmove(&camera->buffers[i], &camera->buffers[i + 1],
				(camera->n_buffers - i - 1u) * sizeof(camera->buffers[0]));
		camera->n_buffers--;
		free(buffer);
		*buffer_ptr = NULL;
		return 0;
	}
	return -ENOENT;
}

int hermes_camera_queue(struct hermes_camera *camera,
		struct hermes_camera_buffer *buffer)
{
	uint32_t i;

	if (camera == NULL || buffer == NULL || buffer->queued)
		return -EINVAL;
	for (i = 0; i < camera->n_buffers; i++) {
		if (camera->buffers[i] == buffer) {
			buffer->queued = true;
			return 0;
		}
	}
	return -ENOENT;
}

int hermes_camera_start(struct hermes_camera *camera)
{
	int res;

	if (camera == NULL || camera->hermes == NULL || camera->n_buffers < 2)
		return -EINVAL;
	if (camera->started)
		return 0;
	if ((res = sdk_error(HermesContAcqToMemoryStart(camera->hermes))) < 0)
		return res;
	camera->batch_id = 0;
	camera->started = true;
	return 0;
}

int hermes_camera_stop(struct hermes_camera *camera)
{
	uint32_t i;
	int res = 0;

	if (camera == NULL)
		return -EINVAL;
	if (camera->started)
		res = sdk_error(HermesContAcqToMemoryStop(camera->hermes));
	for (i = 0; i < camera->n_buffers; i++)
		camera->buffers[i]->queued = false;
	camera->started = false;
	return res;
}

static struct hermes_camera_buffer *take_queued_buffer(
		struct hermes_camera *camera)
{
	uint32_t i;

	for (i = 0; i < camera->n_buffers; i++) {
		uint32_t index = (camera->scan_hint + i) % camera->n_buffers;
		struct hermes_camera_buffer *buffer = camera->buffers[index];

		if (!buffer->queued)
			continue;
		buffer->queued = false;
		camera->scan_hint = (index + 1u) % camera->n_buffers;
		return buffer;
	}
	return NULL;
}

static int wire_status_error(okFrontPanel_HANDLE frontpanel)
{
	unsigned long status;

	if (okFrontPanel_UpdateWireOuts(frontpanel) < 0)
		return -EIO;
	status = okFrontPanel_GetWireOutValue(frontpanel, 0x20);
	if (status & 0x20u)
		return -EIO;
	if (status & 0x08u)
		return -ENOBUFS;
	if (status & (0x01u | 0x02u | 0x100u))
		return -EIO;
	return 0;
}

int hermes_camera_get_completion(struct hermes_camera *camera,
		struct hermes_camera_completion *completion)
{
	struct hermes_camera_buffer *buffer;
	long transferred;
	int res;

	if (camera == NULL || completion == NULL || !camera->started)
		return -EINVAL;
	buffer = take_queued_buffer(camera);
	if (buffer == NULL)
		return 0;
	transferred = okFrontPanel_ReadFromBlockPipeOut(camera->frontpanel, 0xa1,
			HERMES_FRONT_PANEL_BLOCK_SIZE, (long)camera->info.payload_size,
			buffer->memory);
	if (transferred < 0 || (uint64_t)transferred != camera->info.payload_size) {
		buffer->queued = true;
		return transferred < 0 ? -EIO : -EMSGSIZE;
	}
	if ((res = wire_status_error(camera->frontpanel)) < 0) {
		buffer->queued = true;
		return res;
	}
	camera->batch_id++;
	*completion = (struct hermes_camera_completion) {
		.buffer = buffer,
		.user_data = buffer->user_data,
		.batch_id = camera->batch_id,
		.size_filled = (uint64_t)transferred,
	};
	return 1;
}

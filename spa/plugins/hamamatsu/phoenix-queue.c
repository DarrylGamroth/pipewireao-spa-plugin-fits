/* SPDX-License-Identifier: MIT */
#include "phoenix-queue.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * These values are present in the Phoenix 3.7 runtime enum-name table and
 * were verified against every FireBird backend shipped with the Hamamatsu
 * DCAM 26.6.7175 runtime. They are intentionally kept local: this adapter is
 * build-ID gated and does not present them as a supported vendor API.
 */
#define PHX_BUFFER_GET UINT32_C(0xc0030600)
#define PHX_BUFFER_RELEASE UINT32_C(0xc0030700)
#define PHX_USER_LOCK UINT32_C(0xc0031000)
#define PHX_USER_UNLOCK UINT32_C(0xc0031100)
#define PHX_BUFFER_OBJECT_GET UINT32_C(0xc0031200)
#define PHX_BUFFER_VIRTUAL_ADDR UINT32_C(0)
#define PHX_STATUS_OK 0
#define PHX_STATUS_BAD_PARAMETER 8

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t supported_fgphnx_build_id[] = {
	0x05, 0x34, 0xbb, 0x1b, 0x62, 0x1b, 0x2a, 0x8a, 0x2e, 0x4c,
	0x87, 0xd2, 0x21, 0x51, 0xe2, 0x7b, 0x0a, 0xa1, 0xda, 0xc0,
};

typedef int (*phx_acquire_func_t)(uintptr_t handle, uint32_t command,
		void *parameter);
typedef int (*phx_buffer_parameter_get_func_t)(uintptr_t handle,
		void *buffer, uint32_t parameter, void *value);

struct phoenix_user_buffer {
	void *memory;
	size_t size;
};

struct phoenix_queue_buffer {
	struct phoenix_queue_buffer *next;
	void *memory;
	size_t size;
	void *object;
	uintptr_t handle;
};

struct phoenix_queue {
	struct phoenix_queue_buffer *buffers;
	struct phoenix_queue_buffer *current;
	uintptr_t handle;
	bool test_mode;
};

struct phoenix_patch {
	void **slot;
	void *saved_slot;
	void *phxapi;
	phx_acquire_func_t acquire;
	phx_buffer_parameter_get_func_t buffer_parameter_get;
};

static pthread_mutex_t manager_lock = PTHREAD_MUTEX_INITIALIZER;
static struct phoenix_queue *active_queue;
static struct phoenix_patch patch;

static int phoenix_acquire_hook(uintptr_t handle, uint32_t command,
		void *parameter);

static uintptr_t dynamic_address(const struct dl_phdr_info *info,
		ElfW(Addr) value)
{
	return value < info->dlpi_addr ? info->dlpi_addr + value : value;
}

static bool has_supported_build_id(const struct dl_phdr_info *info)
{
	size_t i;

	for (i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[i];
		const uint8_t *cursor, *end;

		if (header->p_type != PT_NOTE)
			continue;
		cursor = (const uint8_t *)(info->dlpi_addr + header->p_vaddr);
		end = cursor + header->p_memsz;
		while ((size_t)(end - cursor) >= sizeof(ElfW(Nhdr))) {
			const ElfW(Nhdr) *note = (const ElfW(Nhdr) *)cursor;
			const uint8_t *name = cursor + sizeof(*note);
			const uint8_t *description = name + ((note->n_namesz + 3u) & ~3u);
			const uint8_t *next = description + ((note->n_descsz + 3u) & ~3u);

			if (next > end)
				break;
			if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4u &&
					note->n_descsz == sizeof(supported_fgphnx_build_id) &&
					memcmp(name, "GNU", 4u) == 0 &&
					memcmp(description, supported_fgphnx_build_id,
						sizeof(supported_fgphnx_build_id)) == 0)
				return true;
			cursor = next;
		}
	}
	return false;
}

static bool address_is_writable(const struct dl_phdr_info *info,
		uintptr_t address)
{
	size_t i;

	for (i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[i];
		uintptr_t begin, end;

		if (header->p_type != PT_LOAD)
			continue;
		begin = info->dlpi_addr + header->p_vaddr;
		end = begin + header->p_memsz;
		if (address >= begin && address + sizeof(void *) <= end)
			return (header->p_flags & PF_W) != 0;
	}
	return false;
}

struct find_patch_data {
	void **slot;
	int error;
};

static int find_patch_slot(struct dl_phdr_info *info, size_t size,
		void *user_data)
{
	struct find_patch_data *result = user_data;
	const char *name;
	ElfW(Dyn) *dynamic = NULL;
	ElfW(Rela) *relocations = NULL;
	ElfW(Sym) *symbols = NULL;
	const char *strings = NULL;
	size_t relocation_size = 0, i;
	ElfW(Sxword) relocation_type = 0;

	(void)size;
	name = strrchr(info->dlpi_name, '/');
	name = name == NULL ? info->dlpi_name : name + 1;
	if (strncmp(name, "libfgphnx.so.4", strlen("libfgphnx.so.4")) != 0)
		return 0;
	if (!has_supported_build_id(info)) {
		result->error = -ENOTSUP;
		return 1;
	}
	for (i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
			dynamic = (ElfW(Dyn) *)(info->dlpi_addr +
					info->dlpi_phdr[i].p_vaddr);
			break;
		}
	}
	if (dynamic == NULL) {
		result->error = -ENOEXEC;
		return 1;
	}
	for (; dynamic->d_tag != DT_NULL; dynamic++) {
		switch (dynamic->d_tag) {
		case DT_JMPREL:
			relocations = (ElfW(Rela) *)dynamic_address(info,
					dynamic->d_un.d_ptr);
			break;
		case DT_PLTRELSZ: relocation_size = dynamic->d_un.d_val; break;
		case DT_PLTREL: relocation_type = dynamic->d_un.d_val; break;
		case DT_SYMTAB:
			symbols = (ElfW(Sym) *)dynamic_address(info,
					dynamic->d_un.d_ptr);
			break;
		case DT_STRTAB:
			strings = (const char *)dynamic_address(info,
					dynamic->d_un.d_ptr);
			break;
		default: break;
		}
	}
	if (relocations == NULL || symbols == NULL || strings == NULL ||
			relocation_type != DT_RELA) {
		result->error = -ENOEXEC;
		return 1;
	}
	for (i = 0; i < relocation_size / sizeof(*relocations); i++) {
#if __SIZEOF_POINTER__ == 8
		const size_t symbol_index = ELF64_R_SYM(relocations[i].r_info);
#else
		const size_t symbol_index = ELF32_R_SYM(relocations[i].r_info);
#endif
		uintptr_t slot_address;

		if (strcmp(strings + symbols[symbol_index].st_name, "PHX_Acquire") != 0)
			continue;
		slot_address = info->dlpi_addr + relocations[i].r_offset;
		if (!address_is_writable(info, slot_address)) {
			result->error = -EACCES;
			return 1;
		}
		result->slot = (void **)slot_address;
		return 1;
	}
	result->error = -ENOENT;
	return 1;
}

static int resolve_function(void *library, const char *name, void *function,
		size_t function_size)
{
	void *symbol;

	dlerror();
	symbol = dlsym(library, name);
	if (symbol == NULL || dlerror() != NULL || function_size != sizeof(symbol))
		return -ENOENT;
	memcpy(function, &symbol, sizeof(symbol));
	return 0;
}

static int install_patch(void)
{
	struct find_patch_data found = { .error = -ENOENT };
	int result;

	dl_iterate_phdr(find_patch_slot, &found);
	if (found.slot == NULL)
		return found.error;
	patch.phxapi = dlopen("libphxapi-x86_64.so", RTLD_NOW | RTLD_NOLOAD);
	if (patch.phxapi == NULL)
		return -ENOENT;
	result = resolve_function(patch.phxapi, "PHX_Acquire", &patch.acquire,
			sizeof(patch.acquire));
	if (result == 0)
		result = resolve_function(patch.phxapi, "PHX_BufferParameterGet",
				&patch.buffer_parameter_get,
				sizeof(patch.buffer_parameter_get));
	if (result < 0) {
		dlclose(patch.phxapi);
		memset(&patch, 0, sizeof(patch));
		return result;
	}
	patch.slot = found.slot;
	patch.saved_slot = __atomic_load_n(patch.slot, __ATOMIC_ACQUIRE);
	__atomic_store_n(patch.slot, (void *)phoenix_acquire_hook, __ATOMIC_RELEASE);
	return 0;
}

static void uninstall_patch(void)
{
	if (patch.slot != NULL && __atomic_load_n(patch.slot, __ATOMIC_ACQUIRE) ==
			(void *)phoenix_acquire_hook)
		__atomic_store_n(patch.slot, patch.saved_slot, __ATOMIC_RELEASE);
	if (patch.phxapi != NULL)
		dlclose(patch.phxapi);
	memset(&patch, 0, sizeof(patch));
}

static struct phoenix_queue_buffer *find_buffer(struct phoenix_queue *queue,
		void *memory)
{
	struct phoenix_queue_buffer *buffer;

	for (buffer = queue->buffers; buffer != NULL; buffer = buffer->next)
		if (buffer->memory == memory)
			return buffer;
	return NULL;
}

static int handle_user_lock(phx_acquire_func_t acquire,
		struct phoenix_queue *queue, uintptr_t handle, uint32_t command,
		void *parameter)
{
	const struct phoenix_user_buffer *user_buffer = parameter;
	void *memory = user_buffer == NULL ? NULL : user_buffer->memory;
	struct phoenix_queue_buffer *buffer;
	int result = acquire(handle, command, parameter);

	if (result != PHX_STATUS_OK || memory == NULL)
		return result;
	pthread_mutex_lock(&manager_lock);
	buffer = active_queue == queue ? find_buffer(queue, memory) : NULL;
	if (buffer != NULL && (queue->handle == 0 || queue->handle == handle)) {
		queue->handle = handle;
		buffer->handle = handle;
	}
	pthread_mutex_unlock(&manager_lock);
	return result;
}

static int handle_user_unlock(phx_acquire_func_t acquire,
		struct phoenix_queue *queue, uintptr_t handle, uint32_t command,
		void *parameter)
{
	const struct phoenix_user_buffer *user_buffer = parameter;
	void *memory = user_buffer == NULL ? NULL : user_buffer->memory;
	struct phoenix_queue_buffer *buffer;
	int result = acquire(handle, command, parameter);

	if (result != PHX_STATUS_OK || memory == NULL)
		return result;
	pthread_mutex_lock(&manager_lock);
	buffer = active_queue == queue ? find_buffer(queue, memory) : NULL;
	if (buffer != NULL) {
		buffer->handle = 0;
		buffer->object = NULL;
		if (queue->current == buffer)
			queue->current = NULL;
	}
	pthread_mutex_unlock(&manager_lock);
	return result;
}

static int handle_buffer_get(phx_acquire_func_t acquire,
		phx_buffer_parameter_get_func_t parameter_get,
		struct phoenix_queue *queue, uintptr_t handle, void *parameter)
{
	struct phoenix_user_buffer *result_buffer = parameter;
	struct phoenix_queue_buffer *buffer;
	void *object = NULL, *memory = NULL;
	int result;

	if (result_buffer == NULL)
		return PHX_STATUS_BAD_PARAMETER;
	result = acquire(handle, PHX_BUFFER_OBJECT_GET, &object);
	if (result != PHX_STATUS_OK)
		return result;
	result = parameter_get(handle, object, PHX_BUFFER_VIRTUAL_ADDR, &memory);
	if (result != PHX_STATUS_OK) {
		(void)acquire(handle, PHX_BUFFER_RELEASE, object);
		return result;
	}
	pthread_mutex_lock(&manager_lock);
	buffer = active_queue == queue ? find_buffer(queue, memory) : NULL;
	if (buffer == NULL || buffer->handle != handle || buffer->object != NULL ||
			queue->current != NULL) {
		pthread_mutex_unlock(&manager_lock);
		(void)acquire(handle, PHX_BUFFER_RELEASE, object);
		return PHX_STATUS_BAD_PARAMETER;
	}
	buffer->object = object;
	queue->current = buffer;
	result_buffer->memory = memory;
	result_buffer->size = buffer->size;
	pthread_mutex_unlock(&manager_lock);
	return PHX_STATUS_OK;
}

static int phoenix_acquire_hook(uintptr_t handle, uint32_t command,
		void *parameter)
{
	struct phoenix_queue *queue;
	phx_acquire_func_t acquire;
	phx_buffer_parameter_get_func_t parameter_get;
	bool associated;

	pthread_mutex_lock(&manager_lock);
	queue = active_queue;
	acquire = patch.acquire;
	parameter_get = patch.buffer_parameter_get;
	associated = queue != NULL && queue->handle != 0 && queue->handle == handle;
	pthread_mutex_unlock(&manager_lock);
	if (acquire == NULL)
		return PHX_STATUS_BAD_PARAMETER;
	if (queue != NULL && command == PHX_USER_LOCK)
		return handle_user_lock(acquire, queue, handle, command, parameter);
	if (queue != NULL && command == PHX_USER_UNLOCK)
		return handle_user_unlock(acquire, queue, handle, command, parameter);
	if (!associated)
		return acquire(handle, command, parameter);
	if (command == PHX_BUFFER_GET)
		return handle_buffer_get(acquire, parameter_get, queue, handle, parameter);
	if (command == PHX_BUFFER_RELEASE && parameter == NULL) {
		pthread_mutex_lock(&manager_lock);
		if (active_queue == queue)
			queue->current = NULL;
		pthread_mutex_unlock(&manager_lock);
		return PHX_STATUS_OK;
	}
	return acquire(handle, command, parameter);
}

static int create_queue(struct phoenix_queue **queue,
		phx_acquire_func_t acquire,
		phx_buffer_parameter_get_func_t parameter_get, bool test_mode)
{
	struct phoenix_queue *created;
	int result = 0;

	if (queue == NULL)
		return -EINVAL;
	*queue = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return -errno;
	pthread_mutex_lock(&manager_lock);
	if (active_queue != NULL) {
		result = -EBUSY;
	} else if (!test_mode && (result = install_patch()) < 0) {
		/* install_patch does not publish the hook until it succeeds. */
	} else {
		if (test_mode) {
			patch.acquire = acquire;
			patch.buffer_parameter_get = parameter_get;
			created->test_mode = true;
		}
		active_queue = created;
	}
	pthread_mutex_unlock(&manager_lock);
	if (result < 0) {
		free(created);
		return result;
	}
	*queue = created;
	return 0;
}

int phoenix_queue_create(struct phoenix_queue **queue)
{
	return create_queue(queue, NULL, NULL, false);
}

void phoenix_queue_destroy(struct phoenix_queue *queue)
{
	struct phoenix_queue_buffer *buffer, *next;

	if (queue == NULL)
		return;
	(void)phoenix_queue_flush(queue);
	pthread_mutex_lock(&manager_lock);
	if (active_queue == queue) {
		active_queue = NULL;
		if (queue->test_mode)
			memset(&patch, 0, sizeof(patch));
		else
			uninstall_patch();
	}
	pthread_mutex_unlock(&manager_lock);
	for (buffer = queue->buffers; buffer != NULL; buffer = next) {
		next = buffer->next;
		free(buffer);
	}
	free(queue);
}

int phoenix_queue_register(struct phoenix_queue *queue, void *memory,
		size_t size, struct phoenix_queue_buffer **buffer)
{
	struct phoenix_queue_buffer *created;

	if (queue == NULL || memory == NULL || size == 0 || buffer == NULL)
		return -EINVAL;
	*buffer = NULL;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return -errno;
	created->memory = memory;
	created->size = size;
	pthread_mutex_lock(&manager_lock);
	if (active_queue != queue || find_buffer(queue, memory) != NULL) {
		pthread_mutex_unlock(&manager_lock);
		free(created);
		return -EINVAL;
	}
	created->next = queue->buffers;
	queue->buffers = created;
	pthread_mutex_unlock(&manager_lock);
	*buffer = created;
	return 0;
}

int phoenix_queue_unregister(struct phoenix_queue *queue,
		struct phoenix_queue_buffer **buffer)
{
	struct phoenix_queue_buffer **entry;

	if (queue == NULL || buffer == NULL || *buffer == NULL)
		return -EINVAL;
	pthread_mutex_lock(&manager_lock);
	for (entry = &queue->buffers; *entry != NULL; entry = &(*entry)->next) {
		if (*entry != *buffer)
			continue;
		if ((*entry)->object != NULL || queue->current == *entry) {
			pthread_mutex_unlock(&manager_lock);
			return -EBUSY;
		}
		*entry = (*entry)->next;
		pthread_mutex_unlock(&manager_lock);
		free(*buffer);
		*buffer = NULL;
		return 0;
	}
	pthread_mutex_unlock(&manager_lock);
	return -ENOENT;
}

int phoenix_queue_requeue(struct phoenix_queue *queue,
		struct phoenix_queue_buffer *buffer)
{
	phx_acquire_func_t acquire;
	void *object;
	uintptr_t handle;
	int result;

	if (queue == NULL || buffer == NULL)
		return -EINVAL;
	pthread_mutex_lock(&manager_lock);
	if (active_queue != queue || buffer->object == NULL ||
			buffer->handle == 0) {
		pthread_mutex_unlock(&manager_lock);
		return buffer->object == NULL ? -EAGAIN : -EINVAL;
	}
	acquire = patch.acquire;
	object = buffer->object;
	handle = buffer->handle;
	pthread_mutex_unlock(&manager_lock);
	result = acquire(handle, PHX_BUFFER_RELEASE, object);
	if (result != PHX_STATUS_OK)
		return -EIO;
	pthread_mutex_lock(&manager_lock);
	if (buffer->object == object)
		buffer->object = NULL;
	pthread_mutex_unlock(&manager_lock);
	return 0;
}

int phoenix_queue_flush(struct phoenix_queue *queue)
{
	struct phoenix_queue_buffer *buffer;
	int first_error = 0;

	if (queue == NULL)
		return -EINVAL;
	for (;;) {
		pthread_mutex_lock(&manager_lock);
		for (buffer = queue->buffers; buffer != NULL; buffer = buffer->next)
			if (buffer->object != NULL)
				break;
		pthread_mutex_unlock(&manager_lock);
		if (buffer == NULL)
			break;
		if (phoenix_queue_requeue(queue, buffer) < 0) {
			first_error = -EIO;
			pthread_mutex_lock(&manager_lock);
			buffer->object = NULL;
			pthread_mutex_unlock(&manager_lock);
		}
	}
	pthread_mutex_lock(&manager_lock);
	queue->current = NULL;
	pthread_mutex_unlock(&manager_lock);
	return first_error;
}

bool phoenix_queue_is_associated(const struct phoenix_queue *queue)
{
	bool associated;

	if (queue == NULL)
		return false;
	pthread_mutex_lock(&manager_lock);
	associated = active_queue == queue && queue->handle != 0;
	pthread_mutex_unlock(&manager_lock);
	return associated;
}

#ifdef PHOENIX_QUEUE_TESTING
int phoenix_queue_create_for_test(struct phoenix_queue **queue,
		phoenix_acquire_func_t acquire,
		phoenix_buffer_parameter_get_func_t parameter_get)
{
	if (acquire == NULL || parameter_get == NULL)
		return -EINVAL;
	return create_queue(queue, acquire, parameter_get, true);
}

int phoenix_queue_dispatch_for_test(uintptr_t handle, uint32_t command,
		void *parameter)
{
	return phoenix_acquire_hook(handle, command, parameter);
}
#endif

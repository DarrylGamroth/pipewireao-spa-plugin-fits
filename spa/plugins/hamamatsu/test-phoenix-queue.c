/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>

#include "phoenix-queue.h"

#define PHX_BUFFER_GET UINT32_C(0xc0030600)
#define PHX_BUFFER_RELEASE UINT32_C(0xc0030700)
#define PHX_USER_LOCK UINT32_C(0xc0031000)
#define PHX_USER_UNLOCK UINT32_C(0xc0031100)
#define PHX_BUFFER_OBJECT_GET UINT32_C(0xc0031200)

struct user_buffer {
	void *memory;
	size_t size;
};

struct fake_object {
	void *memory;
	uint32_t releases;
};

static struct fake_object objects[2];
static uint32_t next_object;
static uint32_t passthrough_gets;

static int fake_acquire(uintptr_t handle SPA_UNUSED, uint32_t command,
		void *parameter)
{
	if (command == PHX_USER_LOCK || command == PHX_USER_UNLOCK)
		return 0;
	if (command == PHX_BUFFER_OBJECT_GET) {
		if (parameter == NULL || next_object >= SPA_N_ELEMENTS(objects))
			return 8;
		*(void **)parameter = &objects[next_object++];
		return 0;
	}
	if (command == PHX_BUFFER_RELEASE) {
		struct fake_object *object = parameter;

		if (object == NULL)
			return 8;
		object->releases++;
		return 0;
	}
	if (command == PHX_BUFFER_GET) {
		passthrough_gets++;
		return 0;
	}
	return 8;
}

static int fake_buffer_parameter_get(uintptr_t handle SPA_UNUSED,
		void *buffer, uint32_t parameter, void *value)
{
	struct fake_object *object = buffer;

	if (object == NULL || parameter != 0 || value == NULL)
		return 8;
	*(void **)value = object->memory;
	return 0;
}

static void test_live_patch(const char *module_path)
{
	struct phoenix_queue *queue = NULL;
	void *module;
	uint32_t i;

	module = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(module != NULL);
	/* Repeat the lifecycle to verify that the import is restored. */
	for (i = 0; i < 2; i++) {
		spa_assert_se(phoenix_queue_create(&queue) == 0);
		phoenix_queue_destroy(queue);
		queue = NULL;
	}
	spa_assert_se(dlclose(module) == 0);
}

int main(int argc, char *argv[])
{
	_Alignas(8) uint8_t memory[2][64] = { 0 };
	struct user_buffer user[2];
	struct phoenix_queue_buffer *buffers[2] = { NULL, NULL };
	struct phoenix_queue *queue = NULL;
	struct user_buffer result = { 0 };
	const uintptr_t handle = (uintptr_t) 0x1234u;
	uint32_t i;

	spa_assert_se(argc == 1 || argc == 2);
	if (argc == 2)
		test_live_patch(argv[1]);
	objects[0].memory = memory[0];
	objects[1].memory = memory[1];
	spa_assert_se(phoenix_queue_create_for_test(&queue, fake_acquire,
			fake_buffer_parameter_get) == 0);
	for (i = 0; i < SPA_N_ELEMENTS(buffers); i++) {
		user[i] = (struct user_buffer) {
			.memory = memory[i],
			.size = sizeof(memory[i]),
		};
		spa_assert_se(phoenix_queue_register(queue, memory[i],
				sizeof(memory[i]), &buffers[i]) == 0);
		spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_USER_LOCK,
				&user[i]) == 0);
	}
	spa_assert_se(phoenix_queue_is_associated(queue));
	spa_assert_se(phoenix_queue_requeue(queue, buffers[0]) == -EAGAIN);

	spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_BUFFER_GET,
			&result) == 0);
	spa_assert_se(result.memory == memory[0] && result.size == sizeof(memory[0]));
	spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_BUFFER_RELEASE,
			NULL) == 0);
	spa_assert_se(objects[0].releases == 0);
	spa_assert_se(phoenix_queue_requeue(queue, buffers[0]) == 0);
	spa_assert_se(objects[0].releases == 1);

	spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_BUFFER_GET,
			&result) == 0);
	spa_assert_se(result.memory == memory[1] && result.size == sizeof(memory[1]));
	spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_BUFFER_RELEASE,
			NULL) == 0);
	spa_assert_se(objects[1].releases == 0);
	spa_assert_se(phoenix_queue_flush(queue) == 0);
	spa_assert_se(objects[1].releases == 1);

	spa_assert_se(phoenix_queue_dispatch_for_test((uintptr_t) 0x5678u,
			PHX_BUFFER_GET, &result) == 0);
	spa_assert_se(passthrough_gets == 1);
	for (i = 0; i < SPA_N_ELEMENTS(buffers); i++) {
		spa_assert_se(phoenix_queue_dispatch_for_test(handle, PHX_USER_UNLOCK,
				&user[i]) == 0);
		spa_assert_se(phoenix_queue_unregister(queue, &buffers[i]) == 0);
	}
	phoenix_queue_destroy(queue);
	return 0;
}

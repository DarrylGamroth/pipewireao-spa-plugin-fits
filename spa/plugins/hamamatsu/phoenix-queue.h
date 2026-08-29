/* SPDX-License-Identifier: MIT */
#ifndef SPA_HAMAMATSU_PHOENIX_QUEUE_H
#define SPA_HAMAMATSU_PHOENIX_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct phoenix_queue;
struct phoenix_queue_buffer;

int phoenix_queue_create(struct phoenix_queue **queue);
void phoenix_queue_destroy(struct phoenix_queue *queue);

int phoenix_queue_register(struct phoenix_queue *queue, void *memory,
		size_t size, struct phoenix_queue_buffer **buffer);
int phoenix_queue_unregister(struct phoenix_queue *queue,
		struct phoenix_queue_buffer **buffer);
int phoenix_queue_requeue(struct phoenix_queue *queue,
		struct phoenix_queue_buffer *buffer);
int phoenix_queue_flush(struct phoenix_queue *queue);
bool phoenix_queue_is_associated(const struct phoenix_queue *queue);

#ifdef PHOENIX_QUEUE_TESTING
typedef int (*phoenix_acquire_func_t)(uintptr_t handle, uint32_t command,
		void *parameter);
typedef int (*phoenix_buffer_parameter_get_func_t)(uintptr_t handle,
		void *buffer, uint32_t parameter, void *value);

int phoenix_queue_create_for_test(struct phoenix_queue **queue,
		phoenix_acquire_func_t acquire,
		phoenix_buffer_parameter_get_func_t buffer_parameter_get);
int phoenix_queue_dispatch_for_test(uintptr_t handle, uint32_t command,
		void *parameter);
#endif

#endif

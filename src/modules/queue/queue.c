/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <errno.h>
#include <stddef.h>

int pwao_queue_ring_init(struct pwao_queue_ring *ring, uint32_t capacity)
{
	uint32_t i;

	if (ring == NULL || capacity == 0 ||
			capacity > PWAO_QUEUE_RING_MAX_SLOTS)
		return -EINVAL;
	ring->capacity = capacity;
	atomic_init(&ring->read_index, 0);
	atomic_init(&ring->write_index, 0);
	for (i = 0; i < PWAO_QUEUE_RING_MAX_SLOTS; i++)
		atomic_init(&ring->slots[i], UINT32_MAX);
	return 0;
}

int pwao_queue_ring_try_push(struct pwao_queue_ring *ring, uint32_t value)
{
	uint32_t read_index, write_index;

	if (ring == NULL || ring->capacity == 0 || value == UINT32_MAX)
		return -EINVAL;
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_relaxed);
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	if (write_index - read_index >= ring->capacity)
		return 0;
	atomic_store_explicit(&ring->slots[write_index % ring->capacity], value,
			memory_order_relaxed);
	atomic_store_explicit(&ring->write_index, write_index + 1u,
			memory_order_release);
	return 1;
}

int pwao_queue_ring_try_pop(struct pwao_queue_ring *ring, uint32_t *value)
{
	uint32_t attempt;

	if (ring == NULL || value == NULL || ring->capacity == 0)
		return -EINVAL;
	*value = UINT32_MAX;
	for (attempt = 0; attempt < 2; attempt++) {
		uint32_t read_index = atomic_load_explicit(&ring->read_index,
				memory_order_relaxed);
		const uint32_t write_index = atomic_load_explicit(
				&ring->write_index, memory_order_acquire);
		uint32_t expected, candidate;

		if (read_index == write_index)
			return 0;
		candidate = atomic_load_explicit(
				&ring->slots[read_index % ring->capacity],
				memory_order_relaxed);
		expected = read_index;
		if (atomic_compare_exchange_strong_explicit(&ring->read_index,
				&expected, read_index + 1u,
				memory_order_acq_rel, memory_order_relaxed)) {
			*value = candidate;
			return 1;
		}
	}
	return 0;
}

int pwao_queue_ring_drop_oldest_push(struct pwao_queue_ring *ring,
		uint32_t value, uint32_t *dropped_value)
{
	uint32_t read_index, write_index;

	if (ring == NULL || dropped_value == NULL || ring->capacity == 0 ||
			value == UINT32_MAX)
		return -EINVAL;
	*dropped_value = UINT32_MAX;
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_relaxed);
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	if (write_index - read_index >= ring->capacity) {
		const uint32_t candidate = atomic_load_explicit(
				&ring->slots[read_index % ring->capacity],
				memory_order_relaxed);
		uint32_t expected = read_index;

		if (atomic_compare_exchange_strong_explicit(&ring->read_index,
				&expected, read_index + 1u,
				memory_order_acq_rel, memory_order_relaxed))
			*dropped_value = candidate;
	}
	atomic_store_explicit(&ring->slots[write_index % ring->capacity], value,
			memory_order_relaxed);
	atomic_store_explicit(&ring->write_index, write_index + 1u,
			memory_order_release);
	return 1;
}

int pwao_queue_ring_admit(struct pwao_queue_ring *ring, uint32_t value,
		enum pwao_queue_overflow overflow, uint32_t *released_value)
{
	int result;

	if (released_value == NULL)
		return -EINVAL;
	if (overflow != PWAO_QUEUE_OVERFLOW_BACKPRESSURE &&
			overflow != PWAO_QUEUE_OVERFLOW_DROP_OLDEST &&
			overflow != PWAO_QUEUE_OVERFLOW_DROP_NEWEST)
		return -EINVAL;
	*released_value = UINT32_MAX;
	result = pwao_queue_ring_try_push(ring, value);
	if (result != 0)
		return result < 0 ? result : PWAO_QUEUE_ADMIT_QUEUED;
	switch (overflow) {
	case PWAO_QUEUE_OVERFLOW_BACKPRESSURE:
		return PWAO_QUEUE_ADMIT_BACKPRESSURE;
	case PWAO_QUEUE_OVERFLOW_DROP_NEWEST:
		*released_value = value;
		return PWAO_QUEUE_ADMIT_DROPPED;
	case PWAO_QUEUE_OVERFLOW_DROP_OLDEST:
		if (pwao_queue_ring_drop_oldest_push(ring, value,
				released_value) < 0)
			return -EINVAL;
		return *released_value == UINT32_MAX ?
				PWAO_QUEUE_ADMIT_QUEUED : PWAO_QUEUE_ADMIT_REPLACED;
	default:
		return -EINVAL;
	}
}

uint32_t pwao_queue_ring_size(const struct pwao_queue_ring *ring)
{
	uint32_t read_index, write_index, size;

	if (ring == NULL || ring->capacity == 0)
		return 0;
	write_index = atomic_load_explicit(&ring->write_index,
			memory_order_acquire);
	read_index = atomic_load_explicit(&ring->read_index,
			memory_order_acquire);
	size = write_index - read_index;
	return size > ring->capacity ? ring->capacity : size;
}

void pwao_queue_ring_reset(struct pwao_queue_ring *ring)
{
	uint32_t i;

	if (ring == NULL)
		return;
	for (i = 0; i < PWAO_QUEUE_RING_MAX_SLOTS; i++)
		atomic_store_explicit(&ring->slots[i], UINT32_MAX,
				memory_order_relaxed);
	atomic_store_explicit(&ring->read_index, 0, memory_order_relaxed);
	atomic_store_explicit(&ring->write_index, 0, memory_order_relaxed);
}

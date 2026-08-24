/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_QUEUE_H
#define PIPEWIREAO_QUEUE_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include <spa/utils/defs.h>

#define PWAO_QUEUE_RING_MAX_SLOTS 64u

struct pwao_queue_ring {
	_Alignas(SPA_CACHE_LINE_SIZE) uint32_t capacity;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint32_t read_index;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint32_t write_index;
	_Alignas(SPA_CACHE_LINE_SIZE)
	_Atomic uint32_t slots[PWAO_QUEUE_RING_MAX_SLOTS];
};

_Static_assert(offsetof(struct pwao_queue_ring, read_index) %
		SPA_CACHE_LINE_SIZE == 0, "queue read index alignment");
_Static_assert(offsetof(struct pwao_queue_ring, write_index) %
		SPA_CACHE_LINE_SIZE == 0, "queue write index alignment");
_Static_assert(offsetof(struct pwao_queue_ring, slots) %
		SPA_CACHE_LINE_SIZE == 0, "queue slot alignment");
_Static_assert(sizeof(struct pwao_queue_ring) % SPA_CACHE_LINE_SIZE == 0,
		"queue ring size alignment");

enum pwao_queue_overflow {
	PWAO_QUEUE_OVERFLOW_BACKPRESSURE,
	PWAO_QUEUE_OVERFLOW_DROP_OLDEST,
	PWAO_QUEUE_OVERFLOW_DROP_NEWEST,
};

enum pwao_queue_admit_result {
	PWAO_QUEUE_ADMIT_QUEUED = 1,
	PWAO_QUEUE_ADMIT_REPLACED,
	PWAO_QUEUE_ADMIT_DROPPED,
	PWAO_QUEUE_ADMIT_BACKPRESSURE,
};

int pwao_queue_ring_init(struct pwao_queue_ring *ring, uint32_t capacity);

/* These functions are valid only for one producer and one consumer. */
int pwao_queue_ring_try_push(struct pwao_queue_ring *ring, uint32_t value);
int pwao_queue_ring_try_pop(struct pwao_queue_ring *ring, uint32_t *value);

/*
 * Admit value even at capacity. The producer and consumer race for ownership
 * of the oldest slot. dropped_value is UINT32_MAX when the consumer won and
 * no queued value was dropped by this call.
 */
int pwao_queue_ring_drop_oldest_push(struct pwao_queue_ring *ring,
		uint32_t value, uint32_t *dropped_value);

/*
 * Apply one overflow policy without waiting. released_value identifies the
 * displaced queued value or dropped arriving value, otherwise UINT32_MAX.
 */
int pwao_queue_ring_admit(struct pwao_queue_ring *ring, uint32_t value,
		enum pwao_queue_overflow overflow, uint32_t *released_value);

uint32_t pwao_queue_ring_size(const struct pwao_queue_ring *ring);

/* The caller must quiesce both ring endpoints before reset. */
void pwao_queue_ring_reset(struct pwao_queue_ring *ring);

#endif

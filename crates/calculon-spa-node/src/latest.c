/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <spa/node/buffer-latest.h>

#include <pipewireao-plugins/pod.h>

size_t calculon_spa_unwrap_fixed_pod(const struct spa_pod *source,
		void *storage, size_t size)
{
	struct spa_pod_builder builder;
	struct spa_pod *result;

	if (source == NULL || storage == NULL || size > UINT32_MAX)
		return 0;
	spa_pod_builder_init(&builder, storage, (uint32_t)size);
	result = pipewireao_pod_unwrap_fixed_choices(&builder, source);
	return result == NULL ? 0 : SPA_POD_SIZE(result);
}

struct spa_buffer_latest *calculon_spa_latest_new(uint32_t direction, void *data)
{
	return spa_buffer_latest_new(direction, data, NULL);
}

void calculon_spa_latest_destroy(struct spa_buffer_latest *latest)
{
	spa_buffer_latest_destroy(latest);
}

void calculon_spa_latest_set_buffers(struct spa_buffer_latest *latest,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	spa_buffer_latest_set_buffers(latest, buffers, n_buffers);
}

void calculon_spa_latest_clear_buffers(struct spa_buffer_latest *latest)
{
	spa_buffer_latest_clear_buffers(latest);
}

int calculon_spa_latest_set_io(struct spa_buffer_latest *latest, uint32_t id,
		void *data, size_t size)
{
	return spa_buffer_latest_set_io(latest, id, data, size);
}

bool calculon_spa_latest_has_links(const struct spa_buffer_latest *latest)
{
	return spa_buffer_latest_has_links(latest);
}

bool calculon_spa_latest_worker_is_active(const struct spa_buffer_latest *latest)
{
	return spa_buffer_latest_worker_is_active(latest);
}

int calculon_spa_latest_worker_begin(struct spa_buffer_latest *latest)
{
	return spa_buffer_latest_worker_begin(latest);
}

int calculon_spa_latest_worker_end(struct spa_buffer_latest *latest)
{
	return spa_buffer_latest_worker_end(latest);
}

int calculon_spa_latest_dequeue(struct spa_buffer_latest *latest,
		uint32_t *buffer_id)
{
	return spa_buffer_latest_dequeue(latest, buffer_id, NULL);
}

int calculon_spa_latest_queue(struct spa_buffer_latest *latest,
		uint32_t buffer_id)
{
	return spa_buffer_latest_queue(latest, buffer_id);
}

int calculon_spa_latest_return(struct spa_buffer_latest *latest,
		uint32_t buffer_id)
{
	return spa_buffer_latest_return(latest, buffer_id);
}

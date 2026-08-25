/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "buffer-transfer.h"
#include "queue.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>

#define MAX_QUEUE_BUFFERS 62u
#define MAX_POOL_BUFFERS (MAX_QUEUE_BUFFERS + 2u)
#define MAX_DATA_BLOCKS PWAO_QUEUE_MAX_DATA_BLOCKS
#define MAX_METAS PWAO_QUEUE_MAX_METAS
#define PARAM_BUFFER_SIZE 16384u
#define STATS_INTERVAL_NSEC SPA_NSEC_PER_SEC

enum storage_mode {
	STORAGE_COPY,
	STORAGE_LEASE,
};

enum slot_state {
	SLOT_FREE,
	SLOT_PENDING,
	SLOT_ACTIVE,
	SLOT_COMPLETING,
	SLOT_BLOCKED,
};

struct queue_slot {
	uint32_t index;
	struct pw_buffer *capture;
	struct pw_buffer *playback;
	_Atomic uint32_t state;
	bool output_available;
	bool output_in_flight;
	uint32_t delivered_input;
	int owned_fds[MAX_DATA_BLOCKS];
};

struct input_stats {
	_Atomic uint64_t publications;
	_Atomic uint64_t replacements;
	_Atomic uint64_t dropped_arrivals;
	_Atomic uint64_t backpressure;
};

struct output_stats {
	_Atomic uint64_t deliveries;
	_Atomic uint64_t completions;
	_Atomic uint64_t pool_exhaustions;
	_Atomic uint64_t protocol_errors;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct pw_core *core;
	bool disconnect_core;

	struct spa_hook module_listener;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct pw_stream *playback;
	struct spa_hook playback_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
	struct spa_pod *format;

	struct spa_source *stats_timer;
	struct pwao_queue_ring pending;
	struct pwao_queue_ring completions;
	struct queue_slot slots[MAX_POOL_BUFFERS];
	uint32_t n_capture_buffers;
	uint32_t n_capture_present;
	uint32_t n_playback_buffers;
	uint32_t n_playback_present;
	_Atomic uint32_t blocked_input;
	uint32_t active_outputs;
	uint32_t max_buffers;
	enum pwao_queue_overflow overflow;
	enum storage_mode storage;

	_Alignas(SPA_CACHE_LINE_SIZE) struct input_stats input_stats;
	_Alignas(SPA_CACHE_LINE_SIZE) struct output_stats output_stats;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint32_t fatal_error;
	bool destroying;
};

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "PipeWireAO contributors" },
	{ PW_KEY_MODULE_DESCRIPTION,
		"Create a bounded complete-buffer queue between graph contexts" },
	{ PW_KEY_MODULE_USAGE,
		"queue.max-buffers=<1..62> "
		"queue.overflow=<backpressure|drop-oldest|drop-newest> "
		"queue.storage=<copy|lease> "
		"( capture.props=<properties> ) "
		"( playback.props=<properties> )" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static void mark_protocol_error(struct impl *impl)
{
	atomic_fetch_add_explicit(&impl->output_stats.protocol_errors, 1,
			memory_order_relaxed);
	atomic_store_explicit(&impl->fatal_error, 1, memory_order_release);
}

static struct queue_slot *slot_from_buffer(struct pw_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->user_data;
}

static int return_capture_buffer(struct impl *impl, uint32_t index)
{
	struct queue_slot *slot;

	if (index >= impl->n_capture_buffers)
		return -EINVAL;
	slot = &impl->slots[index];
	atomic_store_explicit(&slot->state, SLOT_FREE, memory_order_release);
	if (slot->capture == NULL)
		return -EIO;
	return pw_stream_queue_buffer(impl->capture, slot->capture);
}

static void drain_completions(struct impl *impl)
{
	uint32_t count;

	for (count = 0; count < impl->n_capture_buffers; count++) {
		uint32_t index;
		int result = pwao_queue_ring_try_pop(&impl->completions, &index);

		if (result <= 0)
			break;
		if (index >= impl->n_capture_buffers ||
				atomic_load_explicit(&impl->slots[index].state,
					memory_order_acquire) != SLOT_COMPLETING ||
				return_capture_buffer(impl, index) < 0) {
			mark_protocol_error(impl);
			break;
		}
	}
}

static int publish_completion(struct impl *impl, uint32_t index)
{
	struct queue_slot *slot;
	uint32_t expected = SLOT_ACTIVE;

	if (index >= impl->n_capture_buffers)
		return -EINVAL;
	slot = &impl->slots[index];
	if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected,
			SLOT_COMPLETING, memory_order_acq_rel,
			memory_order_relaxed))
		return -EPROTO;
	if (pwao_queue_ring_try_push(&impl->completions, index) != 1)
		return -ENOSPC;
	atomic_fetch_add_explicit(&impl->output_stats.completions, 1,
			memory_order_relaxed);
	return 0;
}

static void capture_process(void *data)
{
	struct impl *impl = data;
	uint32_t count;
	uint32_t blocked_input;

	drain_completions(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0)
		return;

	blocked_input = atomic_load_explicit(&impl->blocked_input,
			memory_order_relaxed);
	if (blocked_input != UINT32_MAX) {
		struct queue_slot *slot = &impl->slots[blocked_input];

		if (pwao_queue_ring_try_push(&impl->pending,
				blocked_input) != 1)
			return;
		atomic_store_explicit(&slot->state, SLOT_PENDING,
				memory_order_release);
		atomic_store_explicit(&impl->blocked_input, UINT32_MAX,
				memory_order_release);
	}

	for (count = 0; count < impl->n_capture_buffers; count++) {
		struct pw_buffer *buffer = pw_stream_dequeue_buffer(impl->capture);
		struct queue_slot *slot;
		uint32_t expected = SLOT_FREE;
		uint32_t released;
		int result;

		if (buffer == NULL)
			break;
		slot = slot_from_buffer(buffer);
		if (slot == NULL || slot->index >= impl->n_capture_buffers ||
				!atomic_compare_exchange_strong_explicit(&slot->state,
					&expected, SLOT_PENDING,
					memory_order_acq_rel,
					memory_order_relaxed)) {
			mark_protocol_error(impl);
			return;
		}
		atomic_fetch_add_explicit(&impl->input_stats.publications, 1,
				memory_order_relaxed);
		result = pwao_queue_ring_admit(&impl->pending, slot->index,
				impl->overflow, &released);
		switch (result) {
		case PWAO_QUEUE_ADMIT_QUEUED:
			break;
		case PWAO_QUEUE_ADMIT_REPLACED:
			if (released >= impl->n_capture_buffers ||
					return_capture_buffer(impl, released) < 0) {
				mark_protocol_error(impl);
				return;
			}
			atomic_fetch_add_explicit(&impl->input_stats.replacements, 1,
					memory_order_relaxed);
			break;
		case PWAO_QUEUE_ADMIT_DROPPED:
			if (released != slot->index) {
				mark_protocol_error(impl);
				return;
			}
			if (return_capture_buffer(impl, slot->index) < 0) {
				mark_protocol_error(impl);
				return;
			}
			atomic_fetch_add_explicit(
					&impl->input_stats.dropped_arrivals, 1,
					memory_order_relaxed);
			break;
		case PWAO_QUEUE_ADMIT_BACKPRESSURE:
			atomic_store_explicit(&slot->state, SLOT_BLOCKED,
					memory_order_release);
			atomic_store_explicit(&impl->blocked_input, slot->index,
					memory_order_release);
			atomic_fetch_add_explicit(&impl->input_stats.backpressure,
					1, memory_order_relaxed);
			return;
		default:
			mark_protocol_error(impl);
			return;
		}
	}
}

static int transfer_buffer(struct impl *impl, uint32_t input_index,
		struct queue_slot *output_slot)
{
	struct spa_buffer *input, *output;

	if (input_index >= impl->n_capture_buffers || output_slot == NULL ||
			output_slot->playback == NULL)
		return -EINVAL;
	input = impl->slots[input_index].capture->buffer;
	output = output_slot->playback->buffer;
	return pwao_queue_buffer_transfer(input, output,
			impl->storage == STORAGE_COPY);
}

static struct queue_slot *find_copy_output(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_playback_buffers; i++)
		if (impl->slots[i].output_available)
			return &impl->slots[i];
	return NULL;
}

static int recover_backpressure(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	uint32_t blocked_input;

	(void)loop;
	(void)async;
	(void)seq;
	(void)data;
	(void)size;
	drain_completions(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0)
		return 0;
	blocked_input = atomic_load_explicit(&impl->blocked_input,
			memory_order_relaxed);
	if (blocked_input != UINT32_MAX &&
			pwao_queue_ring_try_push(&impl->pending, blocked_input) == 1) {
		atomic_store_explicit(&impl->slots[blocked_input].state,
				SLOT_PENDING, memory_order_release);
		atomic_store_explicit(&impl->blocked_input, UINT32_MAX,
				memory_order_release);
	}
	return 0;
}

static void request_backpressure_recovery(struct impl *impl)
{
	struct pw_loop *loop;

	if (impl->overflow != PWAO_QUEUE_OVERFLOW_BACKPRESSURE ||
			impl->capture == NULL ||
			atomic_load_explicit(&impl->blocked_input,
				memory_order_acquire) == UINT32_MAX)
		return;
	loop = pw_stream_get_data_loop(impl->capture);
	if (loop == NULL || pw_loop_invoke(loop, recover_backpressure, 1,
			NULL, 0, false, impl) < 0)
		mark_protocol_error(impl);
}

static void playback_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buffer;
	struct queue_slot *output_slot;
	uint32_t input_index;
	int result;

	while ((buffer = pw_stream_dequeue_buffer(impl->playback)) != NULL) {
		struct queue_slot *slot = slot_from_buffer(buffer);

		if (slot == NULL || slot->index >= impl->n_playback_buffers) {
			mark_protocol_error(impl);
			return;
		}
		if (slot->output_in_flight) {
			if (impl->storage == STORAGE_LEASE &&
					publish_completion(impl,
						slot->delivered_input) < 0) {
				mark_protocol_error(impl);
				return;
			}
			slot->output_in_flight = false;
			slot->delivered_input = UINT32_MAX;
			if (impl->active_outputs == 0) {
				mark_protocol_error(impl);
				return;
			}
			impl->active_outputs--;
		}
		slot->output_available = true;
	}

	if (impl->active_outputs != 0)
		return;
	if (impl->storage == STORAGE_COPY) {
		output_slot = find_copy_output(impl);
		if (output_slot == NULL) {
			atomic_fetch_add_explicit(
					&impl->output_stats.pool_exhaustions, 1,
					memory_order_relaxed);
			return;
		}
	} else {
		output_slot = NULL;
	}

	result = pwao_queue_ring_try_pop(&impl->pending, &input_index);
	if (result <= 0)
		return;
	request_backpressure_recovery(impl);
	if (input_index >= impl->n_capture_buffers ||
			atomic_exchange_explicit(&impl->slots[input_index].state,
				SLOT_ACTIVE, memory_order_acq_rel) != SLOT_PENDING) {
		mark_protocol_error(impl);
		return;
	}
	if (impl->storage == STORAGE_LEASE) {
		output_slot = &impl->slots[input_index];
		if (!output_slot->output_available) {
			mark_protocol_error(impl);
			return;
		}
	}
	if (transfer_buffer(impl, input_index, output_slot) < 0) {
		mark_protocol_error(impl);
		return;
	}
	if (impl->storage == STORAGE_COPY &&
			publish_completion(impl, input_index) < 0) {
		mark_protocol_error(impl);
		return;
	}
	output_slot->output_available = false;
	output_slot->output_in_flight = true;
	output_slot->delivered_input = impl->storage == STORAGE_LEASE ?
			input_index : UINT32_MAX;
	impl->active_outputs++;
	atomic_fetch_add_explicit(&impl->output_stats.deliveries, 1,
			memory_order_relaxed);
	if (pw_stream_queue_buffer(impl->playback,
			output_slot->playback) < 0)
		mark_protocol_error(impl);
}

static void reset_ownership_quiescent(struct impl *impl,
		bool return_capture)
{
	uint32_t i;

	for (i = 0; i < impl->n_capture_buffers; i++) {
		struct queue_slot *slot = &impl->slots[i];
		uint32_t state = atomic_exchange_explicit(&slot->state,
				SLOT_FREE, memory_order_acq_rel);

		if (return_capture && state != SLOT_FREE && slot->capture != NULL)
			(void)pw_stream_queue_buffer(impl->capture, slot->capture);
	}
	pwao_queue_ring_reset(&impl->pending);
	pwao_queue_ring_reset(&impl->completions);
	atomic_store_explicit(&impl->blocked_input, UINT32_MAX,
			memory_order_relaxed);
	impl->active_outputs = 0;
	for (i = 0; i < impl->n_playback_buffers; i++) {
		impl->slots[i].output_available = false;
		impl->slots[i].output_in_flight = false;
		impl->slots[i].delivered_input = UINT32_MAX;
	}
}

static void release_all_quiescent(struct impl *impl)
{
	reset_ownership_quiescent(impl, true);
}

static int validate_capture_pool(struct impl *impl)
{
	struct spa_buffer *sample;
	uint32_t i, n_datas;

	if (impl->n_capture_buffers < impl->max_buffers + 2u ||
			impl->n_capture_buffers > MAX_POOL_BUFFERS ||
			impl->n_capture_present != impl->n_capture_buffers)
		return -ENOSPC;
	if (impl->slots[0].capture == NULL)
		return -EINVAL;
	sample = impl->slots[0].capture->buffer;
	n_datas = sample->n_datas;
	if (n_datas == 0 || n_datas > MAX_DATA_BLOCKS ||
			sample->n_metas > MAX_METAS)
		return -EINVAL;
	for (i = 0; i < sample->n_metas; i++)
		if (sample->metas[i].type == SPA_META_SyncTimeline)
			return -ENOTSUP;
	for (i = 0; i < impl->n_capture_buffers; i++) {
		struct spa_buffer *buffer;
		uint32_t j;

		if (impl->slots[i].capture == NULL)
			return -EINVAL;
		buffer = impl->slots[i].capture->buffer;
		if (buffer->n_datas != n_datas ||
				buffer->n_metas != sample->n_metas)
			return -EINVAL;
		for (j = 0; j < sample->n_metas; j++) {
			const struct spa_meta *meta = &sample->metas[j];
			const struct spa_meta *candidate =
					spa_buffer_find_meta(buffer, meta->type);

			if (candidate == NULL || candidate->size != meta->size)
				return -EINVAL;
		}
		for (j = 0; j < n_datas; j++) {
			struct spa_data *data = &buffer->datas[j];

			if (data->maxsize != sample->datas[j].maxsize)
				return -EINVAL;
			if (impl->storage == STORAGE_COPY && data->data == NULL)
				return -ENOTSUP;
			if (impl->storage == STORAGE_LEASE &&
					(data->type >= 32 || data->fd < 0 ||
					 (data->type != SPA_DATA_MemFd &&
					  data->type != SPA_DATA_DmaBuf)))
				return -ENOTSUP;
		}
	}
	return 0;
}

static int setup_playback(struct impl *impl);

static void capture_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct queue_slot *slot;
	uint32_t index;
	int result;

	for (index = 0; index < MAX_POOL_BUFFERS; index++)
		if (impl->slots[index].capture == NULL &&
				impl->slots[index].playback == NULL)
			break;
	if (index == MAX_POOL_BUFFERS) {
		mark_protocol_error(impl);
		return;
	}
	slot = &impl->slots[index];
	slot->index = index;
	slot->capture = buffer;
	slot->delivered_input = UINT32_MAX;
	atomic_store_explicit(&slot->state, SLOT_FREE, memory_order_relaxed);
	buffer->user_data = slot;
	impl->n_capture_present++;
	impl->n_capture_buffers = SPA_MAX(impl->n_capture_buffers, index + 1u);
	if (impl->playback == NULL && impl->format != NULL &&
			impl->n_capture_present >= impl->max_buffers + 2u &&
			pw_stream_get_state(impl->capture, NULL) ==
				PW_STREAM_STATE_PAUSED &&
			(result = setup_playback(impl)) < 0)
		(void)pw_stream_set_error(impl->capture, result,
				"queue output setup failed: %s", spa_strerror(result));
}

static void capture_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct queue_slot *slot = slot_from_buffer(buffer);

	if (slot == NULL)
		return;
	/* PipeWire can withdraw the capture pool before it emits Format=NULL.
	 * At this point the stream is already quiescent and the buffers are being
	 * revoked, so invalidate all queued ownership without trying to queue a
	 * buffer back into the pool that is currently being removed. The following
	 * format callback destroys playback and its aliases before a new pool is
	 * accepted. */
	reset_ownership_quiescent(impl, false);
	slot->capture = NULL;
	buffer->user_data = NULL;
	if (impl->n_capture_present == 0) {
		mark_protocol_error(impl);
		return;
	}
	impl->n_capture_present--;
	if (impl->n_capture_present == 0)
		impl->n_capture_buffers = 0;
}

static void playback_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct queue_slot *slot;
	uint32_t index;

	for (index = 0; index < impl->n_capture_buffers; index++)
		if (impl->slots[index].capture != NULL &&
				impl->slots[index].playback == NULL)
			break;
	if (index == impl->n_capture_buffers) {
		mark_protocol_error(impl);
		return;
	}
	slot = &impl->slots[index];
	slot->playback = buffer;
	slot->output_available = false;
	slot->output_in_flight = false;
	slot->delivered_input = UINT32_MAX;
	buffer->user_data = slot;
	impl->n_playback_present++;
	impl->n_playback_buffers = SPA_MAX(impl->n_playback_buffers, index + 1u);
	if (pwao_queue_buffer_validate_layout(slot->capture->buffer,
			buffer->buffer, impl->storage == STORAGE_COPY) < 0) {
		mark_protocol_error(impl);
		return;
	}
	if (impl->storage == STORAGE_LEASE &&
			pwao_queue_buffer_alias(slot->capture->buffer, buffer->buffer,
				slot->owned_fds, SPA_N_ELEMENTS(slot->owned_fds)) < 0)
		mark_protocol_error(impl);
}

static void playback_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct queue_slot *slot = slot_from_buffer(buffer);

	if (slot == NULL)
		return;
	if (slot->output_in_flight) {
		if (impl->storage == STORAGE_LEASE &&
				publish_completion(impl, slot->delivered_input) < 0)
			mark_protocol_error(impl);
		if (impl->active_outputs == 0)
			mark_protocol_error(impl);
		else
			impl->active_outputs--;
	}
	pwao_queue_buffer_close_fds(slot->owned_fds,
			SPA_N_ELEMENTS(slot->owned_fds));
	slot->playback = NULL;
	slot->output_available = false;
	slot->output_in_flight = false;
	slot->delivered_input = UINT32_MAX;
	buffer->user_data = NULL;
	if (impl->n_playback_present == 0) {
		mark_protocol_error(impl);
		return;
	}
	impl->n_playback_present--;
	if (impl->n_playback_present == 0)
		impl->n_playback_buffers = 0;
}

static int update_capture_params(struct impl *impl)
{
	uint8_t buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	struct spa_pod_frame acquisition;
	const struct spa_pod *params[3];
	uint32_t n_params = 0;

	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers,
			SPA_POD_Int((int32_t)(impl->max_buffers + 2u)));
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_header)));
	spa_pod_builder_push_object(&builder, &acquisition,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(&builder,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_acquisition)),
			0);
	spa_pod_builder_prop(&builder, SPA_PARAM_META_features,
			SPA_POD_PROP_FLAG_MANDATORY);
	spa_pod_builder_int(&builder, SPA_META_FEATURE_ACQUISITION_CURRENT);
	params[n_params++] = spa_pod_builder_pop(&builder, &acquisition);
	return pw_stream_update_params(impl->capture, params, n_params);
}

static void playback_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;

	(void)old;
	(void)error;
	if (state == PW_STREAM_STATE_ERROR && !impl->destroying)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = playback_state_changed,
	.process = playback_process,
	.add_buffer = playback_add_buffer,
	.remove_buffer = playback_remove_buffer,
};

static int setup_playback(struct impl *impl)
{
	uint8_t buffer[PARAM_BUFFER_SIZE];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	struct spa_pod_frame acquisition;
	const struct spa_pod *params[2 + MAX_METAS];
	struct spa_buffer *sample;
	uint32_t data_types, i, n_params = 0, size = 0;
	enum pw_stream_flags flags;
	int result;

	if (impl->playback != NULL)
		return 0;
	if ((result = validate_capture_pool(impl)) < 0)
		return result;
	sample = impl->slots[0].capture->buffer;
	for (i = 0; i < sample->n_datas; i++) {
		if (sample->datas[i].maxsize > INT32_MAX) {
			result = -EOVERFLOW;
			goto done;
		}
		size = SPA_MAX(size, sample->datas[i].maxsize);
	}
	data_types = impl->storage == STORAGE_LEASE ?
			((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_DmaBuf)) :
			((1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd));
	params[n_params++] = impl->format;
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers,
			SPA_POD_Int((int32_t)impl->n_capture_buffers),
			SPA_PARAM_BUFFERS_blocks,
			SPA_POD_Int((int32_t)sample->n_datas),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)size),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(data_types));
	for (i = 0; i < sample->n_metas; i++) {
		if (sample->metas[i].type == SPA_META_Busy ||
		    sample->metas[i].type >= SPA_META_START_features)
			continue;
		if (sample->metas[i].type == SPA_META_Acquisition) {
			spa_pod_builder_push_object(&builder, &acquisition,
					SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
			spa_pod_builder_add(&builder,
					SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int((int32_t)sample->metas[i].size),
					0);
			spa_pod_builder_prop(&builder, SPA_PARAM_META_features,
					SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_int(&builder,
					SPA_META_FEATURE_ACQUISITION_CURRENT);
			params[n_params++] = spa_pod_builder_pop(&builder,
					&acquisition);
			continue;
		}
		params[n_params++] = spa_pod_builder_add_object(&builder,
				SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
				SPA_PARAM_META_type,
				SPA_POD_Id(sample->metas[i].type),
				SPA_PARAM_META_size,
				SPA_POD_Int((int32_t)sample->metas[i].size));
	}

	impl->playback = pw_stream_new(impl->core, "queue output",
			pw_properties_copy(impl->playback_props));
	if (impl->playback == NULL) {
		result = -errno;
		goto done;
	}
	pw_stream_add_listener(impl->playback, &impl->playback_listener,
			&playback_events, impl);
	flags = PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT;
	if (impl->storage == STORAGE_LEASE)
		flags |= PW_STREAM_FLAG_ALLOC_BUFFERS;
	else
		flags |= PW_STREAM_FLAG_MAP_BUFFERS;
	result = pw_stream_connect(impl->playback, PW_DIRECTION_OUTPUT,
			PW_ID_ANY, flags, params, n_params);
done:
	return result;
}

static void capture_param_changed(void *data, uint32_t id,
		const struct spa_pod *param)
{
	struct impl *impl = data;
	uint32_t media_type, media_subtype;
	int result;

	if (id != SPA_PARAM_Format)
		return;
	if (param == NULL) {
		if (impl->playback != NULL) {
			pw_stream_set_active(impl->playback, false);
			pw_stream_flush(impl->playback, false);
			pw_stream_destroy(impl->playback);
			impl->playback = NULL;
		}
		release_all_quiescent(impl);
		free(impl->format);
		impl->format = NULL;
		return;
	}
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
			media_type != SPA_MEDIA_TYPE_application ||
			media_subtype != SPA_MEDIA_SUBTYPE_ndarray) {
		(void)pw_stream_set_error(impl->capture, -EINVAL,
				"queue requires application/ndarray");
		return;
	}
	free(impl->format);
	impl->format = spa_pod_copy(param);
	if (impl->format == NULL) {
		(void)pw_stream_set_error(impl->capture, -errno,
				"queue input format copy failed");
		return;
	}
	SPA_POD_OBJECT_ID(impl->format) = SPA_PARAM_EnumFormat;
	if ((result = update_capture_params(impl)) < 0)
		(void)pw_stream_set_error(impl->capture, result,
				"queue input format preparation failed: %s",
				spa_strerror(result));
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	int result;

	(void)error;
	if (state == PW_STREAM_STATE_ERROR ||
			state == PW_STREAM_STATE_UNCONNECTED) {
		if (!impl->destroying)
			pw_impl_module_schedule_destroy(impl->module);
		return;
	}
	if (state == PW_STREAM_STATE_PAUSED && old == PW_STREAM_STATE_STREAMING) {
		if (impl->playback != NULL) {
			(void)pw_stream_set_active(impl->playback, false);
			(void)pw_stream_flush(impl->playback, false);
		}
		release_all_quiescent(impl);
		return;
	}
	if (state == PW_STREAM_STATE_STREAMING && impl->playback != NULL) {
		(void)pw_stream_set_active(impl->playback, true);
		return;
	}
	if (state != PW_STREAM_STATE_PAUSED || impl->format == NULL ||
			impl->playback != NULL ||
			impl->n_capture_present < impl->max_buffers + 2u)
		return;
	result = setup_playback(impl);
	if (result < 0)
		(void)pw_stream_set_error(impl->capture, result,
				"queue output setup failed: %s", spa_strerror(result));
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = stream_state_changed,
	.param_changed = capture_param_changed,
	.add_buffer = capture_add_buffer,
	.remove_buffer = capture_remove_buffer,
	.process = capture_process,
};

static void core_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	if (!impl->destroying)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void core_error(void *data, uint32_t id, int seq, int result,
		const char *message)
{
	struct impl *impl = data;

	(void)seq;
	(void)message;
	if (id == PW_ID_CORE && result == -EPIPE && !impl->destroying)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void update_stats(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct spa_dict_item items[8];
	char values[8][32];
	uint32_t count = 0;

	(void)expirations;

#define ADD_COUNTER(key, field) do { \
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64, \
		atomic_load_explicit(&(field), memory_order_relaxed)); \
	items[count] = SPA_DICT_ITEM_INIT((key), values[count]); \
	count++; \
} while (0)
	ADD_COUNTER("queue.stats.publications", impl->input_stats.publications);
	ADD_COUNTER("queue.stats.replacements", impl->input_stats.replacements);
	ADD_COUNTER("queue.stats.dropped-arrivals",
			impl->input_stats.dropped_arrivals);
	ADD_COUNTER("queue.stats.backpressure", impl->input_stats.backpressure);
	ADD_COUNTER("queue.stats.deliveries", impl->output_stats.deliveries);
	ADD_COUNTER("queue.stats.completions", impl->output_stats.completions);
	ADD_COUNTER("queue.stats.pool-exhaustions",
			impl->output_stats.pool_exhaustions);
	ADD_COUNTER("queue.stats.protocol-errors",
			impl->output_stats.protocol_errors);
#undef ADD_COUNTER
	pw_impl_module_update_properties(impl->module,
			&SPA_DICT_INIT(items, count));
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0) {
		if (impl->capture != NULL)
			(void)pw_stream_set_error(impl->capture, -EPROTO,
					"queue ownership protocol failed");
		if (impl->playback != NULL)
			(void)pw_stream_set_error(impl->playback, -EPROTO,
					"queue ownership protocol failed");
	}
}

static void impl_destroy(struct impl *impl)
{
	uint32_t i;

	impl->destroying = true;
	if (impl->stats_timer != NULL) {
		pw_loop_destroy_source(pw_context_get_main_loop(impl->context),
				impl->stats_timer);
		impl->stats_timer = NULL;
	}
	if (impl->capture != NULL)
		(void)pw_stream_set_active(impl->capture, false);
	if (impl->playback != NULL)
		(void)pw_stream_set_active(impl->playback, false);
	if (impl->playback != NULL) {
		(void)pw_stream_flush(impl->playback, false);
		pw_stream_destroy(impl->playback);
		impl->playback = NULL;
	}
	if (impl->capture != NULL)
		release_all_quiescent(impl);
	if (impl->capture != NULL) {
		pw_stream_destroy(impl->capture);
		impl->capture = NULL;
	}
	if (impl->core != NULL) {
		struct pw_core *core = impl->core;

		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		impl->core = NULL;
		if (impl->disconnect_core)
			pw_core_disconnect(core);
	}
	for (i = 0; i < MAX_POOL_BUFFERS; i++)
		pwao_queue_buffer_close_fds(impl->slots[i].owned_fds,
				SPA_N_ELEMENTS(impl->slots[i].owned_fds));
	free(impl->format);
	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int parse_options(struct impl *impl, const struct pw_properties *props)
{
	const char *value;

	value = pw_properties_get(props, "queue.max-buffers");
	if (value == NULL || !spa_atou32(value, &impl->max_buffers, 10) ||
			impl->max_buffers == 0 ||
			impl->max_buffers > MAX_QUEUE_BUFFERS)
		return -EINVAL;
	value = pw_properties_get(props, "queue.overflow");
	if (value != NULL && spa_streq(value, "backpressure"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_BACKPRESSURE;
	else if (spa_streq(value, "drop-oldest"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_DROP_OLDEST;
	else if (spa_streq(value, "drop-newest"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_DROP_NEWEST;
	else
		return -EINVAL;
	value = pw_properties_get(props, "queue.storage");
	if (value != NULL && spa_streq(value, "copy"))
		impl->storage = STORAGE_COPY;
	else if (spa_streq(value, "lease"))
		impl->storage = STORAGE_LEASE;
	else
		return -EINVAL;
	return 0;
}

static int setup_properties(struct impl *impl, struct pw_properties *props,
		uint32_t id)
{
	const char *value, *name;
	uint32_t pid = (uint32_t)getpid();

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL)
		return -errno;
	if ((value = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, value,
				strlen(value));
	if ((value = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, value,
				strlen(value));
	name = pw_properties_get(props, PW_KEY_NODE_NAME);
	if (name == NULL)
		name = "queue";
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_NAME,
				"input.%s-%u-%u", name, pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_NAME,
				"output.%s-%u-%u", name, pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_GROUP,
				"queue.capture-%u-%u", pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_GROUP,
				"queue.playback-%u-%u", pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_LINK_GROUP,
				"queue.capture-link-%u-%u", pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_LINK_GROUP,
				"queue.playback-link-%u-%u", pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_MEDIA_CLASS,
				"Data/Sink");
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_MEDIA_CLASS,
				"Data/Source");
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_VIRTUAL, "true");
	return 0;
}

static int setup_capture(struct impl *impl)
{
	uint8_t buffer[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray));
	impl->capture = pw_stream_new(impl->core, "queue input",
			impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;
	pw_stream_add_listener(impl->capture, &impl->capture_listener,
			&capture_events, impl);
	return pw_stream_connect(impl->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT,
			params, SPA_N_ELEMENTS(params));
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl = NULL;
	const char *remote;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t i, j;
	int result;
	struct timespec interval = { .tv_sec = 1, .tv_nsec = 0 };

	result = posix_memalign((void **)&impl, SPA_CACHE_LINE_SIZE,
			sizeof(*impl));
	if (result != 0)
		return -result;
	memset(impl, 0, sizeof(*impl));
	impl->context = context;
	impl->module = module;
	atomic_init(&impl->blocked_input, UINT32_MAX);
	for (i = 0; i < MAX_POOL_BUFFERS; i++) {
		impl->slots[i].index = i;
		impl->slots[i].delivered_input = UINT32_MAX;
		for (j = 0; j < MAX_DATA_BLOCKS; j++)
			impl->slots[i].owned_fds[j] = -1;
	}
	props = args == NULL ? pw_properties_new(NULL, NULL) :
			pw_properties_new_string(args);
	if (props == NULL) {
		result = -errno;
		goto error;
	}
	if ((result = parse_options(impl, props)) < 0 ||
			(result = setup_properties(impl, props, id)) < 0 ||
			(result = pwao_queue_ring_init(&impl->pending,
				impl->max_buffers)) < 0 ||
			(result = pwao_queue_ring_init(&impl->completions,
				MAX_POOL_BUFFERS)) < 0)
		goto error;

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		remote = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(context,
				pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL), 0);
		impl->disconnect_core = true;
	}
	if (impl->core == NULL) {
		result = -errno;
		goto error;
	}
	pw_proxy_add_listener((struct pw_proxy *)impl->core,
			&impl->core_proxy_listener, &core_proxy_events, impl);
	pw_core_add_listener(impl->core, &impl->core_listener,
			&core_events, impl);
	if ((result = setup_capture(impl)) < 0)
		goto error;
	pw_impl_module_add_listener(module, &impl->module_listener,
			&module_events, impl);
	pw_impl_module_update_properties(module,
			&SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);
	update_stats(impl, 0);
	impl->stats_timer = pw_loop_add_timer(pw_context_get_main_loop(context),
			update_stats, impl);
	if (impl->stats_timer == NULL) {
		result = -errno;
		goto error_listener;
	}
	pw_loop_update_timer(pw_context_get_main_loop(context), impl->stats_timer,
			&interval, &interval, false);
	pw_properties_free(props);
	return 0;

error_listener:
	spa_hook_remove(&impl->module_listener);
error:
	pw_properties_free(props);
	impl_destroy(impl);
	return result;
}

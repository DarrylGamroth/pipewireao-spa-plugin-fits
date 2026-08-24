/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

struct data {
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	_Atomic int error;
};

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + now.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static void on_state_changed(void *user_data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *data = user_data;

	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "stream error: %s\n", error ? error : "unknown");
		atomic_store_explicit(&data->error, EIO, memory_order_release);
	}
	pw_thread_loop_signal(data->loop, false);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
};

static int connect_stream(struct data *data, const char *target, const char *name)
{
	uint8_t pod_buffer[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
			sizeof(pod_buffer));
	const struct spa_pod *params[3];
	uint64_t deadline;
	int res;

	params[0] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw));
	params[1] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[2] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_acquisition)));

	data->loop = pw_thread_loop_new("egrabber-host-client", NULL);
	if (data->loop == NULL)
		return -errno;
	data->stream = pw_stream_new_simple(pw_thread_loop_get_loop(data->loop),
			name,
			pw_properties_new(
				PW_KEY_NODE_NAME, name,
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Capture",
				PW_KEY_MEDIA_ROLE, "Camera",
				PW_KEY_TARGET_OBJECT, target,
				NULL),
			&stream_events, data);
	if (data->stream == NULL)
		return -errno;
	if ((res = pw_thread_loop_start(data->loop)) < 0)
		return res;
	pw_thread_loop_lock(data->loop);
	res = pw_stream_connect(data->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_NO_CONVERT |
			PW_STREAM_FLAG_BUFFER_LATEST,
			params, SPA_N_ELEMENTS(params));
	pw_thread_loop_unlock(data->loop);
	if (res < 0)
		return res;

	deadline = monotonic_nsec() + 5 * SPA_NSEC_PER_SEC;
	while (monotonic_nsec() < deadline) {
		struct timespec delay = { .tv_nsec = 1000000 };

		const int error = atomic_load_explicit(&data->error,
				memory_order_acquire);
		if (error != 0)
			return -error;
		res = pw_stream_buffer_latest_worker_begin(data->stream);
		if (res == 0)
			return 0;
		if (res != -ENOTCONN && res != -EBUSY)
			return res;
		nanosleep(&delay, NULL);
	}
	return -ETIMEDOUT;
}

static int receive_frames(struct data *data, uint32_t requested,
		uint32_t first_hold_msec)
{
	struct pw_stream_buffer_latest_poller poller =
			PW_STREAM_BUFFER_LATEST_POLLER_INIT;
	uint64_t first_sequence = 0, last_sequence = 0;
	uint64_t deadline = monotonic_nsec() + 10 * SPA_NSEC_PER_SEC;
	uint32_t received = 0;
	int res;

	if ((res = pw_stream_buffer_latest_poller_init(&poller, data->stream)) < 0) {
		fprintf(stderr, "poller init failed: %s\n", spa_strerror(res));
		return res;
	}
	while (received < requested && monotonic_nsec() < deadline) {
		struct pw_buffer *pw_buffer;
		struct spa_buffer *buffer;
		struct spa_meta_header *header;
		struct spa_meta *acquisition_meta;
		struct spa_meta_acquisition *acquisition;
		uint64_t retained_header_sequence;
		uint8_t retained_first_byte;
		uint64_t submission_sequence;

		res = pw_stream_buffer_latest_poller_try_dequeue(&poller,
				&pw_buffer, &submission_sequence);
		if (res == 0) {
			cpu_relax();
			continue;
		}
		if (res < 0) {
			fprintf(stderr, "poller dequeue failed: %s\n", spa_strerror(res));
			goto done;
		}
		buffer = pw_buffer->buffer;
		if (buffer->n_datas == 0 || buffer->datas[0].data == NULL ||
				buffer->datas[0].chunk == NULL ||
				buffer->datas[0].chunk->size == 0 ||
				buffer->datas[0].chunk->offset > buffer->datas[0].maxsize ||
				buffer->datas[0].chunk->size > buffer->datas[0].maxsize -
					buffer->datas[0].chunk->offset) {
			fprintf(stderr, "invalid payload buffer: datas=%u data=%p chunk=%p size=%u\n",
					buffer->n_datas,
					buffer->n_datas == 0 ? NULL : buffer->datas[0].data,
					buffer->n_datas == 0 ? NULL :
						(void *)buffer->datas[0].chunk,
					buffer->n_datas == 0 || buffer->datas[0].chunk == NULL ? 0 :
						buffer->datas[0].chunk->size);
			res = -EPROTO;
			goto return_buffer;
		}
		header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
				sizeof(*header));
		acquisition_meta = spa_buffer_find_meta(buffer, SPA_META_Acquisition);
		acquisition = acquisition_meta != NULL ? acquisition_meta->data : NULL;
		if (header == NULL || acquisition == NULL ||
				!spa_meta_acquisition_is_valid(acquisition_meta) ||
				(SPA_FLAG_IS_SET(acquisition->flags,
					SPA_META_ACQUISITION_FLAG_IDENTITY_VALID) &&
				 header->seq != acquisition->sequence) ||
				(received != 0 && header->seq <= last_sequence)) {
			fprintf(stderr, "invalid metadata: header=%p acquisition=%p"
					" valid=%d header-seq=%" PRIu64
					" acquisition-seq=%" PRIu64 " last=%" PRIu64 "\n",
					(void *)header, (void *)acquisition,
					spa_meta_acquisition_is_valid(acquisition_meta),
					header == NULL ? 0 : header->seq,
					acquisition == NULL ? 0 : acquisition->sequence,
					last_sequence);
			res = -EPROTO;
			goto return_buffer;
		}
		if (received == 0)
			first_sequence = header->seq;
		last_sequence = header->seq;
		if (received == 0 && first_hold_msec != 0) {
			struct timespec hold = {
				.tv_sec = first_hold_msec / 1000u,
				.tv_nsec = (long)(first_hold_msec % 1000u) * 1000000L,
			};

			retained_header_sequence = header->seq;
			retained_first_byte = ((const uint8_t *)buffer->datas[0].data)
					[buffer->datas[0].chunk->offset];
			printf("holding=%" PRIu64 " msec=%u\n",
					retained_header_sequence, first_hold_msec);
			fflush(stdout);
			nanosleep(&hold, NULL);
			if (header->seq != retained_header_sequence ||
					((const uint8_t *)buffer->datas[0].data)
						[buffer->datas[0].chunk->offset] != retained_first_byte) {
				fprintf(stderr, "retained payload changed during hold\n");
				res = -EPROTO;
				goto return_buffer;
			}
		}
		received++;
		res = 0;

return_buffer:
		if (pw_stream_queue_buffer(data->stream, pw_buffer) < 0 && res == 0)
			res = -EIO;
		if (res < 0)
			goto done;
		if (received < requested &&
				(res = pw_stream_buffer_latest_poller_init(
					&poller, data->stream)) < 0) {
			fprintf(stderr, "poller reset failed: %s\n", spa_strerror(res));
			goto done;
		}
	}
	res = received == requested ? 0 : -ETIMEDOUT;

done:
	pw_stream_buffer_latest_poller_clear(&poller);
	if (res == 0)
		printf("frames=%u first=%" PRIu64 " last=%" PRIu64 "\n",
				received, first_sequence, last_sequence);
	return res;
}

static void clear(struct data *data)
{
	if (data->stream != NULL) {
		pw_thread_loop_lock(data->loop);
		(void)pw_stream_buffer_latest_worker_end(data->stream);
		(void)pw_stream_disconnect(data->stream);
		pw_stream_destroy(data->stream);
		pw_thread_loop_unlock(data->loop);
	}
	if (data->loop != NULL) {
		pw_thread_loop_stop(data->loop);
		pw_thread_loop_destroy(data->loop);
	}
}

int main(int argc, char **argv)
{
	struct data data = { 0 };
	char *end;
	unsigned long parsed;
	unsigned long parsed_hold = 0;
	uint32_t frames;
	int res;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s TARGET_NODE FRAMES NODE_NAME [FIRST_HOLD_MSEC]\n",
				argv[0]);
		return 2;
	}
	errno = 0;
	parsed = strtoul(argv[2], &end, 10);
	if (errno != 0 || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
		fprintf(stderr, "FRAMES must be a positive 32-bit integer\n");
		return 2;
	}
	frames = (uint32_t)parsed;
	if (argc == 5) {
		errno = 0;
		parsed_hold = strtoul(argv[4], &end, 10);
		if (errno != 0 || *end != '\0' || parsed_hold > 10000) {
			fprintf(stderr, "FIRST_HOLD_MSEC must be at most 10000\n");
			return 2;
		}
	}
	pw_init(&argc, &argv);
	res = connect_stream(&data, argv[1], argv[3]);
	if (res < 0)
		fprintf(stderr, "stream connect failed: %s\n", spa_strerror(res));
	if (res == 0)
		res = receive_frames(&data, frames, (uint32_t)parsed_hold);
	if (res < 0)
		fprintf(stderr, "host capture failed: %s\n", spa_strerror(res));
	clear(&data);
	pw_deinit();
	return res < 0 ? 1 : 0;
}

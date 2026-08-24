/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "buffer-transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

int pwao_queue_buffer_validate_layout(const struct spa_buffer *input,
		const struct spa_buffer *output, bool require_output_capacity)
{
	uint32_t i;

	if (input == NULL || output == NULL || input->n_datas == 0 ||
			input->n_datas > PWAO_QUEUE_MAX_DATA_BLOCKS ||
			input->n_datas != output->n_datas ||
			input->n_metas > PWAO_QUEUE_MAX_METAS ||
			input->n_metas != output->n_metas)
		return -EINVAL;
	for (i = 0; i < input->n_metas; i++) {
		const struct spa_meta *in = &input->metas[i];
		const struct spa_meta *out = spa_buffer_find_meta(output, in->type);

		if (out == NULL || out->size != in->size ||
				(in->size > 0 && (in->data == NULL || out->data == NULL)))
			return -EINVAL;
	}
	if (require_output_capacity)
		for (i = 0; i < input->n_datas; i++)
			if (output->datas[i].maxsize < input->datas[i].maxsize)
				return -ENOSPC;
	return 0;
}

int pwao_queue_buffer_transfer(const struct spa_buffer *input,
		struct spa_buffer *output, bool copy_payload)
{
	uint32_t i;
	int result;

	result = pwao_queue_buffer_validate_layout(input, output, copy_payload);
	if (result < 0)
		return result;
	for (i = 0; i < input->n_metas; i++) {
		const struct spa_meta *in = &input->metas[i];
		struct spa_meta *out = spa_buffer_find_meta(output, in->type);

		if (in->size > 0)
			memcpy(out->data, in->data, in->size);
	}
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		struct spa_data *out = &output->datas[i];

		if (in->chunk == NULL || out->chunk == NULL ||
				in->chunk->offset > in->maxsize ||
				in->chunk->size > in->maxsize - in->chunk->offset)
			return -EINVAL;
		if (copy_payload) {
			if (in->data == NULL || out->data == NULL ||
					in->chunk->offset > out->maxsize ||
					in->chunk->size >
						out->maxsize - in->chunk->offset)
				return -EINVAL;
			memcpy(SPA_PTROFF(out->data, in->chunk->offset, void),
					SPA_PTROFF(in->data, in->chunk->offset, void),
					in->chunk->size);
		}
		*out->chunk = *in->chunk;
	}
	return 0;
}

void pwao_queue_buffer_close_fds(int *owned_fds, uint32_t n_owned_fds)
{
	uint32_t i;

	if (owned_fds == NULL)
		return;
	for (i = 0; i < n_owned_fds; i++) {
		if (owned_fds[i] >= 0)
			(void)close(owned_fds[i]);
		owned_fds[i] = -1;
	}
}

int pwao_queue_buffer_alias(const struct spa_buffer *input,
		struct spa_buffer *output, int *owned_fds, uint32_t n_owned_fds)
{
	uint32_t i;
	int result;

	if (owned_fds == NULL || input == NULL ||
			input->n_datas > n_owned_fds)
		return -EINVAL;
	result = pwao_queue_buffer_validate_layout(input, output, false);
	if (result < 0)
		return result;
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		const struct spa_data *out = &output->datas[i];

		if (in->type >= 32 || in->fd < 0 ||
				(in->type != SPA_DATA_MemFd &&
				 in->type != SPA_DATA_DmaBuf) ||
				(out->type & (1u << in->type)) == 0) {
			result = -ENOTSUP;
			goto error;
		}
		owned_fds[i] = fcntl(in->fd, F_DUPFD_CLOEXEC, 0);
		if (owned_fds[i] < 0) {
			result = -errno;
			goto error;
		}
	}
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		struct spa_data *out = &output->datas[i];

		out->type = in->type;
		out->flags = in->flags;
		out->fd = owned_fds[i];
		out->mapoffset = in->mapoffset;
		out->maxsize = in->maxsize;
		out->data = in->data;
	}
	return 0;

error:
	pwao_queue_buffer_close_fds(owned_fds, n_owned_fds);
	return result;
}

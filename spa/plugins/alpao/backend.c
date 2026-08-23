/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "backend.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_ALPAO_MOCK
struct mock_backend {
	struct alpao_backend backend;
	uint32_t actuator_count;
	uint32_t daq_frequency;
	uint64_t send_count;
	bool started;
};

static int mock_start(struct alpao_backend *backend, const char *serial,
		uint32_t actuator_count, uint32_t daq_frequency)
{
	struct mock_backend *mock = (struct mock_backend *)backend;

	(void)serial;
	mock->actuator_count = actuator_count;
	mock->daq_frequency = daq_frequency;
	mock->started = true;
	return 0;
}

static int mock_send(struct alpao_backend *backend, const double *command,
		size_t actuator_count)
{
	struct mock_backend *mock = (struct mock_backend *)backend;

	(void)command;
	if (!mock->started)
		return -EPIPE;
	if (actuator_count != mock->actuator_count)
		return -EINVAL;
	mock->send_count++;
	return 0;
}

static int mock_stop(struct alpao_backend *backend)
{
	struct mock_backend *mock = (struct mock_backend *)backend;

	mock->started = false;
	return 0;
}

static void mock_destroy(struct alpao_backend *backend)
{
	free(backend);
}

static const struct alpao_backend_methods mock_methods = {
	.start = mock_start,
	.send = mock_send,
	.stop = mock_stop,
	.destroy = mock_destroy,
};
#endif

int alpao_backend_new(const char *name, struct alpao_backend **backend)
{
	if (name == NULL || backend == NULL)
		return -EINVAL;
	*backend = NULL;
#ifdef HAVE_ALPAO_MOCK
	if (strcmp(name, "mock") == 0) {
		struct mock_backend *mock;

		mock = calloc(1, sizeof(*mock));
		if (mock == NULL)
			return -errno;
		mock->backend.methods = &mock_methods;
		*backend = &mock->backend;
		return 0;
	}
#endif
#ifdef HAVE_ALPAO_ASDK
	if (strcmp(name, "asdk") == 0)
		return alpao_asdk_backend_new(backend);
#endif
	return -ENOTSUP;
}

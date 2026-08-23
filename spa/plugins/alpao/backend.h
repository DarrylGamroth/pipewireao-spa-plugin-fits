/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_ALPAO_BACKEND_H
#define PIPEWIREAO_ALPAO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

struct alpao_backend;

struct alpao_backend_methods {
	int (*start)(struct alpao_backend *backend, const char *serial,
			uint32_t actuator_count, uint32_t daq_frequency);
	int (*send)(struct alpao_backend *backend, const double *command,
			size_t actuator_count);
	int (*stop)(struct alpao_backend *backend);
	void (*destroy)(struct alpao_backend *backend);
};

struct alpao_backend {
	const struct alpao_backend_methods *methods;
};

int alpao_backend_new(const char *name, struct alpao_backend **backend);

#ifdef HAVE_ALPAO_ASDK
int alpao_asdk_backend_new(struct alpao_backend **backend);
#endif

static inline int alpao_backend_start(struct alpao_backend *backend,
		const char *serial, uint32_t actuator_count, uint32_t daq_frequency)
{
	return backend->methods->start(backend, serial, actuator_count,
			daq_frequency);
}

static inline int alpao_backend_send(struct alpao_backend *backend,
		const double *command, size_t actuator_count)
{
	return backend->methods->send(backend, command, actuator_count);
}

static inline int alpao_backend_stop(struct alpao_backend *backend)
{
	return backend->methods->stop(backend);
}

static inline void alpao_backend_destroy(struct alpao_backend *backend)
{
	if (backend != NULL)
		backend->methods->destroy(backend);
}

#endif /* PIPEWIREAO_ALPAO_BACKEND_H */

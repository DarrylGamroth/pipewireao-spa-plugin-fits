/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "backend.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <asdkWrapper.h>

struct asdk_backend {
	struct alpao_backend backend;
	asdkDM *mirror;
};

static int asdk_start(struct alpao_backend *backend, const char *serial,
		uint32_t actuator_count)
{
	struct asdk_backend *asdk = (struct asdk_backend *)backend;
	Scalar reported = 0.0;

	if (asdk->mirror != NULL)
		return 0;
	if (serial == NULL || serial[0] == '\0')
		return -EINVAL;
	asdk->mirror = asdkInit(serial);
	if (asdk->mirror == NULL)
		return -ENODEV;
	if (asdkGet(asdk->mirror, "NbOfActuator", &reported) != SUCCESS ||
			!isfinite(reported) || reported < 1.0 || reported > UINT32_MAX ||
			floor(reported) != reported || (uint32_t)reported != actuator_count) {
		(void)asdkRelease(asdk->mirror);
		asdk->mirror = NULL;
		return -EINVAL;
	}
	if (asdkReset(asdk->mirror) != SUCCESS) {
		(void)asdkRelease(asdk->mirror);
		asdk->mirror = NULL;
		return -EIO;
	}
	return 0;
}

static int asdk_send(struct alpao_backend *backend, const double *command,
		size_t actuator_count)
{
	struct asdk_backend *asdk = (struct asdk_backend *)backend;

	(void)actuator_count;
	if (asdk->mirror == NULL)
		return -EPIPE;
	return asdkSend(asdk->mirror, command) == SUCCESS ? 0 : -EIO;
}

static int asdk_stop(struct alpao_backend *backend)
{
	struct asdk_backend *asdk = (struct asdk_backend *)backend;
	int result = 0;

	if (asdk->mirror == NULL)
		return 0;
	if (asdkReset(asdk->mirror) != SUCCESS)
		result = -EIO;
	if (asdkRelease(asdk->mirror) != SUCCESS && result == 0)
		result = -EIO;
	asdk->mirror = NULL;
	return result;
}

static void asdk_destroy(struct alpao_backend *backend)
{
	(void)asdk_stop(backend);
	free(backend);
}

static const struct alpao_backend_methods asdk_methods = {
	.start = asdk_start,
	.send = asdk_send,
	.stop = asdk_stop,
	.destroy = asdk_destroy,
};

int alpao_asdk_backend_new(struct alpao_backend **backend)
{
	struct asdk_backend *asdk;

	if (backend == NULL)
		return -EINVAL;
	*backend = NULL;
	asdk = calloc(1, sizeof(*asdk));
	if (asdk == NULL)
		return -errno;
	asdk->backend.methods = &asdk_methods;
	*backend = &asdk->backend;
	return 0;
}

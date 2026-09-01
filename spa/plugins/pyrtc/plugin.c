/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include "pyrtc.h"

static const struct spa_handle_factory *factories[] = {
	&spa_pyrtc_source_factory,
	&spa_pyrtc_sink_factory,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(factories))
		return 0;
	*factory = factories[(*index)++];
	return 1;
}

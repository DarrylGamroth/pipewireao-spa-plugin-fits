/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>

#include <pipewireao-plugins/alpao.h>

extern const struct spa_handle_factory spa_alpao_sink_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index != 0)
		return 0;
	*factory = &spa_alpao_sink_factory;
	(*index)++;
	return 1;
}

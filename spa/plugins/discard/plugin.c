/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>

#include <pipewireao-plugins/discard.h>

extern const struct spa_handle_factory spa_pipewireao_discard_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index != 0)
		return 0;
	*factory = &spa_pipewireao_discard_factory;
	(*index)++;
	return 1;
}

/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include "imagestreamio.h"

SPA_LOG_TOPIC_DEFINE(imagestreamio_log_topic, "spa.imagestreamio");
SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

static const struct spa_handle_factory *factories[] = {
	&spa_imagestreamio_source_factory,
	&spa_imagestreamio_sink_factory,
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

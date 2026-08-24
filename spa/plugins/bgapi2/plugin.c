/* SPDX-License-Identifier: MIT */
#include <errno.h>

#include "bgapi2.h"

SPA_LOG_TOPIC_DEFINE(bgapi2_log_topic, "spa.bgapi2");
SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	if (*index > 0)
		return 0;
	*factory = &spa_bgapi2_source_factory;
	(*index)++;
	return 1;
}

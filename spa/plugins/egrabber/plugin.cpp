/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#include "egrabber.hpp"

extern "C" {

SPA_LOG_TOPIC_DEFINE(egrabber_log_topic, "spa.egrabber");
SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_egrabber_manager_factory;
		break;
	case 1:
		*factory = &spa_egrabber_device_factory;
		break;
	case 2:
		*factory = &spa_egrabber_source_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

}

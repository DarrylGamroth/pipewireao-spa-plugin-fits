/* SPDX-License-Identifier: MIT */
#ifndef SPA_ARAVIS_H
#define SPA_ARAVIS_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_ARAVIS_SOURCE "api.aravis.source"
#define SPA_KEY_API_ARAVIS_DEVICE "api.aravis.device"

extern const struct spa_handle_factory spa_aravis_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &aravis_log_topic
extern struct spa_log_topic aravis_log_topic;

#endif

/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_H
#define SPA_BGAPI2_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_BGAPI2_SOURCE "api.bgapi2.source"

#define SPA_KEY_API_BGAPI2_PRODUCER "api.bgapi2.producer"
#define SPA_KEY_API_BGAPI2_INTERFACE_INDEX "api.bgapi2.interface-index"
#define SPA_KEY_API_BGAPI2_DEVICE_INDEX "api.bgapi2.device-index"
#define SPA_KEY_API_BGAPI2_STREAM_INDEX "api.bgapi2.stream-index"

extern const struct spa_handle_factory spa_bgapi2_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &bgapi2_log_topic
extern struct spa_log_topic bgapi2_log_topic;

#endif

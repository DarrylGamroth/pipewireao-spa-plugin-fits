/* SPDX-License-Identifier: MIT */
#ifndef SPA_BGAPI2_H
#define SPA_BGAPI2_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_BGAPI2_SOURCE "api.bgapi2.source"
#define SPA_NAME_API_BGAPI2_ENUM_MANAGER "api.bgapi2.enum.manager"
#define SPA_NAME_API_BGAPI2_DEVICE "api.bgapi2.device"

#define SPA_KEY_API_BGAPI2_PRODUCER "api.bgapi2.producer"
#define SPA_KEY_API_BGAPI2_SERIAL "api.bgapi2.serial"
#define SPA_KEY_API_BGAPI2_INTERFACE_INDEX "api.bgapi2.interface-index"
#define SPA_KEY_API_BGAPI2_DEVICE_INDEX "api.bgapi2.device-index"
#define SPA_KEY_API_BGAPI2_STREAM_INDEX "api.bgapi2.stream-index"
#define SPA_KEY_API_BGAPI2_TRANSPORT "api.bgapi2.transport"
#define SPA_KEY_API_BGAPI2_READINESS "api.bgapi2.readiness"

extern const struct spa_handle_factory spa_bgapi2_manager_factory;
extern const struct spa_handle_factory spa_bgapi2_device_factory;
extern const struct spa_handle_factory spa_bgapi2_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &bgapi2_log_topic
extern struct spa_log_topic bgapi2_log_topic;

#endif

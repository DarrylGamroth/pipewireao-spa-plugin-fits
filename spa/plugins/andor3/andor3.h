/* SPDX-License-Identifier: MIT */
#ifndef SPA_ANDOR3_H
#define SPA_ANDOR3_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_ANDOR3_SOURCE "api.andor3.source"

#define SPA_KEY_API_ANDOR3_DEVICE_INDEX "api.andor3.device-index"
#define SPA_KEY_API_ANDOR3_READINESS "api.andor3.readiness"
#define SPA_KEY_API_ANDOR3_PIXEL_ENCODING "api.andor3.pixel-encoding"

extern const struct spa_handle_factory spa_andor3_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &andor3_log_topic
extern struct spa_log_topic andor3_log_topic;

#endif

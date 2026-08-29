/* SPDX-License-Identifier: MIT */
#ifndef SPA_HAMAMATSU_H
#define SPA_HAMAMATSU_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_HAMAMATSU_SOURCE "api.hamamatsu.source"

#define SPA_KEY_API_HAMAMATSU_DEVICE_INDEX "api.hamamatsu.device-index"
#define SPA_KEY_API_HAMAMATSU_CAPTURE_MODE "api.hamamatsu.capture-mode"
#define SPA_KEY_API_HAMAMATSU_READINESS "api.hamamatsu.readiness"
#define SPA_KEY_API_HAMAMATSU_PIXEL_ENCODING "api.hamamatsu.pixel-encoding"

extern const struct spa_handle_factory spa_hamamatsu_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &hamamatsu_log_topic
extern struct spa_log_topic hamamatsu_log_topic;

#endif

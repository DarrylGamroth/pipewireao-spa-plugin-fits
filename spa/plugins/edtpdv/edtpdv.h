/* SPDX-License-Identifier: MIT */
#ifndef SPA_EDTPDV_H
#define SPA_EDTPDV_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_EDTPDV_SOURCE "api.edtpdv.source"

#define SPA_KEY_API_EDTPDV_DEVICE "api.edtpdv.device"
#define SPA_KEY_API_EDTPDV_UNIT "api.edtpdv.unit"
#define SPA_KEY_API_EDTPDV_CHANNEL "api.edtpdv.channel"
#define SPA_KEY_API_EDTPDV_RING_BUFFERS "api.edtpdv.ring-buffers"
#define SPA_KEY_API_EDTPDV_READINESS "api.edtpdv.readiness"

extern const struct spa_handle_factory spa_edtpdv_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &edtpdv_log_topic
extern struct spa_log_topic edtpdv_log_topic;

#endif

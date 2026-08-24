/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#pragma once

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define SPA_NAME_API_EGRABBER_ENUM_MANAGER "api.egrabber.enum.manager"
#define SPA_NAME_API_EGRABBER_DEVICE "api.egrabber.device"
#define SPA_NAME_API_EGRABBER_SOURCE "api.egrabber.source"

#define SPA_KEY_API_EGRABBER_PRODUCER "api.egrabber.producer"
#define SPA_KEY_API_EGRABBER_SERIAL "api.egrabber.serial"
#define SPA_KEY_API_EGRABBER_USER_ID "api.egrabber.user-id"
#define SPA_KEY_API_EGRABBER_INTERFACE_INDEX "api.egrabber.interface-index"
#define SPA_KEY_API_EGRABBER_DEVICE_INDEX "api.egrabber.device-index"
#define SPA_KEY_API_EGRABBER_STREAM_INDEX "api.egrabber.stream-index"
#define SPA_KEY_API_EGRABBER_BUFFER_COUNT "api.egrabber.buffer-count"
#define SPA_KEY_API_EGRABBER_CONTROL "api.egrabber.control"
#define SPA_KEY_API_EGRABBER_TRANSPORT "api.egrabber.transport"
#define SPA_KEY_API_EGRABBER_PROGRESSIVE "api.egrabber.progressive"
#define SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN "api.egrabber.acquisition-domain"
#define SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION "api.egrabber.acquisition-generation"
#define SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT "api.egrabber.acquisition-sequence-context"

extern "C" {

extern const struct spa_handle_factory spa_egrabber_manager_factory;
extern const struct spa_handle_factory spa_egrabber_device_factory;
extern const struct spa_handle_factory spa_egrabber_source_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &egrabber_log_topic
extern struct spa_log_topic egrabber_log_topic;

}

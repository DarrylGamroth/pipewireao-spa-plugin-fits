/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PWAO_IMAGESTREAMIO_H
#define PWAO_IMAGESTREAMIO_H

#include <spa/support/log.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/imagestreamio.h>

extern const struct spa_handle_factory spa_imagestreamio_source_factory;
extern const struct spa_handle_factory spa_imagestreamio_sink_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &imagestreamio_log_topic
extern struct spa_log_topic imagestreamio_log_topic;

#endif /* PWAO_IMAGESTREAMIO_H */

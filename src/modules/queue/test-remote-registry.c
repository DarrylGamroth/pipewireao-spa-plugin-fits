/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/impl.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct test_data;

struct node_watch {
	struct test_data *test;
	struct pw_node *node;
	struct spa_hook listener;
	uint32_t id;
	bool input;
	bool ports_valid;
};

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_impl_module *module;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	struct spa_source *timeout;
	bool timed_out;
	struct node_watch input;
	struct node_watch output;
	char input_queue_id[64];
	char output_queue_id[64];
};

static void maybe_complete(struct test_data *data)
{
	if (data->input.id != 0 && data->output.id != 0 &&
			data->input.ports_valid && data->output.ports_valid &&
			data->input_queue_id[0] != '\0' &&
			data->output_queue_id[0] != '\0')
		pw_main_loop_quit(data->main_loop);
}

static void on_node_info(void *user_data, const struct pw_node_info *info)
{
	struct node_watch *watch = user_data;
	struct test_data *data = watch->test;
	const char *queue_id;
	char *destination;
	size_t capacity;

	if (watch->input) {
		CHECK(info->n_input_ports == 1 && info->n_output_ports == 0);
	} else {
		CHECK(info->n_input_ports == 0 && info->n_output_ports == 1);
	}
	watch->ports_valid = true;
	if (info->props == NULL) {
		maybe_complete(data);
		return;
	}
	queue_id = spa_dict_lookup(info->props, PWAO_QUEUE_ID_PROPERTY);
	if (queue_id == NULL)
		return;
	destination = watch->input ? data->input_queue_id :
			data->output_queue_id;
	capacity = watch->input ? sizeof(data->input_queue_id) :
			sizeof(data->output_queue_id);
	CHECK(strlen(queue_id) < capacity);
	snprintf(destination, capacity, "%s", queue_id);
	maybe_complete(data);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static void on_global(void *user_data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct test_data *data = user_data;
	struct node_watch *watch;
	const char *name;

	(void)permissions;
	if (props == NULL || !spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;
	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (name == NULL)
		return;
	if (spa_streq(name, "test.remote-queue-input"))
		watch = &data->input;
	else if (spa_streq(name, "test.remote-queue-output"))
		watch = &data->output;
	else
		return;
	if (watch->node != NULL)
		return;
	watch->id = id;
	watch->node = pw_registry_bind(data->registry, id,
			PW_TYPE_INTERFACE_Node, SPA_MIN(version, PW_VERSION_NODE), 0);
	CHECK(watch->node != NULL);
	pw_node_add_listener(watch->node, &watch->listener, &node_events, watch);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

static void on_core_error(void *user_data, uint32_t id, int seq, int result,
		const char *message)
{
	(void)user_data;
	(void)id;
	(void)seq;
	fprintf(stderr, "remote core error %d: %s\n", result,
			message == NULL ? "unknown" : message);
	abort();
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_timeout(void *user_data, uint64_t expirations)
{
	struct test_data *data = user_data;

	(void)expirations;
	data->timed_out = true;
	pw_main_loop_quit(data->main_loop);
}

int main(int argc, char **argv)
{
	struct test_data data = { 0 };
	struct timespec timeout = { .tv_sec = 5 };
	struct pw_properties *properties;
	char module_args[512];
	int length, result;

	CHECK(argc == 2);
	pw_init(&argc, &argv);
	data.main_loop = pw_main_loop_new(NULL);
	CHECK(data.main_loop != NULL);
	data.context = pw_context_new(pw_main_loop_get_loop(data.main_loop),
			NULL, 0);
	CHECK(data.context != NULL);
	properties = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);
	CHECK(properties != NULL);
	data.core = pw_context_connect(data.context, properties, 0);
	CHECK(data.core != NULL);
	data.input.test = &data;
	data.input.input = true;
	data.output.test = &data;
	length = snprintf(module_args, sizeof(module_args),
			"remote.name=%s queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=lease "
			"queue.media=application/ndarray "
			"capture.props={ node.name=test.remote-queue-input } "
			"playback.props={ node.name=test.remote-queue-output }",
			argv[1]);
	CHECK(length >= 0 && (size_t)length < sizeof(module_args));
	data.module = pw_context_load_module(data.context,
			"libpipewire-module-queue", module_args, NULL);
	CHECK(data.module != NULL);
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	CHECK(data.registry != NULL);
	pw_registry_add_listener(data.registry, &data.registry_listener,
			&registry_events, &data);
	data.timeout = pw_loop_add_timer(pw_main_loop_get_loop(data.main_loop),
			on_timeout, &data);
	CHECK(data.timeout != NULL);
	CHECK(pw_loop_update_timer(pw_main_loop_get_loop(data.main_loop),
			data.timeout, &timeout, NULL, false) == 0);
	result = pw_main_loop_run(data.main_loop);
	CHECK(result >= 0 && !data.timed_out);
	CHECK(data.input.id != 0 && data.output.id != 0);
	CHECK(data.input.id != data.output.id);
	CHECK(data.input_queue_id[0] != '\0');
	CHECK(strcmp(data.input_queue_id, data.output_queue_id) == 0);

	pw_loop_destroy_source(pw_main_loop_get_loop(data.main_loop), data.timeout);
	spa_hook_remove(&data.input.listener);
	spa_hook_remove(&data.output.listener);
	pw_proxy_destroy((struct pw_proxy *)data.input.node);
	pw_proxy_destroy((struct pw_proxy *)data.output.node);
	pw_impl_module_destroy(data.module);
	pw_proxy_destroy((struct pw_proxy *)data.registry);
	pw_core_disconnect(data.core);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.main_loop);
	pw_deinit();
	return 0;
}

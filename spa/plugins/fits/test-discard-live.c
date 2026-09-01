/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <fitsio.h>
#include <pipewire/pipewire.h>
#include <spa/pod/parser.h>

#include <pipewireao-plugins/discard.h>
#include <pipewireao-plugins/fits.h>

#define IMAGE_WIDTH 4u
#define IMAGE_HEIGHT 3u
#define IMAGE_BYTES (IMAGE_WIDTH * IMAGE_HEIGHT * sizeof(uint16_t))
#define TIMEOUT_NSEC (5u * SPA_NSEC_PER_SEC)
#define METRIC_SEQUENCE 0x4644
#define SOURCE_NAME "test.fits-discard.source"
#define SINK_NAME "test.fits-discard.sink"
#define TEST_SCHEMA "org.pipewireao.test.fits-discard/1"

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct node_watch {
	struct pw_node *node;
	struct spa_hook listener;
	uint32_t global_id;
	uint32_t input_ports;
	uint32_t output_ports;
	bool info_seen;
	bool discard;
	bool metrics_seen;
	uint64_t buffers;
	uint64_t data_blocks;
	uint64_t bytes;
	uint64_t protocol_errors;
	uint64_t process_calls;
};

struct link_watch {
	struct pw_link *link;
	struct spa_hook listener;
	uint32_t global_id;
	enum pw_link_state state;
};

struct global_object {
	uint32_t id;
	bool visible;
};

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	int done_sequence;
	int sync_sequence;
	int core_errors;
	struct global_object objects[64];
	uint32_t n_objects;
	struct node_watch source;
	struct node_watch sink;
	struct link_watch link;
};

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static void iterate_main_loop(struct test_data *test)
{
	(void)pw_loop_iterate(pw_main_loop_get_loop(test->main_loop), 10);
}

static void on_core_done(void *data, uint32_t id, int sequence)
{
	struct test_data *test = data;

	(void)id;
	test->done_sequence = sequence;
}

static void on_core_error(void *data, uint32_t id, int sequence, int result,
		const char *message)
{
	struct test_data *test = data;

	(void)id;
	(void)sequence;
	test->core_errors++;
	fprintf(stderr, "private core error %d: %s\n", result,
			message == NULL ? "unknown" : message);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void sync_core(struct test_data *test)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	int sequence = pw_core_sync(test->core, PW_ID_CORE, test->sync_sequence);

	CHECK(sequence >= 0);
	test->sync_sequence = sequence;
	while (test->done_sequence != sequence && monotonic_nsec() < deadline)
		iterate_main_loop(test);
	CHECK(test->done_sequence == sequence);
	CHECK(test->core_errors == 0);
}

static void on_node_info(void *data, const struct pw_node_info *info)
{
	struct node_watch *watch = data;

	watch->input_ports = info->n_input_ports;
	watch->output_ports = info->n_output_ports;
	watch->info_seen = true;
}

static bool metric_value(int64_t value, uint64_t *destination)
{
	if (value < 0)
		return false;
	*destination = (uint64_t)value;
	return true;
}

static void on_node_param(void *data, int sequence, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct node_watch *watch = data;
	int64_t buffers = -1, data_blocks = -1, bytes = -1;
	int64_t protocol_errors = -1, process_calls = -1;

	(void)sequence;
	(void)index;
	(void)next;
	if (!watch->discard || id != SPA_PARAM_Props || param == NULL)
		return;
	if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS, SPA_POD_Long(&buffers),
			SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS,
			SPA_POD_Long(&data_blocks),
			SPA_PROP_PIPEWIREAO_DISCARD_BYTES, SPA_POD_Long(&bytes),
			SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
			SPA_POD_Long(&protocol_errors),
			SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS,
			SPA_POD_Long(&process_calls)) < 0)
		return;
	if (!metric_value(buffers, &watch->buffers) ||
			!metric_value(data_blocks, &watch->data_blocks) ||
			!metric_value(bytes, &watch->bytes) ||
			!metric_value(protocol_errors, &watch->protocol_errors) ||
			!metric_value(process_calls, &watch->process_calls))
		return;
	watch->metrics_seen = true;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
	.param = on_node_param,
};

static void on_link_info(void *data, const struct pw_link_info *info)
{
	struct link_watch *watch = data;

	watch->state = info->state;
	if (info->state == PW_LINK_STATE_ERROR)
		fprintf(stderr, "FITS-to-discard link error: %s\n",
				info->error == NULL ? "unknown" : info->error);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = on_link_info,
};

static void on_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct test_data *test = data;

	(void)permissions;
	(void)version;
	(void)props;
	if (!spa_streq(type, PW_TYPE_INTERFACE_Node) &&
			!spa_streq(type, PW_TYPE_INTERFACE_Link))
		return;
	CHECK(test->n_objects < SPA_N_ELEMENTS(test->objects));
	test->objects[test->n_objects++] = (struct global_object) {
		.id = id,
		.visible = true,
	};
}

static void on_global_remove(void *data, uint32_t id)
{
	struct test_data *test = data;
	uint32_t i;

	for (i = 0; i < test->n_objects; i++)
		if (test->objects[i].id == id)
			test->objects[i].visible = false;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
	.global_remove = on_global_remove,
};

static bool global_visible(const struct test_data *test, uint32_t id)
{
	uint32_t i;

	for (i = 0; i < test->n_objects; i++)
		if (test->objects[i].id == id)
			return test->objects[i].visible;
	return false;
}

static void create_node(struct test_data *test,
		struct node_watch *watch, struct pw_properties *properties)
{
	struct pw_node *node;

	CHECK(properties != NULL);
	node = pw_core_create_object(test->core, "spa-node-factory",
			PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
			&properties->dict, 0);
	pw_properties_free(properties);
	CHECK(node != NULL);
	watch->node = node;
	CHECK(pw_node_add_listener(node, &watch->listener, &node_events,
			watch) == 0);
}

static struct pw_properties *sink_properties(void)
{
	return pw_properties_new(
			SPA_KEY_FACTORY_NAME, SPA_NAME_API_PIPEWIREAO_DISCARD,
			PW_KEY_NODE_NAME, SINK_NAME,
			PW_KEY_NODE_DESCRIPTION, "Private FITS discard test sink",
			PW_KEY_NODE_VIRTUAL, "true",
			PW_KEY_NODE_WANT_DRIVER, "true",
			PW_KEY_OBJECT_LINGER, "false",
			NULL);
}

static struct pw_properties *source_properties(const char *path)
{
	return pw_properties_new(
			SPA_KEY_FACTORY_NAME, SPA_NAME_API_FITS_SOURCE,
			PW_KEY_NODE_NAME, SOURCE_NAME,
			PW_KEY_NODE_DESCRIPTION, "Private FITS discard test source",
			PW_KEY_NODE_VIRTUAL, "true",
			PW_KEY_OBJECT_LINGER, "false",
			SPA_KEY_API_FITS_PATH, path,
			SPA_KEY_API_FITS_SAMPLE_RANK, "2",
			SPA_KEY_API_FITS_RATE, "1/1",
			SPA_KEY_API_FITS_SCHEMA, TEST_SCHEMA,
			SPA_KEY_API_FITS_LOOP, "false",
			SPA_KEY_API_FITS_READINESS, "timerfd",
			SPA_KEY_API_FITS_OUTPUT_MODE, "frame",
			NULL);
}

static void create_link(struct test_data *test)
{
	struct pw_properties *properties = pw_properties_new(NULL, NULL);
	struct pw_link *link;

	CHECK(properties != NULL);
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u",
			test->source.global_id) >= 0);
	CHECK(pw_properties_set(properties, PW_KEY_LINK_OUTPUT_PORT, "0") >= 0);
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u",
			test->sink.global_id) >= 0);
	CHECK(pw_properties_set(properties, PW_KEY_LINK_INPUT_PORT, "0") >= 0);
	CHECK(pw_properties_set(properties, PW_KEY_OBJECT_LINGER, "false") >= 0);
	link = pw_core_create_object(test->core, "link-factory",
			PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
			&properties->dict, 0);
	pw_properties_free(properties);
	CHECK(link != NULL);
	test->link.link = link;
	test->link.state = PW_LINK_STATE_INIT;
	CHECK(pw_link_add_listener(link, &test->link.listener, &link_events,
			&test->link) == 0);
}

static void wait_for_nodes(struct test_data *test)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	test->source.global_id = pw_proxy_get_bound_id(
			(struct pw_proxy *)test->source.node);
	test->sink.global_id = pw_proxy_get_bound_id(
			(struct pw_proxy *)test->sink.node);
	CHECK(test->source.global_id != SPA_ID_INVALID);
	CHECK(test->sink.global_id != SPA_ID_INVALID);
	while ((!global_visible(test, test->source.global_id) ||
			!global_visible(test, test->sink.global_id) ||
			!test->source.info_seen || !test->sink.info_seen) &&
			monotonic_nsec() < deadline)
		iterate_main_loop(test);
	CHECK(global_visible(test, test->source.global_id));
	CHECK(global_visible(test, test->sink.global_id));
	CHECK(test->source.info_seen && test->sink.info_seen);
	CHECK(test->source.input_ports == 0 && test->source.output_ports == 1);
	CHECK(test->sink.input_ports == 1 && test->sink.output_ports == 0);
}

static void wait_for_active_link(struct test_data *test)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	test->link.global_id = pw_proxy_get_bound_id(
			(struct pw_proxy *)test->link.link);
	CHECK(test->link.global_id != SPA_ID_INVALID);
	while ((!global_visible(test, test->link.global_id) ||
			test->link.state != PW_LINK_STATE_ACTIVE) &&
			test->link.state != PW_LINK_STATE_ERROR &&
			monotonic_nsec() < deadline)
		iterate_main_loop(test);
	CHECK(global_visible(test, test->link.global_id));
	CHECK(test->link.state == PW_LINK_STATE_ACTIVE);
}

static void wait_for_discarded_image(struct test_data *test)
{
	struct node_watch *sink = &test->sink;
	const struct timespec retry_delay = { .tv_nsec = 10 * 1000 * 1000 };
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (sink->buffers == 0 && monotonic_nsec() < deadline) {
		sink->metrics_seen = false;
		CHECK(pw_node_enum_params(sink->node, METRIC_SEQUENCE,
				SPA_PARAM_Props, 0, 1, NULL) >= 0);
		sync_core(test);
		CHECK(sink->metrics_seen);
		if (sink->buffers == 0)
			CHECK(nanosleep(&retry_delay, NULL) == 0);
	}
	if (sink->buffers == 0) {
		fprintf(stderr,
				"discard metrics: buffers=%" PRIu64
				" blocks=%" PRIu64 " bytes=%" PRIu64
				" errors=%" PRIu64 " process-calls=%" PRIu64 "\n",
				sink->buffers, sink->data_blocks, sink->bytes,
				sink->protocol_errors, sink->process_calls);
	}
	CHECK(sink->buffers == 1);
	CHECK(sink->data_blocks == 1);
	CHECK(sink->bytes == IMAGE_BYTES);
	CHECK(sink->protocol_errors == 0);
	CHECK(sink->process_calls >= sink->buffers);
}

static void wait_for_owned_removal(struct test_data *test)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while ((global_visible(test, test->link.global_id) ||
			global_visible(test, test->source.global_id) ||
			global_visible(test, test->sink.global_id)) &&
			monotonic_nsec() < deadline)
		iterate_main_loop(test);
	CHECK(!global_visible(test, test->link.global_id));
	CHECK(!global_visible(test, test->source.global_id));
	CHECK(!global_visible(test, test->sink.global_id));
}

static void make_image(const char *path)
{
	uint16_t pixels[IMAGE_HEIGHT][IMAGE_WIDTH];
	LONGLONG axes[] = { IMAGE_WIDTH, IMAGE_HEIGHT };
	char create_path[512];
	fitsfile *file = NULL;
	int status = 0;
	uint32_t row, column;

	CHECK(snprintf(create_path, sizeof(create_path), "!%s", path) > 0);
	for (row = 0; row < IMAGE_HEIGHT; row++)
		for (column = 0; column < IMAGE_WIDTH; column++)
			pixels[row][column] = (uint16_t)(row * 100u + column);
	fits_create_file(&file, create_path, &status);
	fits_create_imgll(file, USHORT_IMG, SPA_N_ELEMENTS(axes), axes, &status);
	fits_write_img(file, TUSHORT, 1, IMAGE_WIDTH * IMAGE_HEIGHT,
			pixels, &status);
	fits_close_file(file, &status);
	CHECK(status == 0);
}

int main(int argc, char **argv)
{
	struct test_data test = { 0 };
	struct pw_properties *properties;

	CHECK(argc == 3);
	make_image(argv[2]);
	pw_init(&argc, &argv);
	test.main_loop = pw_main_loop_new(NULL);
	CHECK(test.main_loop != NULL);
	test.context = pw_context_new(pw_main_loop_get_loop(test.main_loop),
			NULL, 0);
	CHECK(test.context != NULL);
	properties = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);
	CHECK(properties != NULL);
	test.core = pw_context_connect(test.context, properties, 0);
	CHECK(test.core != NULL);
	pw_core_add_listener(test.core, &test.core_listener, &core_events, &test);
	test.registry = pw_core_get_registry(test.core, PW_VERSION_REGISTRY, 0);
	CHECK(test.registry != NULL);
	pw_registry_add_listener(test.registry, &test.registry_listener,
			&registry_events, &test);
	test.sink.discard = true;
	pw_loop_enter(pw_main_loop_get_loop(test.main_loop));
	sync_core(&test);
	create_node(&test, &test.sink, sink_properties());
	create_node(&test, &test.source, source_properties(argv[2]));
	sync_core(&test);
	wait_for_nodes(&test);
	create_link(&test);
	sync_core(&test);
	wait_for_active_link(&test);
	wait_for_discarded_image(&test);

	pw_proxy_destroy((struct pw_proxy *)test.link.link);
	sync_core(&test);
	pw_proxy_destroy((struct pw_proxy *)test.source.node);
	pw_proxy_destroy((struct pw_proxy *)test.sink.node);
	sync_core(&test);
	wait_for_owned_removal(&test);
	pw_proxy_destroy((struct pw_proxy *)test.registry);
	pw_core_disconnect(test.core);
	pw_loop_leave(pw_main_loop_get_loop(test.main_loop));
	pw_context_destroy(test.context);
	pw_main_loop_destroy(test.main_loop);
	pw_deinit();
	CHECK(unlink(argv[2]) == 0);
	return 0;
}

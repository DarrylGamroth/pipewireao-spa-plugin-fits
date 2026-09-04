/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/utils/defs.h>

#include <pipewireao-plugins/alpao.h>

#define PROFILE "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define CONFIG "{\"actuator_count\":3,\"command_scale\":2.0," \
	"\"profile\":\"" PROFILE "\",\"rate_numerator\":2000," \
	"\"rate_denominator\":1}"

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta meta;
	struct spa_meta_header header;
	uint8_t storage[3 * sizeof(double)];
};

static int unused_dense(void *data SPA_UNUSED,
		const struct spa_fgn_dense_f32_task *task SPA_UNUSED)
{
	return -ENOTSUP;
}

static const struct spa_fgn_executor executor = {
	.struct_size = sizeof(struct spa_fgn_executor),
	.version = SPA_FGN_EXECUTOR_VERSION,
	.n_lanes = 1,
	.run_dense_f32 = unused_dense,
};

static void init_buffer(struct test_buffer *buffer, uint32_t size)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->chunk.stride = 1;
	buffer->data.type = SPA_DATA_MemPtr;
	buffer->data.data = buffer->storage;
	buffer->data.maxsize = sizeof(buffer->storage);
	buffer->data.chunk = &buffer->chunk;
	buffer->meta.type = SPA_META_Header;
	buffer->meta.size = sizeof(buffer->header);
	buffer->meta.data = &buffer->header;
	buffer->buffer.n_metas = 1;
	buffer->buffer.metas = &buffer->meta;
	buffer->buffer.n_datas = 1;
	buffer->buffer.datas = &buffer->data;
	buffer->chunk.size = size;
}

static void expect_bad_config(const struct spa_fgn_descriptor *descriptor,
		const char *config)
{
	void *instance = (void *)(uintptr_t)1;
	int res;

	res = descriptor->instantiate(descriptor, config, &executor, &instance);
	if (res != -EINVAL)
		fprintf(stderr, "config unexpectedly returned %d: %s\n", res, config);
	assert(res == -EINVAL);
	assert(instance == (void *)(uintptr_t)1);
}

int main(int argc, char *argv[])
{
	spa_fgn_plugin_entry_func_t entry;
	const struct spa_fgn_descriptor *descriptor;
	const struct spa_fgn_plugin *plugin;
	const struct spa_fgn_format *input_format, *output_format;
	struct spa_fgn_buffer inputs[1], outputs[1];
	struct test_buffer input, output;
	float *physical;
	double *normalized;
	void *library, *instance = NULL;

	if (argc != 2) {
		fprintf(stderr, "usage: %s PLUGIN\n", argv[0]);
		return 2;
	}
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	assert(library != NULL);
	entry = (spa_fgn_plugin_entry_func_t)dlsym(library,
			SPA_FGN_PLUGIN_ENTRY_NAME);
	assert(entry != NULL);
	assert(entry(SPA_FGN_PLUGIN_ABI_VERSION - 1) == NULL);
	plugin = entry(SPA_FGN_PLUGIN_ABI_VERSION);
	assert(plugin != NULL);
	assert(plugin->struct_size == sizeof(*plugin));
	assert(plugin->abi_version == SPA_FGN_PLUGIN_ABI_VERSION);
	assert(strcmp(plugin->name, "alpao") == 0);
	assert(plugin->find_descriptor("unknown") == NULL);
	descriptor = plugin->find_descriptor(
			SPA_FGN_ALPAO_LABEL_COMMAND_NORMALIZATION);
	assert(descriptor != NULL);
	assert(descriptor->version == SPA_FGN_PLUGIN_ABI_VERSION);
	assert(descriptor->n_ports == 2);
	assert(descriptor->instantiate(descriptor, CONFIG, &executor,
			&instance) == 0);
	assert(instance != NULL);
	assert(descriptor->get_port_format(instance, 0, &input_format) == 0);
	assert(descriptor->get_port_format(instance, 1, &output_format) == 0);
	assert(input_format->element_type == SPA_ELEMENT_TYPE_F32_LE);
	assert(input_format->layout == SPA_NDARRAY_LAYOUT_ROW_MAJOR);
	assert(input_format->rate_num == 2000 && input_format->rate_denom == 1);
	assert(input_format->n_dimensions == 1 && input_format->shape[0] == 3);
	assert(strcmp(input_format->schema,
			SPA_ALPAO_SCHEMA_DEMANDED_PDM_COMMAND) == 0);
	assert(output_format->element_type == SPA_ELEMENT_TYPE_F64_LE);
	assert(output_format->rate_num == 0 && output_format->rate_denom == 0);
	assert(strcmp(output_format->schema,
			SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND) == 0);

	init_buffer(&input, 3 * sizeof(float));
	init_buffer(&output, 0);
	input.header.seq = 42;
	input.header.pts = 1234567;
	physical = (float *)input.storage;
	physical[0] = -2.0f;
	physical[1] = 0.0f;
	physical[2] = 2.0f;
	inputs[0] = (struct spa_fgn_buffer) {
		.buffer = &input.buffer,
		.format = input_format,
	};
	outputs[0] = (struct spa_fgn_buffer) {
		.buffer = &output.buffer,
		.format = output_format,
	};
	assert(descriptor->process(instance, inputs, 1, outputs, 1) == 0);
	normalized = (double *)output.storage;
	assert(normalized[0] == -1.0 && normalized[1] == 0.0 &&
			normalized[2] == 1.0);
	assert(output.chunk.size == 3 * sizeof(double));
	assert(output.header.seq == input.header.seq &&
			output.header.pts == input.header.pts);

	physical[2] = 2.1f;
	output.chunk.size = 0;
	assert(descriptor->process(instance, inputs, 1, outputs, 1) == -ERANGE);
	assert(output.chunk.size == 0);
	descriptor->cleanup(instance);

	expect_bad_config(descriptor, "{}");
	expect_bad_config(descriptor,
			"{\"actuator_count\":0,\"command_scale\":2.0,"
			"\"profile\":\"" PROFILE "\",\"rate_numerator\":2000,"
			"\"rate_denominator\":1}");
	expect_bad_config(descriptor,
			"{\"actuator_count\":3,\"command_scale\":0.0,"
			"\"profile\":\"" PROFILE "\",\"rate_numerator\":2000,"
			"\"rate_denominator\":1}");
	expect_bad_config(descriptor,
			"{\"actuator_count\":3,\"command_scale\":2.0,"
			"\"profile\":\"sha256:bad\",\"rate_numerator\":2000,"
			"\"rate_denominator\":1}");
	expect_bad_config(descriptor,
			"{\"actuator_count\":3,\"command_scale\":2.0,"
			"\"profile\":\"" PROFILE "\",\"rate_numerator\":2000,"
			"\"rate_denominator\":0}");
	expect_bad_config(descriptor,
			"{\"actuator_count\":3,\"command_scale\":2.0,"
			"\"profile\":\"" PROFILE "\",\"rate_numerator\":2000,"
			"\"rate_denominator\":1,"
			"\"unknown\":1}");

	assert(dlclose(library) == 0);
	return 0;
}

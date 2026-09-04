/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/filter-graph/ndarray-plugin.h>
#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/alpao.h>

#define PROFILE_DIGEST_CHARACTERS 64u
#define PROFILE_SIZE (sizeof("sha256:") - 1u + PROFILE_DIGEST_CHARACTERS + 1u)

struct normalization_instance {
	uint32_t shape[1];
	/* spa_json_parse_stringn() sizes against the quoted source token. */
	char profile[PROFILE_SIZE + 2u];
	struct spa_fgn_format formats[2];
	uint32_t actuator_count;
	double command_scale;
};

static const struct spa_fgn_port_info ports[] = {
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 0,
		.direction = SPA_DIRECTION_INPUT,
		.name = "demanded-command",
	},
	{
		.struct_size = sizeof(struct spa_fgn_port_info),
		.index = 1,
		.direction = SPA_DIRECTION_OUTPUT,
		.name = "normalized-command",
	},
};

static bool valid_profile(const char *profile)
{
	uint32_t i;

	if (profile == NULL || !spa_strstartswith(profile, "sha256:"))
		return false;
	profile += strlen("sha256:");
	if (strlen(profile) != PROFILE_DIGEST_CHARACTERS)
		return false;
	for (i = 0; i < PROFILE_DIGEST_CHARACTERS; i++)
		if (!((profile[i] >= '0' && profile[i] <= '9') ||
		      (profile[i] >= 'a' && profile[i] <= 'f')))
			return false;
	return true;
}

static int instantiate(const struct spa_fgn_descriptor *descriptor,
		const char *config, const struct spa_fgn_executor *executor,
		void **result)
{
	struct normalization_instance *instance;
	struct spa_json object;
	char key[64];
	const char *token;
	uint32_t rate_num = 0, rate_denom = 0;
	bool have_count = false, have_scale = false;
	bool have_profile = false, have_rate_num = false, have_rate_denom = false;
	int len, value, res = -EINVAL;
	float scale;

	if (descriptor == NULL || config == NULL || executor == NULL ||
	    executor->struct_size < sizeof(*executor) ||
	    executor->version != SPA_FGN_EXECUTOR_VERSION ||
	    executor->n_lanes == 0 || executor->run_dense_f32 == NULL ||
	    result == NULL)
		return -EINVAL;
	if ((instance = calloc(1, sizeof(*instance))) == NULL)
		return -ENOMEM;
	if (spa_json_begin_object(&object, config, strlen(config)) <= 0)
		goto error;
	while ((len = spa_json_object_next(&object, key, sizeof(key), &token)) > 0) {
		if (spa_streq(key, "actuator_count")) {
			if (have_count || spa_json_parse_int(token, len, &value) <= 0 ||
			    value <= 0 || value > INT32_MAX / (int)sizeof(double))
				goto error;
			instance->actuator_count = (uint32_t)value;
			have_count = true;
		} else if (spa_streq(key, "command_scale")) {
			if (have_scale || spa_json_parse_float(token, len, &scale) <= 0 ||
			    !isfinite(scale) || scale <= 0.0f)
				goto error;
			instance->command_scale = scale;
			have_scale = true;
		} else if (spa_streq(key, "profile")) {
			if (have_profile || spa_json_parse_stringn(token, len,
					instance->profile, sizeof(instance->profile)) <= 0 ||
			    !valid_profile(instance->profile))
				goto error;
			have_profile = true;
		} else if (spa_streq(key, "rate_numerator")) {
			if (have_rate_num || spa_json_parse_int(token, len, &value) <= 0 ||
			    value <= 0)
				goto error;
			rate_num = (uint32_t)value;
			have_rate_num = true;
		} else if (spa_streq(key, "rate_denominator")) {
			if (have_rate_denom || spa_json_parse_int(token, len, &value) <= 0 ||
			    value <= 0)
				goto error;
			rate_denom = (uint32_t)value;
			have_rate_denom = true;
		} else {
			goto error;
		}
	}
	if (!have_count || !have_scale || !have_profile ||
	    !have_rate_num || !have_rate_denom)
		goto error;

	instance->shape[0] = instance->actuator_count;
	instance->formats[0] = (struct spa_fgn_format) {
		.element_type = SPA_ELEMENT_TYPE_F32_LE,
		.layout = SPA_NDARRAY_LAYOUT_COLUMN_MAJOR,
		.rate_num = rate_num,
		.rate_denom = rate_denom,
		.n_dimensions = 1,
		.shape = instance->shape,
		.schema = SPA_ALPAO_SCHEMA_DEMANDED_PDM_COMMAND,
	};
	instance->formats[1] = (struct spa_fgn_format) {
		.element_type = SPA_ELEMENT_TYPE_F64_LE,
		.layout = SPA_NDARRAY_LAYOUT_ROW_MAJOR,
		.n_dimensions = 1,
		.shape = instance->shape,
		.schema = SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND,
	};
	*result = instance;
	return 0;

error:
	free(instance);
	return res;
}

static void cleanup(void *data)
{
	free(data);
}

static int get_port_format(void *data, uint32_t port,
		const struct spa_fgn_format **format)
{
	struct normalization_instance *instance = data;

	if (instance == NULL || format == NULL || port >= SPA_N_ELEMENTS(ports))
		return -EINVAL;
	*format = &instance->formats[port];
	return 0;
}

static void copy_metadata(struct spa_buffer *output, const struct spa_buffer *input)
{
	uint32_t i;

	for (i = 0; i < output->n_metas; i++) {
		struct spa_meta *source = spa_buffer_find_meta(input,
				output->metas[i].type);
		if (source != NULL && source->data != NULL &&
		    output->metas[i].data != NULL)
			memmove(output->metas[i].data, source->data,
					SPA_MIN(source->size, output->metas[i].size));
	}
}

static int process(void *data, const struct spa_fgn_buffer *inputs,
		uint32_t n_inputs, struct spa_fgn_buffer *outputs,
		uint32_t n_outputs)
{
	struct normalization_instance *instance = data;
	const struct spa_data *input_data;
	struct spa_data *output_data;
	const float *physical;
	double *normalized;
	uint32_t i;

	if (instance == NULL || inputs == NULL || outputs == NULL ||
	    n_inputs != 1 || n_outputs != 1 || inputs[0].buffer == NULL ||
	    outputs[0].buffer == NULL)
		return -EINVAL;
	input_data = &inputs[0].buffer->datas[0];
	output_data = &outputs[0].buffer->datas[0];
	physical = SPA_PTROFF(input_data->data, input_data->chunk->offset,
			const float);
	normalized = SPA_PTROFF(output_data->data, output_data->chunk->offset,
			double);
	for (i = 0; i < instance->actuator_count; i++) {
		double value = (double)physical[i] / instance->command_scale;

		if (!isfinite(value) || value < -1.0 || value > 1.0)
			return -ERANGE;
		normalized[i] = value;
	}
	copy_metadata(outputs[0].buffer, inputs[0].buffer);
	output_data->chunk->size = instance->actuator_count * sizeof(double);
	return 0;
}

static const struct spa_fgn_descriptor descriptor = {
	.struct_size = sizeof(struct spa_fgn_descriptor),
	.version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = SPA_FGN_ALPAO_LABEL_COMMAND_NORMALIZATION,
	.n_ports = SPA_N_ELEMENTS(ports),
	.ports = ports,
	.instantiate = instantiate,
	.cleanup = cleanup,
	.get_port_format = get_port_format,
	.process = process,
};

static const struct spa_fgn_descriptor *find_descriptor(const char *name)
{
	return name != NULL && spa_streq(name, descriptor.name) ? &descriptor : NULL;
}

static const struct spa_fgn_plugin plugin = {
	.struct_size = sizeof(struct spa_fgn_plugin),
	.abi_version = SPA_FGN_PLUGIN_ABI_VERSION,
	.name = "alpao",
	.find_descriptor = find_descriptor,
	.flags = SPA_FGN_PLUGIN_FLAG_NONE,
};

SPA_EXPORT
const struct spa_fgn_plugin *spa_filter_graph_ndarray_plugin_get_interface(
		uint32_t abi_version)
{
	return abi_version == SPA_FGN_PLUGIN_ABI_VERSION ? &plugin : NULL;
}

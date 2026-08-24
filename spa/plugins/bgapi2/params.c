/* SPDX-License-Identifier: MIT */
#include "params.h"

#include <stdbool.h>

#include <spa/param/props.h>

#include "camera.h"

static bool is_scalar(enum bgapi2_feature_kind kind)
{
	return kind != BGAPI2_FEATURE_COMMAND;
}

static int scalar_feature_at(struct bgapi2_camera *camera,
		uint32_t scalar_index, uint32_t *feature_index,
		struct bgapi2_feature_info *info)
{
	uint32_t count = bgapi2_camera_get_feature_count(camera);
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (bgapi2_camera_get_feature_info(camera, i, info) < 0 ||
				!is_scalar(info->kind))
			continue;
		if (scalar_index-- == 0) {
			*feature_index = i;
			return 0;
		}
	}
	return -1;
}

static void append_default(struct spa_pod_builder *builder,
		enum bgapi2_feature_kind kind)
{
	switch (kind) {
	case BGAPI2_FEATURE_BOOLEAN:
		spa_pod_builder_bool(builder, false);
		break;
	case BGAPI2_FEATURE_INTEGER:
		spa_pod_builder_long(builder, 0);
		break;
	case BGAPI2_FEATURE_FLOATING:
		spa_pod_builder_double(builder, 0.0);
		break;
	case BGAPI2_FEATURE_ENUMERATION:
		spa_pod_builder_int(builder, 0);
		break;
	case BGAPI2_FEATURE_STRING:
		spa_pod_builder_string(builder, "");
		break;
	case BGAPI2_FEATURE_COMMAND:
		break;
	}
}

static void append_value(struct spa_pod_builder *builder,
		const struct bgapi2_feature_value *value)
{
	switch (value->kind) {
	case BGAPI2_FEATURE_BOOLEAN:
		spa_pod_builder_bool(builder, value->boolean);
		break;
	case BGAPI2_FEATURE_INTEGER:
		spa_pod_builder_long(builder, value->integer);
		break;
	case BGAPI2_FEATURE_FLOATING:
		spa_pod_builder_double(builder, value->floating);
		break;
	case BGAPI2_FEATURE_ENUMERATION:
		spa_pod_builder_int(builder, value->enumeration);
		break;
	case BGAPI2_FEATURE_STRING:
		spa_pod_builder_string(builder, value->string);
		break;
	case BGAPI2_FEATURE_COMMAND:
		break;
	}
}

struct spa_pod *bgapi2_build_feature_prop_info(struct bgapi2_camera *camera,
		uint32_t scalar_index, struct spa_pod_builder *builder)
{
	struct bgapi2_feature_info info;
	struct bgapi2_feature_value current;
	struct spa_pod_frame object;
	uint32_t index, i;
	bool have_value;

	if (scalar_feature_at(camera, scalar_index, &index, &info) < 0)
		return NULL;
	have_value = info.available && info.readable &&
			bgapi2_camera_get_feature_value(camera, index, &current) == 0;
	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(builder,
			SPA_PROP_INFO_name, SPA_POD_String(info.property_name),
			SPA_PROP_INFO_description, SPA_POD_String(info.description),
			0);
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, 0);
	switch (info.kind) {
	case BGAPI2_FEATURE_BOOLEAN: {
		struct spa_pod_frame choice;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_bool(builder, have_value ? current.boolean : false);
		spa_pod_builder_bool(builder, false);
		spa_pod_builder_bool(builder, true);
		spa_pod_builder_pop(builder, &choice);
		break;
	}
	case BGAPI2_FEATURE_INTEGER: {
		int64_t minimum, maximum;
		int64_t value = have_value ? current.integer : 0;
		if (info.available && bgapi2_camera_get_feature_integer_range(camera,
				index, &minimum, &maximum) == 0)
			spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Long(
					value, minimum, maximum), 0);
		else
			spa_pod_builder_long(builder, value);
		break;
	}
	case BGAPI2_FEATURE_FLOATING: {
		double minimum, maximum;
		double value = have_value ? current.floating : 0.0;
		if (info.available && bgapi2_camera_get_feature_float_range(camera,
				index, &minimum, &maximum) == 0)
			spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Double(
					value, minimum, maximum), 0);
		else
			spa_pod_builder_double(builder, value);
		break;
	}
	case BGAPI2_FEATURE_ENUMERATION: {
		struct spa_pod_frame choice, labels;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_int(builder, have_value ? current.enumeration : 0);
		for (i = 0; i < info.n_enum_entries; i++)
			spa_pod_builder_int(builder, (int32_t)i);
		spa_pod_builder_pop(builder, &choice);
		spa_pod_builder_prop(builder, SPA_PROP_INFO_labels, 0);
		spa_pod_builder_push_struct(builder, &labels);
		for (i = 0; i < info.n_enum_entries; i++) {
			const char *entry = bgapi2_camera_get_feature_enum_entry(camera,
					index, i);
			spa_pod_builder_int(builder, (int32_t)i);
			spa_pod_builder_string(builder, entry == NULL ? "" : entry);
		}
		spa_pod_builder_pop(builder, &labels);
		break;
	}
	case BGAPI2_FEATURE_STRING:
		if (have_value)
			append_value(builder, &current);
		else
			append_default(builder, info.kind);
		break;
	case BGAPI2_FEATURE_COMMAND:
		return NULL;
	}
	spa_pod_builder_add(builder, SPA_PROP_INFO_params,
			SPA_POD_Bool(info.available && info.writable), 0);
	return spa_pod_builder_pop(builder, &object);
}

struct spa_pod *bgapi2_build_feature_props(struct bgapi2_camera *camera,
		struct spa_pod_builder *builder)
{
	struct spa_pod_frame object, values;
	uint32_t count = bgapi2_camera_get_feature_count(camera);
	uint32_t i;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	for (i = 0; i < count; i++) {
		struct bgapi2_feature_info info;
		struct bgapi2_feature_value value;

		if (bgapi2_camera_get_feature_info(camera, i, &info) < 0 ||
				!is_scalar(info.kind) || !info.available || !info.readable ||
				bgapi2_camera_get_feature_value(camera, i, &value) < 0)
			continue;
		spa_pod_builder_string(builder, info.property_name);
		append_value(builder, &value);
	}
	spa_pod_builder_pop(builder, &values);
	return spa_pod_builder_pop(builder, &object);
}

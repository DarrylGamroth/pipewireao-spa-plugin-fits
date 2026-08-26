/* SPDX-License-Identifier: MIT */
#include "params.h"

#include <stdbool.h>

#include <spa/param/props.h>

#include "camera.h"

static bool is_property(enum andor3_feature_kind kind)
{
	return kind >= ANDOR3_FEATURE_BOOLEAN && kind <= ANDOR3_FEATURE_COMMAND;
}

static int scalar_feature_at(struct andor3_camera *camera,
		uint32_t scalar_index, uint32_t *feature_index,
		struct andor3_feature_info *info)
{
	uint32_t count = andor3_camera_get_feature_count(camera);
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (andor3_camera_get_feature_info(camera, i, info) < 0 ||
				!is_property(info->kind))
			continue;
		if (scalar_index-- == 0) {
			*feature_index = i;
			return 0;
		}
	}
	return -1;
}

static void append_default(struct spa_pod_builder *builder,
		enum andor3_feature_kind kind)
{
	switch (kind) {
	case ANDOR3_FEATURE_BOOLEAN:
		spa_pod_builder_bool(builder, false);
		break;
	case ANDOR3_FEATURE_INTEGER:
		spa_pod_builder_long(builder, 0);
		break;
	case ANDOR3_FEATURE_FLOATING:
		spa_pod_builder_double(builder, 0.0);
		break;
	case ANDOR3_FEATURE_ENUMERATION:
		spa_pod_builder_int(builder, 0);
		break;
	case ANDOR3_FEATURE_STRING:
		spa_pod_builder_string(builder, "");
		break;
	case ANDOR3_FEATURE_COMMAND:
		spa_pod_builder_bool(builder, false);
		break;
	}
}

static void append_value(struct spa_pod_builder *builder,
		const struct andor3_feature_value *value)
{
	switch (value->kind) {
	case ANDOR3_FEATURE_BOOLEAN:
		spa_pod_builder_bool(builder, value->boolean);
		break;
	case ANDOR3_FEATURE_INTEGER:
		spa_pod_builder_long(builder, value->integer);
		break;
	case ANDOR3_FEATURE_FLOATING:
		spa_pod_builder_double(builder, value->floating);
		break;
	case ANDOR3_FEATURE_ENUMERATION:
		spa_pod_builder_int(builder, value->enumeration);
		break;
	case ANDOR3_FEATURE_STRING:
		spa_pod_builder_string(builder, value->string);
		break;
	case ANDOR3_FEATURE_COMMAND:
		spa_pod_builder_bool(builder, false);
		break;
	}
}

struct spa_pod *andor3_build_feature_prop_info(struct andor3_camera *camera,
		uint32_t scalar_index, struct spa_pod_builder *builder)
{
	struct andor3_feature_info info;
	struct andor3_feature_value current;
	struct spa_pod_frame object;
	uint32_t index, i;
	bool have_value;

	if (scalar_feature_at(camera, scalar_index, &index, &info) < 0)
		return NULL;
	have_value = info.kind != ANDOR3_FEATURE_COMMAND &&
			info.available && info.readable &&
			andor3_camera_get_feature_value(camera, index, &current) == 0;
	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(builder,
			SPA_PROP_INFO_name, SPA_POD_String(info.property_name),
			SPA_PROP_INFO_description, SPA_POD_String(info.description),
			0);
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, 0);
	switch (info.kind) {
	case ANDOR3_FEATURE_BOOLEAN:
	case ANDOR3_FEATURE_COMMAND: {
		struct spa_pod_frame choice;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_bool(builder, have_value ? current.boolean : false);
		spa_pod_builder_bool(builder, false);
		spa_pod_builder_bool(builder, true);
		spa_pod_builder_pop(builder, &choice);
		break;
	}
	case ANDOR3_FEATURE_INTEGER: {
		struct spa_pod_frame choice;
		int64_t minimum, maximum;
		int64_t value = have_value ? current.integer : 0;
		if (info.available && andor3_camera_get_feature_integer_range(camera,
				index, &minimum, &maximum) == 0) {
			spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Range, 0);
			spa_pod_builder_long(builder, value);
			spa_pod_builder_long(builder, minimum);
			spa_pod_builder_long(builder, maximum);
			spa_pod_builder_pop(builder, &choice);
		} else {
			spa_pod_builder_long(builder, value);
		}
		break;
	}
	case ANDOR3_FEATURE_FLOATING: {
		struct spa_pod_frame choice;
		double minimum, maximum;
		double value = have_value ? current.floating : 0.0;
		if (info.available && andor3_camera_get_feature_float_range(camera,
				index, &minimum, &maximum) == 0) {
			spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Range, 0);
			spa_pod_builder_double(builder, value);
			spa_pod_builder_double(builder, minimum);
			spa_pod_builder_double(builder, maximum);
			spa_pod_builder_pop(builder, &choice);
		} else {
			spa_pod_builder_double(builder, value);
		}
		break;
	}
	case ANDOR3_FEATURE_ENUMERATION: {
		struct spa_pod_frame choice, labels;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_int(builder, have_value ? current.enumeration : 0);
		for (i = 0; i < info.n_enum_entries; i++)
			spa_pod_builder_int(builder, (int32_t)i);
		spa_pod_builder_pop(builder, &choice);
		spa_pod_builder_prop(builder, SPA_PROP_INFO_labels, 0);
		spa_pod_builder_push_struct(builder, &labels);
		for (i = 0; i < info.n_enum_entries; i++) {
			const char *entry = andor3_camera_get_feature_enum_entry(camera,
					index, i);
			spa_pod_builder_int(builder, (int32_t)i);
			spa_pod_builder_string(builder, entry == NULL ? "" : entry);
		}
		spa_pod_builder_pop(builder, &labels);
		break;
	}
	case ANDOR3_FEATURE_STRING:
		if (have_value)
			append_value(builder, &current);
		else
			append_default(builder, info.kind);
		break;
	}
	spa_pod_builder_add(builder, SPA_PROP_INFO_params,
			SPA_POD_Bool(info.available && info.writable), 0);
	return spa_pod_builder_pop(builder, &object);
}

struct spa_pod *andor3_build_feature_props(struct andor3_camera *camera,
		struct spa_pod_builder *builder)
{
	struct spa_pod_frame object, values;
	uint32_t count = andor3_camera_get_feature_count(camera);
	uint32_t i;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	for (i = 0; i < count; i++) {
		struct andor3_feature_info info;
		struct andor3_feature_value value;

		if (andor3_camera_get_feature_info(camera, i, &info) < 0 ||
				info.kind == ANDOR3_FEATURE_COMMAND || !info.available ||
				!info.readable ||
				andor3_camera_get_feature_value(camera, i, &value) < 0)
			continue;
		spa_pod_builder_string(builder, info.property_name);
		append_value(builder, &value);
	}
	spa_pod_builder_pop(builder, &values);
	return spa_pod_builder_pop(builder, &object);
}

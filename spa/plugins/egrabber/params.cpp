/* SPDX-License-Identifier: MIT */

#include "params.hpp"

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <string>
#include <type_traits>

namespace egrabber_pipewire {
namespace {

void append_feature_value(spa_pod_builder *builder, const FeatureValue &value)
{
	std::visit([builder](const auto &item) {
		using type = std::decay_t<decltype(item)>;
		if constexpr (std::is_same_v<type, bool>)
			spa_pod_builder_bool(builder, item);
		else if constexpr (std::is_same_v<type, std::int64_t>)
			spa_pod_builder_long(builder, item);
		else if constexpr (std::is_same_v<type, double>)
			spa_pod_builder_double(builder, item);
		else if constexpr (std::is_same_v<type, std::int32_t>)
			spa_pod_builder_int(builder, item);
		else
			spa_pod_builder_string(builder, item.c_str());
	}, value);
}

FeatureValue default_feature_value(const Feature &feature)
{
	switch (feature.kind) {
	case FeatureKind::boolean:
		return false;
	case FeatureKind::integer:
		return std::int64_t{0};
	case FeatureKind::floating:
		return 0.0;
	case FeatureKind::enumeration:
		return std::int32_t{0};
	case FeatureKind::string:
		return std::string{};
	default:
		return std::int32_t{0};
	}
}

FeatureValue feature_value_or_default(Camera &camera, const Feature &feature)
{
	if (feature.readable) {
		try {
			return camera.feature_value(feature);
		} catch (...) {
		}
	}
	return default_feature_value(feature);
}

} // namespace

const Feature *scalar_feature_at(const Camera &camera, std::uint32_t index)
{
	for (const auto &feature : camera.features()) {
		if (!is_scalar_feature(feature))
			continue;
		if (index-- == 0)
			return &feature;
	}
	return nullptr;
}

spa_pod *build_feature_prop_info(Camera &camera, const Feature &feature,
		spa_pod_builder *builder)
{
	spa_pod_frame object;
	const auto current = feature_value_or_default(camera, feature);

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(builder,
			SPA_PROP_INFO_name, SPA_POD_String(feature.property_name.c_str()),
			SPA_PROP_INFO_description, SPA_POD_String(feature.description.c_str()),
			0);
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, 0);
	switch (feature.kind) {
	case FeatureKind::boolean: {
		spa_pod_frame choice;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_bool(builder, std::get<bool>(current));
		spa_pod_builder_bool(builder, false);
		spa_pod_builder_bool(builder, true);
		spa_pod_builder_pop(builder, &choice);
		break;
	}
	case FeatureKind::integer: {
		const auto value = std::get<std::int64_t>(current);
		const auto range = camera.feature_integer_range(feature);
		if (range)
			spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Long(
					value, range->first, range->second), 0);
		else
			spa_pod_builder_long(builder, value);
		break;
	}
	case FeatureKind::floating: {
		const auto value = std::get<double>(current);
		const auto range = camera.feature_float_range(feature);
		if (range)
			spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Double(
					value, range->first, range->second), 0);
		else
			spa_pod_builder_double(builder, value);
		break;
	}
	case FeatureKind::enumeration: {
		spa_pod_frame choice;
		spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
		spa_pod_builder_int(builder, std::get<std::int32_t>(current));
		for (std::uint32_t i = 0; i < feature.enum_entries.size(); i++)
			spa_pod_builder_int(builder, static_cast<std::int32_t>(i));
		spa_pod_builder_pop(builder, &choice);
		spa_pod_frame labels;
		spa_pod_builder_prop(builder, SPA_PROP_INFO_labels, 0);
		spa_pod_builder_push_struct(builder, &labels);
		for (std::uint32_t i = 0; i < feature.enum_entries.size(); i++) {
			spa_pod_builder_int(builder, static_cast<std::int32_t>(i));
			spa_pod_builder_string(builder, feature.enum_entries[i].c_str());
		}
		spa_pod_builder_pop(builder, &labels);
		break;
	}
	case FeatureKind::string:
		append_feature_value(builder, current);
		break;
	default:
		return nullptr;
	}
	spa_pod_builder_add(builder, SPA_PROP_INFO_params,
			SPA_POD_Bool(feature.writeable), 0);
	return static_cast<spa_pod *>(spa_pod_builder_pop(builder, &object));
}

spa_pod *build_feature_props(Camera &camera, spa_pod_builder *builder)
{
	spa_pod_frame object, values;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &values);
	for (const auto &feature : camera.features()) {
		if (!feature.readable || !is_scalar_feature(feature))
			continue;
		try {
			const auto value = camera.feature_value(feature);
			spa_pod_builder_string(builder, feature.property_name.c_str());
			append_feature_value(builder, value);
		} catch (...) {
		}
	}
	spa_pod_builder_pop(builder, &values);
	return static_cast<spa_pod *>(spa_pod_builder_pop(builder, &object));
}

} // namespace egrabber_pipewire

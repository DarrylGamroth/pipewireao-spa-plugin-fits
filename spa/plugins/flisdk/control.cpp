/* SPDX-License-Identifier: MIT */

#include "control.h"

#include "camera.h"
#include "../egrabber/control_backend.hpp"
#include "../egrabber/feature.hpp"

#include <CLProtocol/ClSerialTypes.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/parser.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using egrabber_pipewire::CLProtocolControlOptions;
using egrabber_pipewire::ControlBackend;
using egrabber_pipewire::Feature;
using egrabber_pipewire::FeatureKind;
using egrabber_pipewire::FeatureValue;
using egrabber_pipewire::SerialTimeout;
using egrabber_pipewire::SerialTransport;

namespace {

std::vector<std::string> split_paths(const char *input)
{
	std::vector<std::string> result;
	if (input == nullptr)
		return result;
	std::string value(input);
	std::size_t begin = 0;
	while (begin <= value.size()) {
		const auto end = value.find(':', begin);
		auto path = value.substr(begin,
				end == std::string::npos ? end : end - begin);
		if (!path.empty())
			result.push_back(std::move(path));
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return result;
}

class FliSdkSerial final : public SerialTransport {
public:
	explicit FliSdkSerial(struct flisdk_camera *camera) : camera_(camera) {}

	void open() override { open_ = true; }
	void close() noexcept override {
		open_ = false;
		response_.clear();
	}
	void flush() override { response_.clear(); }

	std::size_t read(void *buffer, std::size_t size, std::uint32_t) override
	{
		if (!open_ || response_.empty())
			throw SerialTimeout("FliSdk command response is empty");
		const auto count = std::min(size, response_.size());
		std::memcpy(buffer, response_.data(), count);
		response_.erase(0, count);
		return count;
	}

	std::size_t write(const void *buffer, std::size_t size,
			std::uint32_t) override
	{
		if (!open_)
			throw std::runtime_error("FliSdk serial transport is closed");
		std::string command(static_cast<const char *>(buffer), size);
		while (!command.empty() &&
				(command.back() == '\r' || command.back() == '\n'))
			command.pop_back();
		if (command.empty())
			throw std::runtime_error("CLProtocol supplied an empty command");
		std::vector<char> response(64u * 1024u);
		const int result = flisdk_camera_send_command(camera_, command.c_str(),
				response.data(), response.size());
		if (result < 0)
			throw std::runtime_error("FliSdk camera command failed");
		response_ = response.data();
		response_ += "\r\nfli-cli>";
		return size;
	}

	std::uint32_t supported_baud_rates() override
	{
		return CL_BAUDRATE_115200;
	}

	void set_baud_rate(std::uint32_t baud_rate) override
	{
		if (baud_rate != CL_BAUDRATE_115200)
			throw std::runtime_error("FliSdk C-RED transport requires 115200 baud");
	}

private:
	struct flisdk_camera *camera_;
	std::string response_;
	bool open_ = false;
};

FeatureValue feature_value(ControlBackend &backend, const Feature &feature)
{
	if (!backend.readable(feature))
		throw std::runtime_error(feature.name + " is not currently readable");
	switch (feature.kind) {
	case FeatureKind::boolean:
		return backend.get_integer(feature) != 0;
	case FeatureKind::integer:
		return backend.get_integer(feature);
	case FeatureKind::floating:
		return backend.get_float(feature);
	case FeatureKind::enumeration: {
		const auto value = backend.get_string(feature);
		const auto found = std::find(feature.enum_entries.begin(),
				feature.enum_entries.end(), value);
		if (found == feature.enum_entries.end())
			throw std::runtime_error(feature.name +
					" returned an unknown enumeration value");
		return static_cast<std::int32_t>(
				std::distance(feature.enum_entries.begin(), found));
	}
	case FeatureKind::string:
		return backend.get_string(feature);
	default:
		throw std::runtime_error(feature.name + " is not a scalar feature");
	}
}

FeatureValue default_value(const Feature &feature)
{
	switch (feature.kind) {
	case FeatureKind::boolean: return false;
	case FeatureKind::integer: return std::int64_t{0};
	case FeatureKind::floating: return 0.0;
	case FeatureKind::enumeration: return std::int32_t{0};
	case FeatureKind::string: return std::string{};
	default: return false;
	}
}

void append_value(struct spa_pod_builder *builder, const FeatureValue &value)
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

const Feature *property_at(const std::vector<Feature> &features,
		std::uint32_t index)
{
	for (const auto &feature : features) {
		if (!egrabber_pipewire::is_scalar_feature(feature) &&
				feature.kind != FeatureKind::command)
			continue;
		if (index-- == 0)
			return &feature;
	}
	return nullptr;
}

} // namespace

struct flisdk_control {
	struct flisdk_camera *camera;
	FliSdkSerial serial;
	std::unique_ptr<ControlBackend> backend;

	explicit flisdk_control(struct flisdk_camera *camera_value)
		: camera(camera_value), serial(camera_value) {}
};

extern "C" int flisdk_control_open(struct flisdk_control **control_ptr,
		struct flisdk_camera *camera,
		const struct flisdk_control_options *options)
{
	if (control_ptr == nullptr || camera == nullptr || options == nullptr ||
			options->timeout_ms == 0)
		return -EINVAL;
	*control_ptr = nullptr;
	try {
		auto control = std::make_unique<flisdk_control>(camera);
		CLProtocolControlOptions backend_options;
		backend_options.libraries = split_paths(options->libraries);
		if (options->device_template != nullptr)
			backend_options.device_template = options->device_template;
		if (options->camera_serial != nullptr)
			backend_options.camera_serial = options->camera_serial;
		if (options->genapi_runtime != nullptr)
			backend_options.genapi_runtime = options->genapi_runtime;
		backend_options.timeout_ms = options->timeout_ms;
		control->backend = egrabber_pipewire::make_clprotocol_control_backend(
				control->serial, backend_options);
		*control_ptr = control.release();
		return 0;
	} catch (...) {
		return -EIO;
	}
}

extern "C" void flisdk_control_close(struct flisdk_control *control)
{
	delete control;
}

extern "C" int flisdk_control_build_prop_info(struct flisdk_control *control,
		uint32_t index, struct spa_pod_builder *builder,
		struct spa_pod **param)
{
	if (control == nullptr || builder == nullptr || param == nullptr)
		return -EINVAL;
	const auto *feature = property_at(control->backend->features(), index);
	if (feature == nullptr)
		return 0;
	try {
		struct spa_pod_frame object;
		FeatureValue current = feature->kind == FeatureKind::command
			? FeatureValue{false} : default_value(*feature);
		if (feature->kind != FeatureKind::command && feature->readable) {
			try { current = feature_value(*control->backend, *feature); }
			catch (...) {}
		}
		spa_pod_builder_push_object(builder, &object,
				SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
		spa_pod_builder_add(builder,
				SPA_PROP_INFO_name, SPA_POD_String(feature->property_name.c_str()),
				SPA_PROP_INFO_description, SPA_POD_String(feature->description.c_str()),
				0);
		spa_pod_builder_prop(builder, SPA_PROP_INFO_type, 0);
		switch (feature->kind) {
		case FeatureKind::boolean:
		case FeatureKind::command: {
			struct spa_pod_frame choice;
			spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
			spa_pod_builder_bool(builder, std::get<bool>(current));
			spa_pod_builder_bool(builder, false);
			spa_pod_builder_bool(builder, true);
			spa_pod_builder_pop(builder, &choice);
			break;
		}
		case FeatureKind::integer: {
			const auto value = std::get<std::int64_t>(current);
			const auto range = control->backend->integer_range(*feature);
			if (range)
				spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Long(
						value, range->first, range->second), 0);
			else
				spa_pod_builder_long(builder, value);
			break;
		}
		case FeatureKind::floating: {
			const auto value = std::get<double>(current);
			const auto range = control->backend->float_range(*feature);
			if (range)
				spa_pod_builder_add(builder, SPA_POD_CHOICE_RANGE_Double(
						value, range->first, range->second), 0);
			else
				spa_pod_builder_double(builder, value);
			break;
		}
		case FeatureKind::enumeration: {
			struct spa_pod_frame choice;
			spa_pod_builder_push_choice(builder, &choice, SPA_CHOICE_Enum, 0);
			spa_pod_builder_int(builder, std::get<std::int32_t>(current));
			for (std::uint32_t i = 0; i < feature->enum_entries.size(); i++)
				spa_pod_builder_int(builder, static_cast<std::int32_t>(i));
			spa_pod_builder_pop(builder, &choice);
			struct spa_pod_frame labels;
			spa_pod_builder_prop(builder, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(builder, &labels);
			for (std::uint32_t i = 0; i < feature->enum_entries.size(); i++) {
				spa_pod_builder_int(builder, static_cast<std::int32_t>(i));
				spa_pod_builder_string(builder, feature->enum_entries[i].c_str());
			}
			spa_pod_builder_pop(builder, &labels);
			break;
		}
		case FeatureKind::string:
			append_value(builder, current);
			break;
		default:
			return -ENOENT;
		}
		spa_pod_builder_add(builder, SPA_PROP_INFO_params,
				SPA_POD_Bool(feature->writeable), 0);
		*param = static_cast<spa_pod *>(spa_pod_builder_pop(builder, &object));
		return *param == nullptr ? -ENOSPC : 1;
	} catch (...) {
		return -EIO;
	}
}

extern "C" int flisdk_control_build_props(struct flisdk_control *control,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	if (control == nullptr || builder == nullptr || param == nullptr)
		return -EINVAL;
	try {
		struct spa_pod_frame object, values;
		spa_pod_builder_push_object(builder, &object,
				SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_prop(builder, SPA_PROP_params, 0);
		spa_pod_builder_push_struct(builder, &values);
		for (const auto &feature : control->backend->features()) {
			if (!feature.readable || !egrabber_pipewire::is_scalar_feature(feature))
				continue;
			try {
				const auto value = feature_value(*control->backend, feature);
				spa_pod_builder_string(builder, feature.property_name.c_str());
				append_value(builder, value);
			} catch (...) {}
		}
		spa_pod_builder_pop(builder, &values);
		*param = static_cast<spa_pod *>(spa_pod_builder_pop(builder, &object));
		return *param == nullptr ? -ENOSPC : 1;
	} catch (...) {
		return -EIO;
	}
}

extern "C" int flisdk_control_set(struct flisdk_control *control,
		const char *name, const struct spa_pod *value,
		bool allow_layout_change, bool *layout_changed)
{
	if (control == nullptr || name == nullptr || value == nullptr ||
			layout_changed == nullptr)
		return -EINVAL;
	*layout_changed = false;
	const auto &features = control->backend->features();
	const auto found = std::find_if(features.begin(), features.end(),
			[name](const Feature &feature) {
				return feature.property_name == name &&
					(egrabber_pipewire::is_scalar_feature(feature) ||
					 feature.kind == FeatureKind::command);
			});
	if (found == features.end())
		return -ENOENT;
	if (!found->writeable || !control->backend->writeable(*found))
		return -EACCES;
	const bool layout = egrabber_pipewire::changes_payload_layout(*found);
	if (layout && !allow_layout_change)
		return -EBUSY;
	FeatureValue old;
	std::function<void()> restore;
	bool applied = false;
	const struct flisdk_camera_info previous_info =
			*flisdk_camera_get_info(control->camera);
	try {
		if (found->kind == FeatureKind::command) {
			bool execute = false;
			if (spa_pod_get_bool(value, &execute) < 0)
				return -EINVAL;
			if (execute)
				control->backend->execute(*found);
			return 0;
		}
		old = feature_value(*control->backend, *found);
		restore = [&] {
			switch (found->kind) {
			case FeatureKind::boolean:
			case FeatureKind::integer:
				control->backend->set_integer(*found,
						found->kind == FeatureKind::boolean
						? (std::get<bool>(old) ? 1 : 0)
						: std::get<std::int64_t>(old));
				break;
			case FeatureKind::floating:
				control->backend->set_float(*found, std::get<double>(old));
				break;
			case FeatureKind::enumeration:
				control->backend->set_string(*found,
						found->enum_entries[std::get<std::int32_t>(old)]);
				break;
			case FeatureKind::string:
				control->backend->set_string(*found, std::get<std::string>(old));
				break;
			default: break;
			}
		};
		applied = true;
		switch (found->kind) {
		case FeatureKind::boolean: {
			bool parsed;
			if (spa_pod_get_bool(value, &parsed) < 0) return -EINVAL;
			control->backend->set_integer(*found, parsed ? 1 : 0);
			break;
		}
		case FeatureKind::integer: {
			std::int64_t parsed;
			std::int32_t small;
			if (spa_pod_get_long(value, &parsed) < 0) {
				if (spa_pod_get_int(value, &small) < 0) return -EINVAL;
				parsed = small;
			}
			control->backend->set_integer(*found, parsed);
			break;
		}
		case FeatureKind::floating: {
			double parsed;
			float small;
			if (spa_pod_get_double(value, &parsed) < 0) {
				if (spa_pod_get_float(value, &small) < 0) return -EINVAL;
				parsed = small;
			}
			control->backend->set_float(*found, parsed);
			break;
		}
		case FeatureKind::enumeration: {
			std::int32_t parsed;
			std::uint32_t id;
			if (spa_pod_get_int(value, &parsed) < 0) {
				if (spa_pod_get_id(value, &id) < 0 || id > INT32_MAX)
					return -EINVAL;
				parsed = static_cast<std::int32_t>(id);
			}
			if (parsed < 0 || static_cast<std::size_t>(parsed) >=
					found->enum_entries.size())
				return -EINVAL;
			control->backend->set_string(*found, found->enum_entries[parsed]);
			break;
		}
		case FeatureKind::string: {
			const char *parsed;
			if (spa_pod_get_string(value, &parsed) < 0) return -EINVAL;
			control->backend->set_string(*found, parsed);
			break;
		}
		default: return -EINVAL;
		}
		if (layout) {
			const auto width = std::find_if(features.begin(), features.end(),
					[](const Feature &feature) { return feature.name == "Width"; });
			const auto height = std::find_if(features.begin(), features.end(),
					[](const Feature &feature) { return feature.name == "Height"; });
			if (width == features.end() || height == features.end()) {
				restore();
				return -EIO;
			}
			const auto new_width = control->backend->get_integer(*width);
			const auto new_height = control->backend->get_integer(*height);
			if (new_width <= 0 || new_height <= 0 ||
					new_width > std::numeric_limits<std::uint32_t>::max() ||
					new_height > std::numeric_limits<std::uint32_t>::max() ||
					flisdk_camera_apply_layout(control->camera,
							static_cast<std::uint32_t>(new_width),
							static_cast<std::uint32_t>(new_height)) < 0) {
				restore();
				(void)flisdk_camera_apply_layout(control->camera,
						previous_info.width, previous_info.height);
				applied = false;
				return -EIO;
			}
			*layout_changed = true;
		} else if (flisdk_camera_refresh_info(control->camera) < 0) {
			restore();
			applied = false;
			return -EIO;
		}
		return 0;
	} catch (...) {
		if (applied && restore) {
			try {
				restore();
				if (layout)
					(void)flisdk_camera_apply_layout(control->camera,
							previous_info.width, previous_info.height);
				else
					(void)flisdk_camera_refresh_info(control->camera);
			} catch (...) {}
		}
		return -EIO;
	}
}

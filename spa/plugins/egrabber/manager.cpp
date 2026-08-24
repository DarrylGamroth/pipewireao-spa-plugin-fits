/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "camera.hpp"
#include "egrabber.hpp"
#include "options.hpp"

namespace {

using egrabber_pipewire::DiscoveredCamera;
using egrabber_pipewire::Options;

struct camera_descriptor {
	DiscoveredCamera camera;
	std::string key;
	std::string interface_index;
	std::string device_index;
	std::string stream_index;
	std::string name;
	std::string description;
	std::string path;
};

struct impl {
	struct spa_handle handle = {};
	struct spa_device device = {};
	struct spa_hook_list hooks = {};
	struct spa_device_info info = SPA_DEVICE_INFO_INIT();
	Options options;
	std::vector<camera_descriptor> cameras;
};

std::string camera_key(const Options &options, const DiscoveredCamera &camera)
{
	const auto &identity = camera.identity;
	if (!identity.serial.empty())
		return options.producer + "\x1fserial\x1f" + identity.serial +
				'\x1f' + identity.stream_id + '\x1f' +
				std::to_string(camera.stream_index);
	if (!identity.interface_id.empty() || !identity.device_id.empty() ||
			!identity.stream_id.empty())
		return options.producer + "\x1ftransport\x1f" +
				identity.interface_id + '\x1f' + identity.device_id + '\x1f' +
				identity.stream_id;
	return options.producer + "\x1findex\x1f" +
			std::to_string(camera.interface_index) + '\x1f' +
			std::to_string(camera.device_index) + '\x1f' +
			std::to_string(camera.stream_index);
}

camera_descriptor describe_camera(const Options &options,
		DiscoveredCamera camera)
{
	camera_descriptor descriptor;
	descriptor.key = camera_key(options, camera);
	descriptor.camera = std::move(camera);
	descriptor.interface_index = std::to_string(descriptor.camera.interface_index);
	descriptor.device_index = std::to_string(descriptor.camera.device_index);
	descriptor.stream_index = std::to_string(descriptor.camera.stream_index);
	const auto &identity = descriptor.camera.identity;
	const std::string stable = !identity.serial.empty() ? identity.serial :
			descriptor.interface_index + "." + descriptor.device_index + "." +
			descriptor.stream_index;
	descriptor.name = "egrabber_device." + stable;
	descriptor.description = identity.vendor;
	if (!identity.model.empty()) {
		if (!descriptor.description.empty())
			descriptor.description += " ";
		descriptor.description += identity.model;
	}
	if (descriptor.description.empty())
		descriptor.description = "eGrabber camera";
	descriptor.path = "egrabber:" + options.producer + ":" + stable;
	return descriptor;
}

int emit_camera(impl *self, uint32_t id)
{
	const auto &camera = self->cameras[id];
	const auto &identity = camera.camera.identity;
	const std::string buffer_count = std::to_string(self->options.buffer_count);
	const std::string acquisition_domain = self->options.acquisition_domain
		? egrabber_pipewire::format_acquisition_domain(
				*self->options.acquisition_domain) : std::string{};
	const std::string acquisition_generation = std::to_string(
			self->options.acquisition_generation);
	const std::string acquisition_sequence_context = std::to_string(
			self->options.acquisition_sequence_context);
	struct spa_dict_item items[24];
	uint32_t n_items = 0;

#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_ENUM_API, "egrabber.manager");
	ADD_ITEM(SPA_KEY_DEVICE_API, "egrabber");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	ADD_ITEM(SPA_KEY_OBJECT_PATH, camera.path.c_str());
	ADD_ITEM(SPA_KEY_DEVICE_NAME, camera.name.c_str());
	ADD_ITEM(SPA_KEY_DEVICE_DESCRIPTION, camera.description.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PRODUCER, self->options.producer.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_INTERFACE_INDEX, camera.interface_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_DEVICE_INDEX, camera.device_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_STREAM_INDEX, camera.stream_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_BUFFER_COUNT, buffer_count.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_CONTROL, self->options.control.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PROGRESSIVE,
			egrabber_pipewire::progressive_policy_name(self->options.progressive));
	if (self->options.acquisition_domain) {
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
				acquisition_domain.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION,
				acquisition_generation.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT,
				acquisition_sequence_context.c_str());
	}
	if (!identity.vendor.empty())
		ADD_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, identity.vendor.c_str());
	if (!identity.model.empty())
		ADD_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, identity.model.c_str());
	if (!identity.serial.empty()) {
		ADD_ITEM(SPA_KEY_DEVICE_SERIAL, identity.serial.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_SERIAL, identity.serial.c_str());
	}
	if (!identity.user_id.empty())
		ADD_ITEM(SPA_KEY_API_EGRABBER_USER_ID, identity.user_id.c_str());
	if (!identity.transport.empty())
		ADD_ITEM(SPA_KEY_API_EGRABBER_TRANSPORT, identity.transport.c_str());
#undef ADD_ITEM

	struct spa_dict dict = SPA_DICT_INIT(items, n_items);
	struct spa_device_object_info info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_EGRABBER_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
			SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &dict;
	spa_device_emit_object_info(&self->hooks, id, &info);
	return 1;
}

void discover_once(impl *self)
{
	auto discovered = egrabber_pipewire::discover_cameras(self->options);
	self->cameras.reserve(discovered.size());
	for (auto &&camera : discovered) {
		auto descriptor = describe_camera(self->options, std::move(camera));
		for (const auto &existing : self->cameras)
			if (existing.key == descriptor.key)
				throw std::runtime_error(
						"eGrabber discovery returned duplicate camera identities");
		self->cameras.emplace_back(std::move(descriptor));
	}
}

void emit_info(impl *self, bool full)
{
	const uint64_t old = full ? self->info.change_mask : 0;
	static const struct spa_dict_item items[] = {
		{ SPA_KEY_DEVICE_API, "egrabber" },
		{ SPA_KEY_DEVICE_NICK, "eGrabber manager" },
	};
	static const struct spa_dict props = SPA_DICT_INIT_ARRAY(items);

	if (full)
		self->info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS |
				SPA_DEVICE_CHANGE_MASK_PROPS;
	if (self->info.change_mask != 0) {
		self->info.props = &props;
		spa_device_emit_info(&self->hooks, &self->info);
		self->info.change_mask = old;
	}
}

int add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	auto *self = static_cast<impl *>(object);
	struct spa_hook_list save;

	spa_return_val_if_fail(self != nullptr && events != nullptr, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_info(self, true);
	for (uint32_t i = 0; i < self->cameras.size(); i++)
		emit_camera(self, i);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

int sync(void *object, int seq)
{
	auto *self = static_cast<impl *>(object);
	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_device_emit_result(&self->hooks, seq, 0, 0, nullptr);
	return 0;
}

int enum_params(void *, int, uint32_t, uint32_t, uint32_t,
		const struct spa_pod *)
{
	return -ENOTSUP;
}

int set_param(void *, uint32_t, uint32_t, const struct spa_pod *)
{
	return -ENOTSUP;
}

const struct spa_device_methods device_methods = {
	.version = SPA_VERSION_DEVICE_METHODS,
	.add_listener = add_listener,
	.sync = sync,
	.enum_params = enum_params,
	.set_param = set_param,
};

int get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto *self = reinterpret_cast<impl *>(handle);
	spa_return_val_if_fail(self != nullptr && interface != nullptr, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Device))
		return -ENOENT;
	*interface = &self->device;
	return 0;
}

int clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	auto *self = reinterpret_cast<impl *>(handle);
	std::destroy_at(self);
	return 0;
}

size_t get_size(const struct spa_handle_factory *, const struct spa_dict *)
{
	return sizeof(impl);
}

int init(const struct spa_handle_factory *, struct spa_handle *handle,
		const struct spa_dict *info, const struct spa_support *, uint32_t)
{
	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	auto *self = new (handle) impl{};
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	spa_hook_list_init(&self->hooks);
	self->device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE, &device_methods, self);
	try {
		egrabber_pipewire::read_options(self->options, info);
		discover_once(self);
	} catch (const std::invalid_argument &) {
		self->~impl();
		return -EINVAL;
	} catch (...) {
		self->~impl();
		return -EIO;
	}
	return 0;
}

const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Device, },
};

int enum_interface_info(const struct spa_handle_factory *,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != nullptr && index != nullptr, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

} // namespace

extern "C" const struct spa_handle_factory spa_egrabber_manager_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_EGRABBER_ENUM_MANAGER,
	nullptr,
	get_size,
	init,
	enum_interface_info,
};

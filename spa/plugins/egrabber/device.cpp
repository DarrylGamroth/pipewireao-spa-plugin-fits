/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/node/node.h>
#include <spa/node/keys.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "egrabber.hpp"
#include "options.hpp"

namespace {

using egrabber_pipewire::Options;

struct impl {
	struct spa_handle handle = {};
	struct spa_device device = {};
	struct spa_hook_list hooks = {};
	Options options;
	std::string path;
	std::string name;
	std::string description;
	std::string vendor;
	std::string model;
	std::string transport;
	std::string interface_index;
	std::string device_index;
	std::string stream_index;
	std::string buffer_count;
	std::string acquisition_domain;
	std::string acquisition_generation;
	std::string acquisition_sequence_context;
};

void add_identity_items(const impl *self, struct spa_dict_item *items,
		uint32_t &n_items)
{
#define ADD_ITEM(key, value) items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_API, "egrabber");
	ADD_ITEM(SPA_KEY_API_EGRABBER_PRODUCER, self->options.producer.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_INTERFACE_INDEX, self->interface_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_DEVICE_INDEX, self->device_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_STREAM_INDEX, self->stream_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_BUFFER_COUNT, self->buffer_count.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_CONTROL, self->options.control.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PROGRESSIVE,
			egrabber_pipewire::progressive_policy_name(self->options.progressive));
	if (self->options.acquisition_domain) {
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
				self->acquisition_domain.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION,
				self->acquisition_generation.c_str());
		ADD_ITEM(SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT,
				self->acquisition_sequence_context.c_str());
	}
	if (self->options.serial)
		ADD_ITEM(SPA_KEY_API_EGRABBER_SERIAL, self->options.serial->c_str());
	if (self->options.user_id)
		ADD_ITEM(SPA_KEY_API_EGRABBER_USER_ID, self->options.user_id->c_str());
	if (!self->transport.empty())
		ADD_ITEM(SPA_KEY_API_EGRABBER_TRANSPORT, self->transport.c_str());
#undef ADD_ITEM
}

void emit_info(impl *self)
{
	struct spa_dict_item device_items[24];
	uint32_t n_device_items = 0;
	add_identity_items(self, device_items, n_device_items);
#define ADD_DEVICE_ITEM(key, value) \
	device_items[n_device_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_DEVICE_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	if (!self->path.empty())
		ADD_DEVICE_ITEM(SPA_KEY_OBJECT_PATH, self->path.c_str());
	if (!self->name.empty())
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_NAME, self->name.c_str());
	if (!self->description.empty())
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_DESCRIPTION, self->description.c_str());
	if (!self->vendor.empty())
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, self->vendor.c_str());
	if (!self->model.empty())
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, self->model.c_str());
	if (self->options.serial)
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_SERIAL, self->options.serial->c_str());
#undef ADD_DEVICE_ITEM
	struct spa_dict device_props = SPA_DICT_INIT(device_items, n_device_items);
	struct spa_device_info info = SPA_DEVICE_INFO_INIT();
	info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS | SPA_DEVICE_CHANGE_MASK_PROPS;
	info.props = &device_props;
	spa_device_emit_info(&self->hooks, &info);

	struct spa_dict_item node_items[24];
	uint32_t n_node_items = 0;
	add_identity_items(self, node_items, n_node_items);
#define ADD_NODE_ITEM(key, value) \
	node_items[n_node_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_NODE_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_NODE_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	if (!self->name.empty())
		ADD_NODE_ITEM(SPA_KEY_NODE_NAME, self->name.c_str());
	if (!self->description.empty())
		ADD_NODE_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description.c_str());
#undef ADD_NODE_ITEM
	struct spa_dict node_props = SPA_DICT_INIT(node_items, n_node_items);
	struct spa_device_object_info object = SPA_DEVICE_OBJECT_INFO_INIT();
	object.type = SPA_TYPE_INTERFACE_Node;
	object.factory_name = SPA_NAME_API_EGRABBER_SOURCE;
	object.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
			SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	object.props = &node_props;
	spa_device_emit_object_info(&self->hooks, 0, &object);
}

int add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	auto *self = static_cast<impl *>(object);
	struct spa_hook_list save;
	spa_return_val_if_fail(self != nullptr && events != nullptr, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_info(self);
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
	std::destroy_at(reinterpret_cast<impl *>(handle));
	return 0;
}

std::string lookup(const struct spa_dict *info, const char *key)
{
	const char *value = info == nullptr ? nullptr : spa_dict_lookup(info, key);
	return value == nullptr ? std::string{} : value;
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
	} catch (const std::invalid_argument &) {
		self->~impl();
		return -EINVAL;
	}
	self->path = lookup(info, SPA_KEY_OBJECT_PATH);
	self->name = lookup(info, SPA_KEY_DEVICE_NAME);
	self->description = lookup(info, SPA_KEY_DEVICE_DESCRIPTION);
	self->vendor = lookup(info, SPA_KEY_DEVICE_VENDOR_NAME);
	self->model = lookup(info, SPA_KEY_DEVICE_PRODUCT_NAME);
	self->transport = lookup(info, SPA_KEY_API_EGRABBER_TRANSPORT);
	self->interface_index = std::to_string(self->options.interface_index);
	self->device_index = std::to_string(self->options.device_index);
	self->stream_index = std::to_string(self->options.stream_index);
	self->buffer_count = std::to_string(self->options.buffer_count);
	if (self->options.acquisition_domain)
		self->acquisition_domain = egrabber_pipewire::format_acquisition_domain(
				*self->options.acquisition_domain);
	self->acquisition_generation = std::to_string(
			self->options.acquisition_generation);
	self->acquisition_sequence_context = std::to_string(
			self->options.acquisition_sequence_context);
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

extern "C" const struct spa_handle_factory spa_egrabber_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_EGRABBER_DEVICE,
	nullptr,
	get_size,
	init,
	enum_interface_info,
};

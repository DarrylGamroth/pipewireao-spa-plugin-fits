/* SPDX-License-Identifier: MIT */
#include "bgapi2.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>

struct impl {
	struct spa_handle handle;
	struct spa_device device;
	struct spa_hook_list hooks;
	char producer[PATH_MAX];
	char serial[128];
	char interface_index[16];
	char device_index[16];
	char stream_index[16];
	char transport[64];
	char path[PATH_MAX + 384];
	char name[256];
	char description[320];
	char vendor[128];
	char model[128];
};

static int copy_property(char *destination, size_t capacity,
		const struct spa_dict *info, const char *key, bool required)
{
	const char *value = info == NULL ? NULL : spa_dict_lookup(info, key);
	int written;

	if (value == NULL) {
		destination[0] = '\0';
		return required ? -EINVAL : 0;
	}
	written = snprintf(destination, capacity, "%s", value);
	return written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
}

static void add_identity_items(const struct impl *this,
		struct spa_dict_item *items, uint32_t *n_items)
{
#define ADD_ITEM(key, value) items[(*n_items)++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_ITEM(SPA_KEY_DEVICE_API, "bgapi2");
	ADD_ITEM(SPA_KEY_API_BGAPI2_PRODUCER, this->producer);
	if (this->serial[0] != '\0')
		ADD_ITEM(SPA_KEY_API_BGAPI2_SERIAL, this->serial);
	ADD_ITEM(SPA_KEY_API_BGAPI2_INTERFACE_INDEX, this->interface_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_DEVICE_INDEX, this->device_index);
	ADD_ITEM(SPA_KEY_API_BGAPI2_STREAM_INDEX, this->stream_index);
	if (this->transport[0] != '\0')
		ADD_ITEM(SPA_KEY_API_BGAPI2_TRANSPORT, this->transport);
#undef ADD_ITEM
}

static void emit_info(struct impl *this)
{
	struct spa_dict_item device_items[18];
	struct spa_dict_item node_items[18];
	struct spa_device_info device_info = SPA_DEVICE_INFO_INIT();
	struct spa_device_object_info object = SPA_DEVICE_OBJECT_INFO_INIT();
	uint32_t n_device_items = 0, n_node_items = 0;

	add_identity_items(this, device_items, &n_device_items);
#define ADD_DEVICE_ITEM(key, value) \
	device_items[n_device_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_DEVICE_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Device");
	if (this->path[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_OBJECT_PATH, this->path);
	if (this->name[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_NAME, this->name);
	if (this->description[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_DESCRIPTION, this->description);
	if (this->vendor[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, this->vendor);
	if (this->model[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, this->model);
	if (this->serial[0] != '\0')
		ADD_DEVICE_ITEM(SPA_KEY_DEVICE_SERIAL, this->serial);
#undef ADD_DEVICE_ITEM
	struct spa_dict device_props = SPA_DICT_INIT(device_items, n_device_items);
	device_info.change_mask = SPA_DEVICE_CHANGE_MASK_FLAGS |
			SPA_DEVICE_CHANGE_MASK_PROPS;
	device_info.props = &device_props;
	spa_device_emit_info(&this->hooks, &device_info);

	add_identity_items(this, node_items, &n_node_items);
#define ADD_NODE_ITEM(key, value) \
	node_items[n_node_items++] = SPA_DICT_ITEM_INIT((key), (value))
	ADD_NODE_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_NODE_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	if (this->name[0] != '\0')
		ADD_NODE_ITEM(SPA_KEY_NODE_NAME, this->name);
	if (this->description[0] != '\0')
		ADD_NODE_ITEM(SPA_KEY_NODE_DESCRIPTION, this->description);
	if (this->vendor[0] != '\0')
		ADD_NODE_ITEM(SPA_KEY_DEVICE_VENDOR_NAME, this->vendor);
	if (this->model[0] != '\0')
		ADD_NODE_ITEM(SPA_KEY_DEVICE_PRODUCT_NAME, this->model);
	if (this->serial[0] != '\0')
		ADD_NODE_ITEM(SPA_KEY_DEVICE_SERIAL, this->serial);
#undef ADD_NODE_ITEM
	struct spa_dict node_props = SPA_DICT_INIT(node_items, n_node_items);
	object.type = SPA_TYPE_INTERFACE_Node;
	object.factory_name = SPA_NAME_API_BGAPI2_SOURCE;
	object.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
			SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	object.props = &node_props;
	spa_device_emit_object_info(&this->hooks, 0, &object);
}

static int impl_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL && events != NULL, -EINVAL);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);
	emit_info(this);
	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);
	return 0;
}

static const struct spa_device_methods device_methods = {
	.version = SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
};

static int impl_get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(this != NULL && interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Device))
		return -ENOENT;
	*interface = &this->device;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support SPA_UNUSED,
		uint32_t n_support SPA_UNUSED)
{
	struct impl *this = (struct impl *)handle;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	memset(this, 0, sizeof(*this));
	if (copy_property(this->producer, sizeof(this->producer), info,
			SPA_KEY_API_BGAPI2_PRODUCER, true) < 0 ||
			copy_property(this->serial, sizeof(this->serial), info,
			SPA_KEY_API_BGAPI2_SERIAL, false) < 0 ||
			copy_property(this->interface_index, sizeof(this->interface_index), info,
			SPA_KEY_API_BGAPI2_INTERFACE_INDEX, true) < 0 ||
			copy_property(this->device_index, sizeof(this->device_index), info,
			SPA_KEY_API_BGAPI2_DEVICE_INDEX, true) < 0 ||
			copy_property(this->stream_index, sizeof(this->stream_index), info,
			SPA_KEY_API_BGAPI2_STREAM_INDEX, true) < 0 ||
			copy_property(this->transport, sizeof(this->transport), info,
			SPA_KEY_API_BGAPI2_TRANSPORT, false) < 0 ||
			copy_property(this->path, sizeof(this->path), info,
			SPA_KEY_OBJECT_PATH, false) < 0 ||
			copy_property(this->name, sizeof(this->name), info,
			SPA_KEY_DEVICE_NAME, false) < 0 ||
			copy_property(this->description, sizeof(this->description), info,
			SPA_KEY_DEVICE_DESCRIPTION, false) < 0 ||
			copy_property(this->vendor, sizeof(this->vendor), info,
			SPA_KEY_DEVICE_VENDOR_NAME, false) < 0 ||
			copy_property(this->model, sizeof(this->model), info,
			SPA_KEY_DEVICE_PRODUCT_NAME, false) < 0)
		return -EINVAL;
	this->handle.get_interface = impl_get_interface;
	this->handle.clear = impl_clear;
	spa_hook_list_init(&this->hooks);
	this->device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE, &device_methods, this);
	return 0;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Device, },
};

static int impl_enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_bgapi2_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BGAPI2_DEVICE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

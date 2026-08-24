/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>

#include <spa/monitor/device.h>
#include <spa/node/node.h>
#include <spa/support/plugin.h>

#include "egrabber.hpp"

namespace {

struct observed_object {
	std::string type;
	std::string factory;
	std::vector<std::pair<std::string, std::string>> props;
};

struct observation {
	std::vector<observed_object> objects;
};

const std::string *property(const observed_object &object, const char *key)
{
	for (const auto &[candidate, value] : object.props)
		if (candidate == key)
			return &value;
	return nullptr;
}

void on_object_info(void *data, uint32_t,
		const struct spa_device_object_info *info)
{
	if (info == nullptr)
		return;
	auto *observed = static_cast<observation *>(data);
	observed_object object;
	object.type = info->type == nullptr ? "" : info->type;
	object.factory = info->factory_name == nullptr ? "" : info->factory_name;
	if (info->props != nullptr)
		for (uint32_t i = 0; i < info->props->n_items; i++)
			object.props.emplace_back(info->props->items[i].key,
					info->props->items[i].value);
	observed->objects.push_back(std::move(object));
}

const struct spa_device_events device_events = {
	.version = SPA_VERSION_DEVICE_EVENTS,
	.object_info = on_object_info,
};

const struct spa_handle_factory *find_factory(spa_handle_factory_enum_func_t enumerate,
		const char *name)
{
	const struct spa_handle_factory *factory = nullptr;
	uint32_t index = 0;
	while (enumerate(&factory, &index) > 0)
		if (spa_streq(factory->name, name))
			return factory;
	return nullptr;
}

struct instance {
	std::unique_ptr<void, decltype(&free)> memory{nullptr, free};
	struct spa_handle *handle = nullptr;
	struct spa_device *device = nullptr;

	instance(const struct spa_handle_factory *factory, const struct spa_dict *info)
		: memory(calloc(1, factory->get_size(factory, info)), free),
		  handle(static_cast<struct spa_handle *>(memory.get()))
	{
		spa_assert_se(memory != nullptr);
		spa_assert_se(factory->init(factory, handle, info, nullptr, 0) == 0);
		spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device,
				reinterpret_cast<void **>(&device)) == 0);
	}

	~instance()
	{
		if (handle != nullptr)
			spa_assert_se(handle->clear(handle) == 0);
	}
};

} // namespace

int main(int argc, char **argv)
{
	spa_assert_se(argc == 2);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != nullptr);
	auto enumerate = reinterpret_cast<spa_handle_factory_enum_func_t>(
			dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME));
	spa_assert_se(enumerate != nullptr);
	const auto *manager_factory = find_factory(enumerate,
			SPA_NAME_API_EGRABBER_ENUM_MANAGER);
	const auto *device_factory = find_factory(enumerate, SPA_NAME_API_EGRABBER_DEVICE);
	spa_assert_se(manager_factory != nullptr && device_factory != nullptr);
	int result = 0;
	{
		const struct spa_dict_item manager_items[] = {
			{ SPA_KEY_API_EGRABBER_PRODUCER, "gigelink" },
			{ SPA_KEY_API_EGRABBER_BUFFER_COUNT, "12" },
			{ SPA_KEY_API_EGRABBER_PROGRESSIVE, "offer" },
			{ SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN,
					"00112233445566778899aabbccddeeff" },
			{ SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION, "42" },
			{ SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT, "2" },
		};
		const struct spa_dict manager_info = SPA_DICT_INIT_ARRAY(manager_items);
		instance manager(manager_factory, &manager_info);
		observation manager_observation;
		struct spa_hook manager_listener;
		spa_assert_se(spa_device_add_listener(manager.device, &manager_listener,
				&device_events, &manager_observation) == 0);
		if (manager_observation.objects.empty()) {
			spa_hook_remove(&manager_listener);
			result = 77;
		} else {
			const size_t discovered_count = manager_observation.objects.size();
			spa_assert_se(spa_device_sync(manager.device, 7) == 0);
			spa_assert_se(manager_observation.objects.size() == discovered_count);
			const auto &camera = manager_observation.objects.front();
			spa_assert_se(camera.type == SPA_TYPE_INTERFACE_Device);
			spa_assert_se(camera.factory == SPA_NAME_API_EGRABBER_DEVICE);
			spa_assert_se(property(camera, SPA_KEY_API_EGRABBER_PRODUCER) != nullptr);
			spa_assert_se(property(camera, SPA_KEY_API_EGRABBER_INTERFACE_INDEX) != nullptr);
			spa_assert_se(property(camera, SPA_KEY_API_EGRABBER_DEVICE_INDEX) != nullptr);
			spa_assert_se(property(camera, SPA_KEY_API_EGRABBER_STREAM_INDEX) != nullptr);
			spa_assert_se(*property(camera, SPA_KEY_API_EGRABBER_BUFFER_COUNT) == "12");
			spa_assert_se(*property(camera, SPA_KEY_API_EGRABBER_PROGRESSIVE) == "offer");
			spa_assert_se(*property(camera,
					SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN) ==
					"00112233445566778899aabbccddeeff");
			spa_assert_se(*property(camera,
					SPA_KEY_API_EGRABBER_ACQUISITION_GENERATION) == "42");
			spa_assert_se(*property(camera,
					SPA_KEY_API_EGRABBER_ACQUISITION_SEQUENCE_CONTEXT) == "2");
			std::vector<struct spa_dict_item> device_items;
			device_items.reserve(camera.props.size());
			for (const auto &[key, value] : camera.props)
				device_items.push_back({ key.c_str(), value.c_str() });
			const struct spa_dict device_info = SPA_DICT_INIT(device_items.data(),
					static_cast<uint32_t>(device_items.size()));

			instance device(device_factory, &device_info);
			observation device_observation;
			struct spa_hook device_listener;
			spa_assert_se(spa_device_add_listener(device.device, &device_listener,
					&device_events, &device_observation) == 0);
			spa_assert_se(device_observation.objects.size() == 1);
			spa_assert_se(device_observation.objects[0].type == SPA_TYPE_INTERFACE_Node);
			spa_assert_se(device_observation.objects[0].factory == SPA_NAME_API_EGRABBER_SOURCE);
			spa_assert_se(property(device_observation.objects[0],
					SPA_KEY_API_EGRABBER_PRODUCER) != nullptr);
			spa_assert_se(property(device_observation.objects[0],
					SPA_KEY_API_EGRABBER_INTERFACE_INDEX) != nullptr);
			spa_assert_se(*property(device_observation.objects[0],
					SPA_KEY_API_EGRABBER_BUFFER_COUNT) == "12");
			spa_assert_se(*property(device_observation.objects[0],
					SPA_KEY_API_EGRABBER_PROGRESSIVE) == "offer");
			spa_assert_se(*property(device_observation.objects[0],
					SPA_KEY_API_EGRABBER_ACQUISITION_DOMAIN) ==
					"00112233445566778899aabbccddeeff");
			spa_hook_remove(&device_listener);
			spa_hook_remove(&manager_listener);
		}
	}
	spa_assert_se(dlclose(library) == 0);
	return result;
}

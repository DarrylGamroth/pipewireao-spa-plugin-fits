/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "camera.hpp"
#include "egrabber.hpp"
#include "options.hpp"

namespace {

using egrabber_pipewire::DiscoveredCamera;
using egrabber_pipewire::Options;

struct discovery_snapshot {
	std::vector<DiscoveredCamera> cameras;
	std::optional<int> sync_sequence;
	std::string error;
};

class discovery_worker {
public:
	discovery_worker(Options options, struct spa_loop_utils *loop_utils,
			struct spa_source *event_source)
		: options_(std::move(options)), loop_utils_(loop_utils),
		  event_source_(event_source), thread_(&discovery_worker::run, this)
	{
	}

	~discovery_worker()
	{
		stop();
	}

	discovery_worker(const discovery_worker &) = delete;
	discovery_worker &operator=(const discovery_worker &) = delete;

	bool request_sync(int sequence)
	{
		std::lock_guard lock(mutex_);
		if (stopping_ || sync_outstanding_)
			return false;
		sync_outstanding_ = true;
		requested_sync_ = sequence;
		refresh_requested_ = true;
		condition_.notify_all();
		return true;
	}

	std::optional<discovery_snapshot> take()
	{
		std::lock_guard lock(mutex_);
		if (!completed_)
			return std::nullopt;
		auto result = std::move(completed_);
		completed_.reset();
		if (result->sync_sequence)
			sync_outstanding_ = false;
		condition_.notify_all();
		return result;
	}

	void stop()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
			condition_.notify_all();
		}
		if (thread_.joinable())
			thread_.join();
	}

private:
	void run()
	{
		bool initial = true;
		for (;;) {
			std::optional<int> sync_sequence;
			{
				std::unique_lock lock(mutex_);
				if (!initial)
					condition_.wait_for(lock, std::chrono::seconds(1), [&] {
						return stopping_ || refresh_requested_;
					});
				if (stopping_)
					return;
				initial = false;
				refresh_requested_ = false;
				sync_sequence = std::exchange(requested_sync_, std::nullopt);
			}

			discovery_snapshot result;
			result.sync_sequence = sync_sequence;
			try {
				result.cameras = egrabber_pipewire::discover_cameras(options_);
			} catch (const std::exception &error) {
				result.error = error.what();
			} catch (...) {
				result.error = "unknown eGrabber discovery failure";
			}

			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [&] { return stopping_ || !completed_; });
				if (stopping_)
					return;
				completed_.emplace(std::move(result));
			}
			(void) spa_loop_utils_signal_event(loop_utils_, event_source_);

			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [&] { return stopping_ || !completed_; });
				if (stopping_)
					return;
			}
		}
	}

	Options options_;
	struct spa_loop_utils *loop_utils_;
	struct spa_source *event_source_;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::optional<discovery_snapshot> completed_;
	std::optional<int> requested_sync_;
	bool refresh_requested_ = false;
	bool sync_outstanding_ = false;
	bool stopping_ = false;
	std::thread thread_;
};

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
	struct spa_log *log = nullptr;
	struct spa_loop_utils *loop_utils = nullptr;
	struct spa_source *discovery_event = nullptr;
	Options options;
	std::vector<std::optional<camera_descriptor>> cameras;
	std::unique_ptr<discovery_worker> worker;
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

bool descriptor_equal(const camera_descriptor &a, const camera_descriptor &b)
{
	const auto &x = a.camera.identity;
	const auto &y = b.camera.identity;
	return a.key == b.key && a.camera.interface_index == b.camera.interface_index &&
			a.camera.device_index == b.camera.device_index &&
			a.camera.stream_index == b.camera.stream_index &&
			x.vendor == y.vendor && x.model == y.model && x.serial == y.serial &&
			x.user_id == y.user_id && x.transport == y.transport &&
			a.name == b.name && a.description == b.description && a.path == b.path;
}

int emit_camera(impl *self, uint32_t id)
{
	const auto &camera = self->cameras[id];
	if (!camera)
		return 0;
	const auto &identity = camera->camera.identity;
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
	ADD_ITEM(SPA_KEY_OBJECT_PATH, camera->path.c_str());
	ADD_ITEM(SPA_KEY_DEVICE_NAME, camera->name.c_str());
	ADD_ITEM(SPA_KEY_DEVICE_DESCRIPTION, camera->description.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PRODUCER, self->options.producer.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_INTERFACE_INDEX, camera->interface_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_DEVICE_INDEX, camera->device_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_STREAM_INDEX, camera->stream_index.c_str());
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

int reconcile_cameras(impl *self, std::vector<DiscoveredCamera> cameras)
{
	std::vector<camera_descriptor> discovered;
	discovered.reserve(cameras.size());
	for (auto &&camera : cameras)
		discovered.push_back(describe_camera(self->options, std::move(camera)));
	for (size_t i = 0; i < discovered.size(); ++i)
		for (size_t j = i + 1; j < discovered.size(); ++j)
			if (discovered[i].key == discovered[j].key)
				throw std::runtime_error(
						"eGrabber discovery returned duplicate camera identities");
	std::vector<bool> matched(discovered.size(), false);

	for (uint32_t id = 0; id < self->cameras.size(); ++id) {
		auto &current = self->cameras[id];
		if (!current)
			continue;
		const auto found = std::find_if(discovered.begin(), discovered.end(),
				[&](const camera_descriptor &candidate) {
					return candidate.key == current->key;
				});
		if (found == discovered.end()) {
			spa_device_emit_object_info(&self->hooks, id, nullptr);
			current.reset();
			continue;
		}
		const auto index = static_cast<size_t>(found - discovered.begin());
		matched[index] = true;
		if (!descriptor_equal(*current, *found)) {
			current = *found;
			emit_camera(self, id);
		}
	}

	for (size_t index = 0; index < discovered.size(); ++index) {
		if (matched[index])
			continue;
		auto slot = std::find_if(self->cameras.begin(), self->cameras.end(),
				[](const auto &camera) { return !camera; });
		uint32_t id;
		if (slot == self->cameras.end()) {
			id = static_cast<uint32_t>(self->cameras.size());
			self->cameras.emplace_back(std::move(discovered[index]));
		} else {
			id = static_cast<uint32_t>(slot - self->cameras.begin());
			*slot = std::move(discovered[index]);
		}
		emit_camera(self, id);
	}
	return 0;
}

int refresh_cameras(impl *self)
{
	return reconcile_cameras(self,
			egrabber_pipewire::discover_cameras(self->options));
}

void on_discovery_event(void *data, uint64_t)
{
	auto *self = static_cast<impl *>(data);
	auto result = self->worker->take();
	if (!result)
		return;
	int status = 0;
	if (!result->error.empty()) {
		status = -EIO;
		spa_log_warn(self->log, "eGrabber discovery refresh failed: %s",
				result->error.c_str());
	} else {
		try {
			status = reconcile_cameras(self, std::move(result->cameras));
		} catch (const std::exception &error) {
			status = -EIO;
			spa_log_warn(self->log, "eGrabber discovery reconciliation failed: %s",
					error.what());
		} catch (...) {
			status = -EIO;
			spa_log_warn(self->log, "eGrabber discovery reconciliation failed");
		}
	}
	if (result->sync_sequence)
		spa_device_emit_result(&self->hooks, *result->sync_sequence,
				status, 0, nullptr);
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
		if (self->cameras[i])
			emit_camera(self, i);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

int sync(void *object, int seq)
{
	auto *self = static_cast<impl *>(object);
	spa_return_val_if_fail(self != nullptr, -EINVAL);
	if (self->worker) {
		if (!self->worker->request_sync(seq))
			spa_device_emit_result(&self->hooks, seq, -EBUSY, 0, nullptr);
		return 0;
	}
	int res = 0;
	try {
		res = refresh_cameras(self);
	} catch (...) {
		res = -EIO;
	}
	spa_device_emit_result(&self->hooks, seq, res, 0, nullptr);
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
	self->worker.reset();
	if (self->discovery_event != nullptr)
		spa_loop_utils_destroy_source(self->loop_utils, self->discovery_event);
	std::destroy_at(self);
	return 0;
}

size_t get_size(const struct spa_handle_factory *, const struct spa_dict *)
{
	return sizeof(impl);
}

int init(const struct spa_handle_factory *, struct spa_handle *handle,
		const struct spa_dict *info, const struct spa_support *support,
		uint32_t n_support)
{
	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	auto *self = new (handle) impl{};
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	spa_hook_list_init(&self->hooks);
	self->log = static_cast<struct spa_log *>(spa_support_find(support,
			n_support, SPA_TYPE_INTERFACE_Log));
	self->loop_utils = static_cast<struct spa_loop_utils *>(spa_support_find(
			support, n_support, SPA_TYPE_INTERFACE_LoopUtils));
	self->device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE, &device_methods, self);
	try {
		egrabber_pipewire::read_options(self->options, info);
		if (self->loop_utils != nullptr) {
			self->discovery_event = spa_loop_utils_add_event(self->loop_utils,
					on_discovery_event, self);
			if (self->discovery_event == nullptr)
				throw std::runtime_error("could not create eGrabber discovery event");
			self->worker = std::make_unique<discovery_worker>(self->options,
					self->loop_utils, self->discovery_event);
		} else
			(void) refresh_cameras(self);
	} catch (const std::invalid_argument &) {
		self->worker.reset();
		if (self->discovery_event != nullptr)
			spa_loop_utils_destroy_source(self->loop_utils, self->discovery_event);
		self->~impl();
		return -EINVAL;
	} catch (...) {
		self->worker.reset();
		if (self->discovery_event != nullptr)
			spa_loop_utils_destroy_source(self->loop_utils,
					self->discovery_event);
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

/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include <dlfcn.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/buffers.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/parser.h>
#include <spa/support/log-impl.h>
#include <spa/support/plugin.h>

#include "egrabber.hpp"

namespace {

SPA_LOG_IMPL(logger);

constexpr uint32_t requested_buffers = 8;
constexpr uint32_t requested_frames = 10;

struct param_result {
	uint32_t expected = SPA_ID_INVALID;
	uint8_t storage[65536] = {};
	struct spa_pod *param = nullptr;
};

struct test_buffer {
	struct spa_buffer buffer = {};
	struct spa_data data = {};
	struct spa_chunk chunk = {};
	struct spa_meta metas[2] = {};
	struct spa_meta_header header = {};
	struct spa_meta_acquisition acquisition = {};
	void *payload = nullptr;
};

void on_result(void *data, int, int res, uint32_t type, const void *result)
{
	auto *capture = static_cast<param_result *>(data);

	spa_assert_se(res >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	const auto *params = static_cast<const struct spa_result_node_params *>(result);
	if (params->id != capture->expected || params->param == nullptr)
		return;
	const uint32_t size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = reinterpret_cast<struct spa_pod *>(capture->storage);
}

const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

struct spa_pod *enum_one(struct spa_node *node, param_result &capture,
		uint32_t id)
{
	capture.expected = id;
	capture.param = nullptr;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_OUTPUT, 0,
			id, 0, 1, nullptr) == 0);
	spa_assert_se(capture.param != nullptr);
	return capture.param;
}

struct spa_pod *enum_node_one(struct spa_node *node, param_result &capture,
		uint32_t id, uint32_t start = 0)
{
	capture.expected = id;
	capture.param = nullptr;
	spa_assert_se(spa_node_enum_params(node, 1, id, start, 1, nullptr) == 0);
	return capture.param;
}

struct spa_pod *find_control_value(struct spa_pod *props, const char *requested)
{
	auto *object = reinterpret_cast<struct spa_pod_object *>(props);
	struct spa_pod_prop *property;
	SPA_POD_OBJECT_FOREACH(object, property) {
		if (property->key != SPA_PROP_params)
			continue;
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		spa_pod_parser_pod(&parser, &property->value);
		spa_assert_se(spa_pod_parser_push_struct(&parser, &frame) == 0);
		while (true) {
			const char *name = nullptr;
			struct spa_pod *value = nullptr;
			if (spa_pod_parser_get_string(&parser, &name) < 0)
				break;
			spa_assert_se(spa_pod_parser_get_pod(&parser, &value) == 0);
			if (spa_streq(name, requested))
				return value;
		}
	}
	return nullptr;
}

struct spa_pod *build_control_write(uint8_t *storage, size_t size,
		const char *name, const struct spa_pod *value)
{
	spa_assert_se(size <= UINT32_MAX);
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			static_cast<uint32_t>(size));
	struct spa_pod_frame object, values;
	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &values);
	spa_pod_builder_string(&builder, name);
	spa_pod_builder_primitive(&builder, value);
	spa_pod_builder_pop(&builder, &values);
	return static_cast<struct spa_pod *>(spa_pod_builder_pop(&builder, &object));
}

void init_buffer(test_buffer &storage, uint32_t size, uint32_t alignment)
{
	alignment = SPA_MAX(alignment, static_cast<uint32_t>(alignof(max_align_t)));
	spa_assert_se((alignment & (alignment - 1u)) == 0);
	spa_assert_se(posix_memalign(&storage.payload, alignment, size) == 0);
	memset(storage.payload, 0, size);
	storage.data.type = SPA_DATA_MemPtr;
	storage.data.data = storage.payload;
	storage.data.maxsize = size;
	storage.data.fd = -1;
	storage.data.chunk = &storage.chunk;
	storage.metas[0] = {
		.type = SPA_META_Header,
		.size = sizeof(storage.header),
		.data = &storage.header,
	};
	storage.metas[1] = {
		.type = SPA_META_Acquisition,
		.size = sizeof(storage.acquisition),
		.data = &storage.acquisition,
	};
	storage.buffer.n_metas = SPA_N_ELEMENTS(storage.metas);
	storage.buffer.metas = storage.metas;
	storage.buffer.n_datas = 1;
	storage.buffer.datas = &storage.data;
}

uint64_t monotonic_nsec()
{
	struct timespec now;
	spa_assert_se(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return static_cast<uint64_t>(now.tv_sec) * SPA_NSEC_PER_SEC + now.tv_nsec;
}

int capture(const struct spa_handle_factory *factory, const char *producer)
{
	const struct spa_dict_item item = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_EGRABBER_PRODUCER, producer == nullptr ? "" : producer);
	const struct spa_dict info = SPA_DICT_INIT(&item,
			producer == nullptr ? 0u : 1u);
	const struct spa_dict *factory_info = producer == nullptr ? nullptr : &info;
	const struct spa_support support[] = {
		SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, &logger.log),
	};
	const size_t size = factory->get_size(factory, factory_info);
	std::unique_ptr<void, decltype(&free)> memory(calloc(1, size), free);
	spa_assert_se(memory != nullptr);
	auto *handle = static_cast<struct spa_handle *>(memory.get());
	const int initialized = factory->init(factory, handle, factory_info,
			support, SPA_N_ELEMENTS(support));
	if (initialized < 0)
		return 77;

	struct spa_node *node = nullptr;
	struct spa_hook listener;
	param_result params;
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			reinterpret_cast<void **>(&node)) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events, &params) == 0);

	struct spa_pod *prop_info = enum_node_one(node, params, SPA_PARAM_PropInfo);
	spa_assert_se(prop_info != nullptr);
	const char *first_control = nullptr;
	spa_assert_se(spa_pod_parse_object(prop_info,
			SPA_TYPE_OBJECT_PropInfo, nullptr,
			SPA_PROP_INFO_name, SPA_POD_String(&first_control)) >= 0);
	spa_assert_se(first_control != nullptr && first_control[0] != '\0');
	struct spa_pod *props = enum_node_one(node, params, SPA_PARAM_Props);
	spa_assert_se(props != nullptr);
	struct spa_pod *scalar_value = nullptr;
	const char *scalar_name = nullptr;
	for (const char *candidate : { "genicam.ExposureTime", "genicam.Gain",
			"genicam.AcquisitionFrameRate" }) {
		if ((scalar_value = find_control_value(props, candidate)) != nullptr) {
			scalar_name = candidate;
			break;
		}
	}
	spa_assert_se(scalar_name != nullptr && scalar_value != nullptr);
	uint8_t scalar_write_storage[512];
	struct spa_pod *scalar_write = build_control_write(scalar_write_storage,
			sizeof(scalar_write_storage), scalar_name, scalar_value);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, scalar_write) == 0);

	struct spa_pod *format = enum_one(node, params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	struct spa_video_info_raw video = {};
	spa_assert_se(spa_format_video_raw_parse(format, &video) >= 0);

	struct spa_pod *buffers_param = enum_one(node, params, SPA_PARAM_Buffers);
	int32_t payload_size = 0;
	int32_t alignment = 1;
	spa_assert_se(spa_pod_parse_object(buffers_param,
			SPA_TYPE_OBJECT_ParamBuffers, nullptr,
			SPA_PARAM_BUFFERS_size, SPA_POD_Int(&payload_size),
			SPA_PARAM_BUFFERS_align, SPA_POD_Int(&alignment)) >= 0);
	spa_assert_se(payload_size > 0);
	spa_assert_se(alignment > 0);

	std::vector<test_buffer> storage(requested_buffers);
	std::vector<struct spa_buffer *> buffers(requested_buffers);
	for (uint32_t i = 0; i < requested_buffers; i++) {
		init_buffer(storage[i], static_cast<uint32_t>(payload_size),
				static_cast<uint32_t>(alignment));
		buffers[i] = &storage[i].buffer;
	}
	struct spa_io_buffers_latest io = {};
	struct spa_io_buffers_latest_link link = {
		.id = 1,
		.flags = SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE,
		.io = &io,
		.notify_fd = -1,
	};
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers.data(), buffers.size()) == 0);
	format = enum_one(node, params, SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	props = enum_node_one(node, params, SPA_PARAM_Props);
	spa_assert_se(props != nullptr);
	if (struct spa_pod *width = find_control_value(props, "genicam.Width")) {
		uint8_t layout_write_storage[512];
		struct spa_pod *layout_write = build_control_write(layout_write_storage,
				sizeof(layout_write_storage), "genicam.Width", width);
		spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
				layout_write) == -EBUSY);
	}
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			scalar_write) == -EBUSY);

	const uint64_t deadline = monotonic_nsec() + 3 * SPA_NSEC_PER_SEC;
	uint32_t frames = 0;
	int64_t last_pts = SPA_TIME_INVALID;
	while (frames < requested_frames) {
		spa_assert_se(spa_node_process(node) >= SPA_STATUS_OK);
		uint64_t submission;
		uint32_t id;
		const int received = spa_io_buffers_latest_receive(&io, &submission, &id);
		if (received == -EPIPE) {
			spa_assert_se(monotonic_nsec() < deadline);
			continue;
		}
		spa_assert_se(received == 0);
		spa_assert_se(id < storage.size());
		spa_assert_se(storage[id].chunk.size > 0);
		spa_assert_se(storage[id].chunk.size <=
				static_cast<uint32_t>(payload_size));
		spa_assert_se(storage[id].header.pts != SPA_TIME_INVALID);
		if (last_pts != SPA_TIME_INVALID)
			spa_assert_se(storage[id].header.pts > last_pts);
		last_pts = storage[id].header.pts;
		spa_assert_se(spa_meta_acquisition_is_valid(&storage[id].metas[1]));
		spa_assert_se(spa_io_buffers_latest_complete(&io, id) == 0);
		frames++;
	}

	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	link.flags = 0;
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_IO_BuffersLatestLink, &link, sizeof(link)) == 0);
	format = enum_one(node, params, SPA_PARAM_Format);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			buffers.data(), buffers.size()) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, nullptr) == 0);
	props = enum_node_one(node, params, SPA_PARAM_Props);
	spa_assert_se(props != nullptr);
	struct spa_pod *width = find_control_value(props, "genicam.Width");
	spa_assert_se(width != nullptr);
	uint8_t layout_write_storage[512];
	struct spa_pod *layout_write = build_control_write(layout_write_storage,
			sizeof(layout_write_storage), "genicam.Width", width);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0,
			layout_write) == 0);
	format = enum_one(node, params, SPA_PARAM_EnumFormat);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, format) == 0);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	for (auto &item : storage)
		free(item.payload);
	return 0;
}

void reject_required_progressive_on_gigelink(
		const struct spa_handle_factory *factory)
{
	const struct spa_dict_item item = SPA_DICT_ITEM_INIT(
			SPA_KEY_API_EGRABBER_PROGRESSIVE, "require");
	const struct spa_dict info = SPA_DICT_INIT(&item, 1);
	const size_t size = factory->get_size(factory, &info);
	std::unique_ptr<void, decltype(&free)> memory(calloc(1, size), free);
	spa_assert_se(memory != nullptr);
	auto *handle = static_cast<struct spa_handle *>(memory.get());
	spa_assert_se(factory->init(factory, handle, &info, nullptr, 0) == -EINVAL);
}

} // namespace

int main(int argc, char **argv)
{
	spa_assert_se(argc == 2 || argc == 3);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != nullptr);
	auto enumerate = reinterpret_cast<spa_handle_factory_enum_func_t>(
			dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME));
	spa_assert_se(enumerate != nullptr);
	const struct spa_handle_factory *factory = nullptr;
	uint32_t index = 0;
	while (enumerate(&factory, &index) > 0 &&
			!spa_streq(factory->name, SPA_NAME_API_EGRABBER_SOURCE))
		factory = nullptr;
	spa_assert_se(factory != nullptr);
	const char *producer = argc == 3 ? argv[2] : nullptr;
	const int res = capture(factory, producer);
	if (res == 0 && producer == nullptr)
		reject_required_progressive_on_gigelink(factory);
	spa_assert_se(dlclose(library) == 0);
	return res;
}

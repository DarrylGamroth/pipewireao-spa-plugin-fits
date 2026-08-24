/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <cerrno>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

#include <spa/buffer/image-source-latest.h>
#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/parser.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

#include "camera.hpp"
#include "acquisition_key.hpp"
#include "buffer_memory.hpp"
#ifdef HAVE_EGRABBER_DRM
#include "dma_buf_sync.hpp"
#endif
#include "egrabber.hpp"
#include "frame_layout.hpp"
#include "frame_sequence.hpp"
#include "options.hpp"
#include "params.hpp"
#include "timestamp_mapper.hpp"

namespace {

namespace gc = Euresys::gc;
namespace ge = Euresys::ge;
using Euresys::Buffer;
using Euresys::BufferIndexRange;
using Euresys::NewBufferData;
using egrabber_pipewire::BufferMetadata;
using egrabber_pipewire::AcquisitionKeySequence;
using egrabber_pipewire::Camera;
using egrabber_pipewire::DeliveredFrameLayout;
using egrabber_pipewire::FrameSequence;
using egrabber_pipewire::NegotiatedFrameLayout;
using egrabber_pipewire::Options;
using egrabber_pipewire::TimestampMapper;

constexpr uint32_t max_buffers = SPA_IMAGE_SOURCE_MAX_BUFFERS;

struct buffer_slot {
	struct spa_image_source_buffer *image = nullptr;
	BufferIndexRange range;
	std::optional<Buffer> completed;
	uint64_t acquisition_generation = 0;
	uint64_t acquisition_sequence = 0;
	uint64_t sequence = 0;
	uint64_t dma_point = 0;
#ifdef HAVE_EGRABBER_DRM
	egrabber_pipewire::DmaBufTimeline dma_sync;
#endif
	bool acquisition_identity_valid = false;
	bool readout_observed = false;
	bool acquisition_discontinuity = false;
	bool frame_discontinuity = false;
	bool progressive = false;
	bool recycle_pending = false;
};

/* EGrabber acquires user buffers in the order they are submitted. */
struct buffer_submission_queue {
	buffer_slot *slots[max_buffers] = {};
	uint32_t head = 0;
	uint32_t tail = 0;
	uint32_t size = 0;

	void clear()
	{
		for (auto &slot : slots)
			slot = nullptr;
		head = 0;
		tail = 0;
		size = 0;
	}

	bool empty() const { return size == 0; }

	buffer_slot *front() const
	{
		return empty() ? nullptr : slots[head];
	}

	void submit(buffer_slot &slot)
	{
		if (size == max_buffers)
			throw std::runtime_error("eGrabber submission queue is full");
		slots[tail] = &slot;
		tail = tail + 1 == max_buffers ? 0 : tail + 1;
		size++;
	}

	void complete(buffer_slot &slot)
	{
		if (empty())
			throw std::runtime_error(
					"eGrabber completed an unsubmitted image slot");
		if (slots[head] != &slot)
			throw std::runtime_error(
					"eGrabber completed buffers out of submission order");
		slots[head] = nullptr;
		head = head + 1 == max_buffers ? 0 : head + 1;
		size--;
	}
};

uint64_t monotonic_nsec()
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		throw std::runtime_error("could not read CLOCK_MONOTONIC");
	return static_cast<uint64_t>(now.tv_sec) * SPA_NSEC_PER_SEC +
			static_cast<uint64_t>(now.tv_nsec);
}

struct port {
	uint64_t info_all = 0;
	struct spa_port_info info = SPA_PORT_INFO_INIT();
	struct spa_param_info params[5] = {};
	struct spa_video_info_raw format = {};
	bool have_format = false;
	uint32_t n_buffers = 0;
};

struct impl {
	struct spa_handle handle = {};
	struct spa_node node = {};
	struct spa_log *log = nullptr;
	struct spa_hook_list hooks = {};
	struct spa_callbacks callbacks = {};
	uint64_t info_all = 0;
	struct spa_node_info info = SPA_NODE_INFO_INIT();
	struct spa_param_info params[2] = {};
	struct spa_dict node_props = {};
	struct spa_dict_item node_items[24] = {};
	Options options;
	std::string node_name;
	std::string description;
	std::string interface_index;
	std::string device_index;
	std::string stream_index;
	std::string acquisition_domain;
	std::string acquisition_generation;
	std::string acquisition_sequence_context;
	port output;
	std::unique_ptr<Camera> camera;
#ifdef HAVE_EGRABBER_DRM
	std::unique_ptr<egrabber_pipewire::DmaBufSyncContext> dma_sync;
#endif
	struct spa_buffer_latest *latest = nullptr;
	struct spa_image_source_latest transport = {};
	struct spa_image_source source = {};
	buffer_slot slots[max_buffers];
	buffer_submission_queue submissions;
	std::vector<BufferIndexRange> ranges;
	FrameSequence frame_sequence;
	TimestampMapper timestamp_mapper;
	AcquisitionKeySequence acquisition_keys;
	std::optional<egrabber_pipewire::TransportEvent> pending_readout;
	buffer_slot *progressive_slot = nullptr;
	spa_fraction frame_rate = SPA_FRACTION(0, 1);
	uint32_t video_format = SPA_VIDEO_FORMAT_UNKNOWN;
	bool started = false;
	bool progressive_offered = false;
	bool progressive_active = false;
	bool dma_buf_offered = false;
	bool direct_dma_buf = false;
};

uint32_t acquisition_context(const Options &options,
		const egrabber_pipewire::TransportEvent &event)
{
	switch (options.acquisition_sequence_context) {
	case 1:
		return event.context1;
	case 2:
		return event.context2;
	case 3:
		return event.context3;
	default:
		throw std::runtime_error("invalid acquisition sequence context");
	}
}

void reset_observation(buffer_slot &slot)
{
	slot.acquisition_generation = 0;
	slot.acquisition_sequence = 0;
	slot.acquisition_identity_valid = false;
	slot.sequence = 0;
	slot.readout_observed = false;
	slot.acquisition_discontinuity = false;
	slot.frame_discontinuity = false;
	slot.progressive = false;
}

std::size_t delivered_line_pitch(const Camera &camera,
		const BufferMetadata &metadata)
{
	const auto natural = camera.natural_line_pitch();
	if (!metadata.x_padding)
		return natural;
	if (*metadata.x_padding > std::numeric_limits<std::size_t>::max() - natural)
		throw std::runtime_error("eGrabber line pitch overflows size_t");
	return natural + *metadata.x_padding;
}

void prepare_readout(impl *self, buffer_slot &slot,
		const egrabber_pipewire::TransportEvent &event,
		const egrabber_pipewire::BufferProgress &observation)
{
	if (slot.readout_observed)
		throw std::runtime_error("duplicate StartOfCameraReadout for one buffer");
	const auto frame = self->frame_sequence.next(observation.frame_id);
	slot.sequence = frame.sequence;
	slot.frame_discontinuity = frame.discontinuity;
	if (self->options.acquisition_domain) {
		const auto key = self->acquisition_keys.observe(
				acquisition_context(self->options, event));
		slot.acquisition_generation = key.generation;
		slot.acquisition_sequence = key.sequence;
		slot.sequence = key.sequence;
		slot.acquisition_identity_valid = true;
		slot.acquisition_discontinuity = key.discontinuity;
	}
	slot.readout_observed = true;
}

struct spa_meta_acquisition acquisition_metadata(const impl *self,
		const buffer_slot &slot)
{
	struct spa_meta_acquisition acquisition;
	if (!spa_meta_acquisition_init(&acquisition))
		throw std::runtime_error("could not initialize acquisition metadata");
	if (slot.acquisition_identity_valid &&
			!spa_meta_acquisition_set_identity(&acquisition,
					self->options.acquisition_domain->data(),
					slot.acquisition_generation,
					slot.acquisition_sequence))
		throw std::runtime_error("could not set eGrabber acquisition identity");
	return acquisition;
}

void begin_progressive(impl *self, buffer_slot &slot,
		const egrabber_pipewire::BufferProgress &observation)
{
	if (!self->progressive_active)
		return;
	if (self->progressive_slot != nullptr)
		throw std::runtime_error("overlapping progressive camera buffers");
	const auto acquisition = acquisition_metadata(self, slot);
	const auto payload_size = static_cast<uint32_t>(self->camera->payload_size());
	const auto granularity = static_cast<uint32_t>(
			self->camera->natural_line_pitch());
	const struct spa_image_frame frame = {
		.version = SPA_VERSION_IMAGE_FRAME,
		.data_index = 0,
		.header_flags = slot.frame_discontinuity ||
				slot.acquisition_discontinuity ||
				(self->options.acquisition_domain &&
				 !slot.acquisition_identity_valid)
			? SPA_META_HEADER_FLAG_DISCONT : 0u,
		.offset = 0,
		.size = payload_size,
		.stride = static_cast<int32_t>(granularity),
		.sequence = slot.sequence,
		.pts = SPA_TIME_INVALID,
		.acquisition = &acquisition,
	};
	const struct spa_image_progressive progressive = {
		.version = SPA_VERSION_IMAGE_PROGRESSIVE,
		.payload_size = payload_size,
		.commit_granularity = granularity,
		.committed = static_cast<uint32_t>(
				egrabber_pipewire::committed_prefix(observation.size_filled,
					payload_size, granularity)),
	};
	const int res = spa_image_source_begin_progressive(&self->source,
			slot.image, &frame, &progressive);
	if (res < 0)
		throw std::runtime_error("could not begin progressive eGrabber image");
	slot.progressive = true;
	self->progressive_slot = &slot;
}

bool poll_readout(impl *self)
{
	bool changed = false;
	if (self->pending_readout) {
		auto *slot = self->submissions.front();
		if (slot == nullptr)
			throw std::runtime_error(
					"StartOfCameraReadout has no submitted image slot");
		const auto observation = self->camera->buffer_progress(slot->range);
		if (observation) {
			prepare_readout(self, *slot, *self->pending_readout, *observation);
			begin_progressive(self, *slot, *observation);
			self->pending_readout.reset();
			changed = true;
		}
	}
	if (self->progressive_slot != nullptr) {
		const auto progress = self->camera->buffer_progress(
				self->progressive_slot->range);
		if (progress) {
			const auto committed = static_cast<uint32_t>(
					egrabber_pipewire::committed_prefix(progress->size_filled,
						self->camera->payload_size(),
						self->camera->natural_line_pitch()));
			const int res = spa_image_source_update_progressive(&self->source,
					self->progressive_slot->image, committed);
			if (res < 0)
				throw std::runtime_error(
						"could not update progressive eGrabber image");
			changed = changed || res > 0;
		}
	}
	return changed;
}

void finish_progressive(impl *self, buffer_slot &slot,
		const std::optional<egrabber_pipewire::ResolvedFrameLayout> &layout,
		const BufferMetadata &metadata, bool supported_payload)
{
	auto *meta = static_cast<struct spa_meta_progressive *>(
			spa_buffer_find_meta_data(slot.image->buffer,
					SPA_META_Progressive,
					sizeof(struct spa_meta_progressive)));
	if (meta == nullptr)
		throw std::runtime_error("active progressive metadata disappeared");
	uint32_t current = 0;
	enum spa_meta_progressive_state current_state;
	uint32_t terminal_flags = 0;
	if (!spa_meta_progressive_snapshot_decode(
			spa_meta_progressive_load_acquire(meta), &current,
			&current_state) || current_state != SPA_META_PROGRESSIVE_STATE_ACTIVE)
		terminal_flags |= SPA_META_PROGRESSIVE_FLAG_PROTOCOL_ERROR;
	if (!layout) {
		terminal_flags |= SPA_META_PROGRESSIVE_FLAG_INVALID_LAYOUT;
	} else {
		if (layout->incomplete)
			terminal_flags |= SPA_META_PROGRESSIVE_FLAG_INCOMPLETE;
		if (layout->corrupted)
			terminal_flags |= SPA_META_PROGRESSIVE_FLAG_CORRUPTED;
		if (layout->image_offset != 0 ||
				layout->data_size != self->camera->payload_size() ||
				layout->line_pitch != self->camera->natural_line_pitch())
			terminal_flags |= SPA_META_PROGRESSIVE_FLAG_INVALID_LAYOUT;
	}
	if (!supported_payload)
		terminal_flags |= SPA_META_PROGRESSIVE_FLAG_PROTOCOL_ERROR;
	if (metadata.size_filled &&
			*metadata.size_filled > self->camera->payload_size())
		terminal_flags |= SPA_META_PROGRESSIVE_FLAG_PROTOCOL_ERROR;

	const bool complete = terminal_flags == 0;
	const auto observed = metadata.size_filled.value_or(current);
	const auto prefix = static_cast<uint32_t>(
			egrabber_pipewire::committed_prefix(observed,
					self->camera->payload_size(),
					self->camera->natural_line_pitch()));
	const uint32_t committed = complete
		? static_cast<uint32_t>(self->camera->payload_size())
		: std::max(current, prefix);
	const int res = spa_image_source_finish_progressive(&self->source,
			slot.image, committed,
			complete ? SPA_META_PROGRESSIVE_STATE_COMPLETE
				: SPA_META_PROGRESSIVE_STATE_ABORTED,
			terminal_flags);
	if (res < 0)
		throw std::runtime_error("could not finish progressive eGrabber image");
	if (self->progressive_slot == &slot)
		self->progressive_slot = nullptr;
	slot.progressive = false;
}

uint32_t video_format(const Camera &camera)
{
	const std::string &format = camera.pixel_format();

	if (format == "Mono8")
		return SPA_VIDEO_FORMAT_GRAY8;
	if (format == "Mono10" || format == "Mono12" ||
			format == "Mono14" || format == "Mono16")
		return SPA_VIDEO_FORMAT_GRAY16_LE;
	throw std::runtime_error("unsupported zero-copy eGrabber pixel format: " + format);
}

spa_fraction frame_rate(double value)
{
	constexpr uint32_t denominator = 1000;
	if (value > static_cast<double>(
			std::numeric_limits<uint32_t>::max() / denominator))
		throw std::runtime_error("eGrabber frame rate exceeds the SPA fraction range");
	const auto numerator = static_cast<uint32_t>(std::llround(value * denominator));
	const auto divisor = std::gcd(numerator, denominator);

	return SPA_FRACTION(numerator / divisor, denominator / divisor);
}

void emit_node_info(impl *self, bool full)
{
	uint64_t old = full ? self->info.change_mask : 0;

	if (full)
		self->info.change_mask = self->info_all;
	if (self->info.change_mask != 0) {
		spa_node_emit_info(&self->hooks, &self->info);
		self->info.change_mask = old;
	}
}

void emit_port_info(impl *self, bool full)
{
	port &output = self->output;
	uint64_t old = full ? output.info.change_mask : 0;

	if (full)
		output.info.change_mask = output.info_all;
	if (output.info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_OUTPUT, 0,
				&output.info);
		output.info.change_mask = old;
	}
}

int add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	auto *self = static_cast<impl *>(object);
	struct spa_hook_list save;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_node_info(self, true);
	emit_port_info(self, true);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

int set_callbacks(void *object, const struct spa_node_callbacks *callbacks,
		void *data)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

int enum_params(void *object, int seq, uint32_t id, uint32_t start,
		uint32_t num, const struct spa_pod *filter)
{
	auto *self = static_cast<impl *>(object);
	struct spa_pod_dynamic_builder dynamic;
	struct spa_pod_builder_state state;
	struct spa_result_node_params result = {};
	uint8_t storage[4096];
	uint32_t count = 0;
	int res = 0;

	spa_return_val_if_fail(self != nullptr && num > 0, -EINVAL);
	if (id != SPA_PARAM_PropInfo && id != SPA_PARAM_Props)
		return -ENOENT;
	spa_pod_dynamic_builder_init(&dynamic, storage, sizeof(storage), 4096);
	spa_pod_builder_get_state(&dynamic.b, &state);
	result.id = id;
	result.next = start;
	try {
		while (count < num) {
			struct spa_pod *param = nullptr;
			result.index = result.next++;
			spa_pod_builder_reset(&dynamic.b, &state);
			if (id == SPA_PARAM_PropInfo) {
				const auto *feature = egrabber_pipewire::scalar_feature_at(
						*self->camera, result.index);
				if (feature == nullptr)
					break;
				param = egrabber_pipewire::build_feature_prop_info(
						*self->camera, *feature, &dynamic.b);
			} else {
				if (result.index > 0)
					break;
				param = egrabber_pipewire::build_feature_props(
						*self->camera, &dynamic.b);
			}
			if (param == nullptr)
				continue;
			if (spa_pod_filter(&dynamic.b, &result.param, param, filter) < 0)
				continue;
			spa_node_emit_result(&self->hooks, seq, 0,
					SPA_RESULT_TYPE_NODE_PARAMS, &result);
			count++;
		}
	} catch (...) {
		res = -EIO;
	}
	spa_pod_dynamic_builder_clean(&dynamic);
	return res;
}

int validate_pixel_format_write(const egrabber_pipewire::Feature &feature,
		const struct spa_pod *value)
{
	if (feature.name != "PixelFormat")
		return 0;
	int32_t index;
	uint32_t id;
	if (spa_pod_get_int(value, &index) < 0) {
		if (spa_pod_get_id(value, &id) < 0 || id > INT32_MAX)
			return -EINVAL;
		index = static_cast<int32_t>(id);
	}
	if (index < 0 || static_cast<size_t>(index) >= feature.enum_entries.size())
		return -EINVAL;
	const auto &format = feature.enum_entries[index];
	return format == "Mono8" || format == "Mono10" || format == "Mono12" ||
			format == "Mono14" || format == "Mono16" ? 0 : -ENOTSUP;
}

int refresh_layout_params(impl *self)
{
	try {
		self->video_format = video_format(*self->camera);
		self->frame_rate = SPA_FRACTION(0, 1);
		if (const auto rate = self->camera->frame_rate(); rate && *rate > 0.0)
			self->frame_rate = frame_rate(*rate);
	} catch (...) {
		return -EIO;
	}
	self->output.have_format = false;
	self->output.params[0].flags ^= SPA_PARAM_INFO_SERIAL;
	self->output.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	self->output.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
	self->output.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(self, false);
	return 0;
}

int set_param(void *object, uint32_t id, uint32_t,
		const struct spa_pod *param)
{
	auto *self = static_cast<impl *>(object);
	const char *name = nullptr;
	struct spa_pod *value = nullptr;
	uint32_t operations = 0;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	if (id != SPA_PARAM_Props)
		return -ENOENT;
	if (param == nullptr)
		return 0;
	if (!spa_pod_is_object(param) ||
			SPA_POD_OBJECT_TYPE(param) != SPA_TYPE_OBJECT_Props)
		return -EINVAL;
	auto *props = const_cast<struct spa_pod_object *>(
			reinterpret_cast<const struct spa_pod_object *>(param));
	struct spa_pod_prop *property;
	SPA_POD_OBJECT_FOREACH(props, property) {
		if (property->key != SPA_PROP_params)
			continue;
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		spa_pod_parser_pod(&parser, &property->value);
		if (spa_pod_parser_push_struct(&parser, &frame) < 0)
			return -EINVAL;
		while (true) {
			const char *candidate = nullptr;
			struct spa_pod *candidate_value = nullptr;
			if (spa_pod_parser_get_string(&parser, &candidate) < 0)
				break;
			if (spa_pod_parser_get_pod(&parser, &candidate_value) < 0)
				return -EINVAL;
			if (++operations > 1)
				return -EINVAL;
			name = candidate;
			value = candidate_value;
		}
	}
	if (operations == 0)
		return 0;
	const auto found = std::find_if(self->camera->features().begin(),
			self->camera->features().end(), [name](const auto &feature) {
			return egrabber_pipewire::is_scalar_feature(feature) &&
					feature.property_name == name;
		});
	if (found == self->camera->features().end())
		return -ENOENT;
	if (!found->writeable)
		return -EACCES;
	if (self->started)
		return -EBUSY;
	const bool layout_change = egrabber_pipewire::changes_payload_layout(*found);
	if (layout_change && self->output.n_buffers != 0)
		return -EBUSY;
	int res = validate_pixel_format_write(*found, value);
	if (res < 0)
		return res;
	try {
		self->camera->set_feature(*found, value, layout_change);
	} catch (const egrabber_pipewire::BufferChangeRequired &) {
		return -EBUSY;
	} catch (...) {
		return -EIO;
	}
	if (layout_change && (res = refresh_layout_params(self)) < 0)
		return res;
	self->params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	self->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	emit_node_info(self, false);
	return 0;
}

int set_io(void *, uint32_t, void *, size_t)
{
	return -ENOTSUP;
}

int add_port(void *, enum spa_direction, uint32_t, const struct spa_dict *)
{
	return -ENOTSUP;
}

int remove_port(void *, enum spa_direction, uint32_t)
{
	return -ENOTSUP;
}

int build_port_param(impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	const Camera &camera = *self->camera;
	port &output = self->output;
	const struct spa_rectangle size = SPA_RECTANGLE(
			static_cast<uint32_t>(camera.width()),
			static_cast<uint32_t>(camera.height()));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(self->video_format),
				SPA_FORMAT_VIDEO_size,
				SPA_POD_Rectangle(&size),
				SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&self->frame_rate));
		return 1;
	case SPA_PARAM_Format:
		if (index > 0)
			return 0;
		if (!output.have_format)
			return -EIO;
		*param = spa_format_video_raw_build(builder, id, &output.format);
		return 1;
	case SPA_PARAM_Buffers:
		if (index == 0 && self->dma_buf_offered) {
			struct spa_pod_frame object;
			spa_pod_builder_push_object(builder, &object,
					SPA_TYPE_OBJECT_ParamBuffers, id);
			spa_pod_builder_add(builder,
					SPA_PARAM_BUFFERS_buffers,
					SPA_POD_CHOICE_RANGE_Int(
							static_cast<int32_t>(camera.buffer_count()),
							static_cast<int32_t>(camera.announce_minimum()),
							static_cast<int32_t>(max_buffers)),
					SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(3),
					SPA_PARAM_BUFFERS_size,
					SPA_POD_Int(static_cast<int32_t>(camera.payload_size())),
					SPA_PARAM_BUFFERS_stride,
					SPA_POD_Int(static_cast<int32_t>(camera.natural_line_pitch())),
					SPA_PARAM_BUFFERS_align,
					SPA_POD_Int(static_cast<int32_t>(camera.buffer_alignment())),
					SPA_PARAM_BUFFERS_dataType,
					SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_DmaBuf));
			spa_pod_builder_prop(builder, SPA_PARAM_BUFFERS_metaType,
					SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_int(builder, 1u << SPA_META_SyncTimeline);
			*param = static_cast<struct spa_pod *>(
					spa_pod_builder_pop(builder, &object));
			return 1;
		}
		if (index != (self->dma_buf_offered ? 1u : 0u))
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(
						static_cast<int32_t>(camera.buffer_count()),
						static_cast<int32_t>(camera.announce_minimum()),
						static_cast<int32_t>(max_buffers)),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int(static_cast<int32_t>(camera.payload_size())),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int(static_cast<int32_t>(camera.natural_line_pitch())),
				SPA_PARAM_BUFFERS_align,
				SPA_POD_Int(static_cast<int32_t>(camera.buffer_alignment())),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return 1;
	case SPA_PARAM_Meta:
		if (index == 0) {
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_header)));
			return 1;
		}
		if (index == 1) {
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_acquisition)));
			return 1;
		}
		if (index == 2 && self->progressive_offered) {
			*param = spa_pod_builder_add_object(builder,
					SPA_TYPE_OBJECT_ParamMeta, id,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Progressive),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_progressive)));
			return 1;
		}
		if (index == (self->progressive_offered ? 3u : 2u) &&
				self->dma_buf_offered) {
			struct spa_pod_frame object;
			spa_pod_builder_push_object(builder, &object,
					SPA_TYPE_OBJECT_ParamMeta, id);
			spa_pod_builder_add(builder,
					SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline),
					SPA_PARAM_META_size,
					SPA_POD_Int(sizeof(struct spa_meta_sync_timeline)));
			spa_pod_builder_prop(builder, SPA_PARAM_META_features,
					SPA_POD_PROP_FLAG_DROP);
			spa_pod_builder_int(builder,
					SPA_META_FEATURE_SYNC_TIMELINE_RELEASE);
			*param = static_cast<struct spa_pod *>(
					spa_pod_builder_pop(builder, &object));
			return 1;
		}
		return 0;
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_BuffersLatestLink),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers_latest_link)));
		return 1;
	default:
		return -ENOENT;
	}
}

int port_enum_params(void *object, int seq, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	auto *self = static_cast<impl *>(object);
	struct spa_pod_builder builder;
	struct spa_result_node_params result;
	uint8_t buffer[1024];
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod *param;

		result.index = result.next++;
		spa_pod_builder_init(&builder, buffer, sizeof(buffer));
		res = build_port_param(self, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

int release_buffers(impl *self);

int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t,
		const struct spa_pod *param)
{
	auto *self = static_cast<impl *>(object);
	struct spa_video_info_raw format;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (param == nullptr) {
		if (self->started)
			return -EBUSY;
		if (self->output.n_buffers != 0) {
			const int res = release_buffers(self);
			if (res < 0)
				return res;
		}
		self->output.have_format = false;
		return 0;
	}
	spa_zero(format);
	if (spa_format_video_raw_parse(param, &format) < 0)
		return -EINVAL;
	if (self->output.have_format &&
			(self->started || self->output.n_buffers != 0)) {
		const auto &bound = self->output.format;
		if (format.format == bound.format &&
				format.size.width == bound.size.width &&
				format.size.height == bound.size.height &&
				format.framerate.num == bound.framerate.num &&
				format.framerate.denom == bound.framerate.denom)
			return 0;
		return -EBUSY;
	}
	const Camera &camera = *self->camera;
	if (format.format != self->video_format ||
			format.size.width != camera.width() ||
			format.size.height != camera.height())
		return -EINVAL;
	self->output.format = format;
	self->output.have_format = true;
	return 0;
}

int release_buffers(impl *self)
{
	int res = 0;
	if (spa_buffer_latest_has_links(self->latest))
		return -EBUSY;

	const auto cleanup = [&res](auto &&operation) noexcept {
		try {
			operation();
		} catch (...) {
			if (res == 0)
				res = -EIO;
		}
	};
	cleanup([&] { self->camera->clear_frame_callback(); });
	cleanup([&] { self->camera->set_transport_event_callback({}); });
	cleanup([&] { self->camera->disable_events(); });
	for (uint32_t index = 0; index < self->output.n_buffers; index++)
		self->slots[index].completed.reset();
	if (!self->ranges.empty())
		cleanup([&] { self->camera->release(self->ranges); });
	self->ranges.clear();
	self->submissions.clear();
	for (uint32_t index = 0; index < self->output.n_buffers; index++) {
		auto &slot = self->slots[index];
		if (slot.image != nullptr)
			spa_image_source_buffer_set_user_data(slot.image, nullptr);
		slot.image = nullptr;
		slot.recycle_pending = false;
		slot.dma_point = 0;
#ifdef HAVE_EGRABBER_DRM
		slot.dma_sync = {};
#endif
		reset_observation(slot);
	}
	self->pending_readout.reset();
	self->progressive_slot = nullptr;
	self->progressive_active = false;
	self->direct_dma_buf = false;
	if (self->output.n_buffers != 0) {
		int teardown = spa_image_source_latest_teardown(
				&self->transport, &self->source);
		if (res == 0)
			res = teardown;
		if (teardown == 0)
			self->output.n_buffers = 0;
	}
	return res;
}

int port_use_buffers(void *object, enum spa_direction direction, uint32_t,
		uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	auto *self = static_cast<impl *>(object);
	uint32_t i;
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return release_buffers(self);
	if (self->output.n_buffers != 0 && (res = release_buffers(self)) < 0)
		return res;
	if (!self->output.have_format || buffers == nullptr || n_buffers > max_buffers ||
			n_buffers < self->camera->announce_minimum())
		return -EINVAL;
	std::vector<egrabber_pipewire::BufferMemoryOffer> offers;
	offers.reserve(n_buffers);
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == nullptr || buffers[i]->n_datas == 0 ||
				(data = &buffers[i]->datas[0])->chunk == nullptr ||
				data->maxsize < self->camera->payload_size())
			return -EINVAL;
		egrabber_pipewire::OfferedMemory type;
		switch (data->type) {
		case SPA_DATA_MemPtr:
			type = egrabber_pipewire::OfferedMemory::mem_ptr;
			break;
		case SPA_DATA_MemFd:
			type = egrabber_pipewire::OfferedMemory::mem_fd;
			break;
		case SPA_DATA_DmaBuf:
			type = egrabber_pipewire::OfferedMemory::dma_buf;
			break;
		default:
			return -ENOTSUP;
		}
		offers.push_back({type, data->data != nullptr});
	}
	const auto memory = egrabber_pipewire::choose_announced_memory(
			offers, self->dma_buf_offered);
	if (memory == egrabber_pipewire::AnnouncedMemory::unavailable)
		return -ENOTSUP;
	const bool direct_dma_buf = memory ==
			egrabber_pipewire::AnnouncedMemory::direct_dma_buf;
	if (direct_dma_buf &&
			std::popcount(spa_buffer_latest_active_mask(self->latest)) > 1)
		return -EBUSY;
	bool progressive_metadata = self->progressive_offered && !direct_dma_buf;
	for (i = 0; i < n_buffers; i++) {
		const struct spa_data *data = &buffers[i]->datas[0];
		if (direct_dma_buf) {
			if (data->type != SPA_DATA_DmaBuf || data->fd < 0 ||
					data->mapoffset % self->camera->buffer_alignment() != 0 ||
					buffers[i]->n_datas < 3 ||
					spa_buffer_find_meta_data(buffers[i],
							SPA_META_SyncTimeline,
							sizeof(struct spa_meta_sync_timeline)) == nullptr)
				return -EINVAL;
		} else if ((data->type != SPA_DATA_MemPtr &&
				data->type != SPA_DATA_MemFd) || data->data == nullptr ||
				reinterpret_cast<uintptr_t>(data->data) %
						self->camera->buffer_alignment() != 0) {
			return -EINVAL;
		}
		progressive_metadata = progressive_metadata &&
				spa_buffer_find_meta_data(buffers[i], SPA_META_Progressive,
						sizeof(struct spa_meta_progressive)) != nullptr;
	}
	if (self->options.progressive ==
			egrabber_pipewire::ProgressivePolicy::require &&
			!progressive_metadata)
		return -ENOTSUP;
	self->progressive_active = progressive_metadata;
	self->direct_dma_buf = direct_dma_buf;
	res = spa_image_source_latest_prepare(&self->transport, &self->source,
			buffers, n_buffers);
	if (res < 0) {
		self->progressive_active = false;
		self->direct_dma_buf = false;
		return res;
	}
	self->output.n_buffers = n_buffers;
	try {
		self->ranges.reserve(n_buffers);
		self->camera->select_memory_type(self->direct_dma_buf);
		for (i = 0; i < n_buffers; i++) {
			struct spa_image_source_buffer *image = nullptr;
			struct spa_data *data = &buffers[i]->datas[0];

			if (spa_image_source_try_acquire(&self->source, &image) != 1 ||
					image == nullptr)
				throw std::runtime_error("PipeWireAO image pool is incomplete");
			buffer_slot &slot = self->slots[i];
			slot.image = image;
			spa_image_source_buffer_set_user_data(image, &slot);
#ifdef HAVE_EGRABBER_DRM
			if (self->direct_dma_buf) {
				if (!self->dma_sync)
					throw std::runtime_error(
							"DMA-BUF synchronization context is unavailable");
				slot.dma_sync = self->dma_sync->import(buffers[i]);
				if (!slot.dma_sync.release_ready())
					throw std::runtime_error(
							"DMA-BUF release timeline is still pending during setup");
			}
#endif
			slot.range = self->camera->announce(data->data, data->fd,
					self->camera->payload_size(), data->mapoffset,
					self->direct_dma_buf, &slot);
			self->ranges.push_back(slot.range);
			self->submissions.submit(slot);
		}
		self->camera->set_frame_callback([self](const NewBufferData &data) {
			auto *slot = static_cast<buffer_slot *>(data.userPointer);
			if (slot == nullptr || slot->image == nullptr)
				throw std::runtime_error("eGrabber completion has no image slot");
			self->submissions.complete(*slot);

			Buffer completed(data);
			const BufferMetadata metadata = self->camera->buffer_metadata(completed);
			const bool supported_payload = !metadata.payload_type ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_UNKNOWN ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_IMAGE ||
					*metadata.payload_type == gc::PAYLOAD_TYPE_CHUNK_DATA;
			std::optional<egrabber_pipewire::ResolvedFrameLayout> layout;
			try {
				layout = egrabber_pipewire::resolve_frame_layout(
						NegotiatedFrameLayout{
							self->camera->width(), self->camera->height(),
							self->camera->offset_x(), self->camera->offset_y(),
							self->camera->natural_line_pitch(),
							self->camera->payload_size(), self->camera->pixel_format(),
						},
						DeliveredFrameLayout{
							self->camera->width(), self->camera->height(),
							delivered_line_pitch(*self->camera, metadata),
							self->camera->pixel_format(), self->camera->offset_x(),
							self->camera->offset_y(),
							metadata.image_offset, metadata.size_filled,
							metadata.data_size, metadata.x_padding,
							metadata.image_present, metadata.data_larger_than_buffer,
							supported_payload, metadata.incomplete.value_or(false),
						});
			} catch (const std::runtime_error &) {
				if (!self->progressive_active)
					throw;
			}
			if (layout && layout->line_pitch >
					static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
				if (!self->progressive_active)
					throw std::runtime_error("eGrabber line pitch exceeds SPA stride");
				layout.reset();
			}
			if (!slot->readout_observed && self->pending_readout) {
				const egrabber_pipewire::BufferProgress observation = {
					.size_filled = metadata.size_filled.value_or(0),
					.frame_id = metadata.frame_id,
				};
				prepare_readout(self, *slot, *self->pending_readout, observation);
				begin_progressive(self, *slot, observation);
				self->pending_readout.reset();
			}
			if (!slot->readout_observed) {
				const auto sequence = self->frame_sequence.next(metadata.frame_id);
				slot->sequence = sequence.sequence;
				slot->frame_discontinuity = sequence.discontinuity;
			}
			if (self->progressive_active && !slot->progressive)
				throw std::runtime_error(
						"progressive completion had no StartOfCameraReadout");
			slot->completed.emplace(std::move(completed));
			if (slot->progressive) {
				finish_progressive(self, *slot, layout, metadata,
						supported_payload);
				return;
			}
			if (!layout)
				throw std::runtime_error("eGrabber delivered an invalid frame layout");
			const auto timestamp = self->timestamp_mapper.map(
					metadata.timestamp_ns.value_or(0),
					monotonic_nsec());
			const auto acquisition = acquisition_metadata(self, *slot);
			struct spa_image_frame frame = {
				.version = SPA_VERSION_IMAGE_FRAME,
				.data_index = 0,
				.header_flags = slot->frame_discontinuity || timestamp.discontinuity ||
						slot->acquisition_discontinuity ||
						(self->options.acquisition_domain &&
						 !slot->acquisition_identity_valid)
					? SPA_META_HEADER_FLAG_DISCONT : 0u,
				.chunk_flags = layout->corrupted ? SPA_CHUNK_FLAG_CORRUPTED : 0u,
				.offset = static_cast<uint32_t>(layout->image_offset),
				.size = static_cast<uint32_t>(layout->data_size),
				.stride = static_cast<int32_t>(layout->line_pitch),
				.sequence = slot->sequence,
				.pts = timestamp.pts,
				.acquisition = &acquisition,
			};
#ifdef HAVE_EGRABBER_DRM
			if (self->direct_dma_buf) {
				if (!slot->dma_sync)
					throw std::runtime_error(
							"DMA-BUF completion has no synchronization timeline");
				slot->dma_sync.signal_acquire(++slot->dma_point);
			}
#endif
			const int publish = spa_image_source_publish_complete(
					&self->source, slot->image, &frame);
			if (publish < 0) {
				auto failed = std::move(slot->completed);
				reset_observation(*slot);
				self->camera->recycle(*failed);
				self->submissions.submit(*slot);
				throw std::runtime_error("could not publish eGrabber image");
			}
		});
		self->camera->set_transport_event_callback(
				[self](const egrabber_pipewire::TransportEvent &event) {
			if (event.kind == egrabber_pipewire::TransportEventKind::data_stream &&
					event.id ==
					ge::EVENT_DATA_NUMID_DATASTREAM_START_OF_CAMERA_READOUT &&
					(self->options.acquisition_domain ||
					 self->progressive_active)) {
				if (self->pending_readout || self->progressive_slot != nullptr)
					throw std::runtime_error(
							"overlapping StartOfCameraReadout events");
				self->pending_readout = event;
			}
			if (event.kind == egrabber_pipewire::TransportEventKind::data_stream &&
					event.id == ge::EVENT_DATA_NUMID_DATASTREAM_FAILURE)
				throw std::runtime_error("eGrabber reported a data-stream failure");
		});
	} catch (...) {
		const auto cleanup = [](auto &&operation) noexcept {
			try {
				operation();
			} catch (...) {
			}
		};
		cleanup([&] { self->camera->clear_frame_callback(); });
		cleanup([&] { self->camera->set_transport_event_callback({}); });
		if (!self->ranges.empty())
			cleanup([&] { self->camera->release(self->ranges); });
		self->ranges.clear();
		self->submissions.clear();
		self->progressive_slot = nullptr;
		self->progressive_active = false;
		self->direct_dma_buf = false;
		for (auto &slot : self->slots) {
			if (slot.image != nullptr)
				spa_image_source_buffer_set_user_data(slot.image, nullptr);
			slot.image = nullptr;
			slot.completed.reset();
			slot.recycle_pending = false;
			slot.dma_point = 0;
#ifdef HAVE_EGRABBER_DRM
			slot.dma_sync = {};
#endif
			reset_observation(slot);
		}
		(void) spa_image_source_latest_teardown(&self->transport, &self->source);
		self->output.n_buffers = 0;
		return -EIO;
	}
	return 0;
}

int port_set_io(void *object, enum spa_direction direction, uint32_t port_id,
		uint32_t id, void *data, size_t size)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_OUTPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_BuffersLatest && id != SPA_IO_BuffersLatestNotify &&
			id != SPA_IO_BuffersLatestLink)
		return -ENOENT;
	if (self->direct_dma_buf && id != SPA_IO_BuffersLatestNotify) {
		uint32_t link_id = 0;
		bool active = data != nullptr;
		if (id == SPA_IO_BuffersLatestLink && data != nullptr &&
				size >= sizeof(struct spa_io_buffers_latest_link)) {
			const auto *link =
					static_cast<const struct spa_io_buffers_latest_link *>(data);
			link_id = link->id;
			active = SPA_FLAG_IS_SET(link->flags,
					SPA_IO_BUFFERS_LATEST_LINK_FLAG_ACTIVE);
		}
		if (active && spa_buffer_latest_find_link(self->latest, link_id,
				nullptr) == nullptr && spa_buffer_latest_has_links(self->latest))
			return -EBUSY;
	}
	return spa_buffer_latest_set_io(self->latest, id, data, size);
}

int reuse_buffer(void *, uint32_t, uint32_t)
{
	return -ENOTSUP;
}

int recycle_slot(impl *self, buffer_slot &slot)
{
	if (!slot.recycle_pending)
		return 0;
#ifdef HAVE_EGRABBER_DRM
	if (self->direct_dma_buf && !slot.dma_sync.release_ready())
		return 0;
#endif
	auto completed = std::move(slot.completed);
	if (!completed)
		return -EPROTO;
	slot.recycle_pending = false;
	reset_observation(slot);
	self->camera->recycle(*completed);
	self->submissions.submit(slot);
	return 1;
}

int recycle_buffers(impl *self, bool reclaim)
{
	bool changed = false;
	uint32_t count;

	for (uint32_t index = 0; index < self->output.n_buffers; index++) {
		auto &slot = self->slots[index];
		const int res = recycle_slot(self, slot);
		if (res < 0)
			return res;
		changed = changed || res > 0;
		if (reclaim && changed)
			return 1;
	}

	for (count = 0; count < self->output.n_buffers; count++) {
		struct spa_image_source_buffer *image = nullptr;
		const int res = reclaim
			? spa_image_source_try_reclaim_submission(&self->source, &image)
			: spa_image_source_try_acquire(&self->source, &image);
		if (res < 0)
			return res;
		if (res == 0)
			return changed ? 1 : 0;
		auto *slot = static_cast<buffer_slot *>(
				spa_image_source_buffer_get_user_data(image));
		if (slot == nullptr || slot->image != image || !slot->completed)
			return -EPROTO;
		slot->recycle_pending = true;
		const int recycled = recycle_slot(self, *slot);
		if (recycled < 0)
			return recycled;
		changed = changed || recycled > 0;
		if (reclaim && recycled > 0)
			return 1;
	}
	return changed ? 1 : 0;
}

int process(void *object)
{
	auto *self = static_cast<impl *>(object);

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	if (!self->started)
		return SPA_STATUS_OK;
	try {
		bool changed = false;
		int res = recycle_buffers(self, false);
		if (res < 0)
			return res;
		changed = res > 0;
		const bool processed = self->camera->process_event();
		changed = poll_readout(self) || changed;
		res = recycle_buffers(self, false);
		if (res < 0)
			return res;
		changed = changed || processed || res > 0;
		if (!processed && self->submissions.empty()) {
			res = recycle_buffers(self, true);
			if (res < 0)
				return res;
			changed = changed || res > 0;
		}
		return changed ? SPA_STATUS_HAVE_DATA : SPA_STATUS_OK;
	} catch (...) {
		return -EIO;
	}
}

int send_command(void *object, const struct spa_command *command)
{
	auto *self = static_cast<impl *>(object);
	int res;

	spa_return_val_if_fail(self != nullptr, -EINVAL);
	spa_return_val_if_fail(command != nullptr, -EINVAL);
	try {
		switch (SPA_NODE_COMMAND_ID(command)) {
		case SPA_NODE_COMMAND_Start:
			if (!self->output.have_format || self->output.n_buffers == 0 ||
					!spa_buffer_latest_has_links(self->latest))
				return -EIO;
			if (self->started)
				return 0;
			if (self->direct_dma_buf &&
					std::popcount(spa_buffer_latest_active_mask(self->latest)) > 1)
				return -EBUSY;
			if ((res = spa_buffer_latest_worker_begin(self->latest)) < 0)
				return res;
			self->started = true;
			self->frame_sequence.reset();
			self->timestamp_mapper.request_reset();
			if (self->options.acquisition_domain)
				self->acquisition_keys.start();
			self->pending_readout.reset();
			self->progressive_slot = nullptr;
			self->camera->start();
			return 0;
		case SPA_NODE_COMMAND_Pause:
		case SPA_NODE_COMMAND_Suspend:
			if (!self->started)
				return 0;
			self->camera->stop();
			for (uint32_t count = 0;
					self->progressive_slot != nullptr &&
					count < self->output.n_buffers * 4u + 16u;
					count++) {
				if (!self->camera->process_event())
					break;
				(void) poll_readout(self);
			}
			if (self->progressive_slot != nullptr)
				throw std::runtime_error(
						"camera stopped with an active progressive image");
			self->started = false;
			self->pending_readout.reset();
			return spa_buffer_latest_worker_end(self->latest);
		default:
			return -ENOTSUP;
		}
	} catch (...) {
		if (self->started) {
			self->started = false;
			(void) spa_buffer_latest_worker_end(self->latest);
		}
		return -EIO;
	}
}

const struct spa_node_methods node_methods = {
	.version = SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.set_callbacks = set_callbacks,
	.enum_params = enum_params,
	.set_param = set_param,
	.set_io = set_io,
	.send_command = send_command,
	.add_port = add_port,
	.remove_port = remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = reuse_buffer,
	.process = process,
};

int get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto *self = reinterpret_cast<impl *>(handle);

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	spa_return_val_if_fail(interface != nullptr, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

int clear(struct spa_handle *handle)
{
	auto *self = reinterpret_cast<impl *>(handle);
	int res = 0;

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	if (self->started) {
		try {
			self->camera->stop();
		} catch (...) {
			res = -EIO;
		}
		self->started = false;
		const int ended = spa_buffer_latest_worker_end(self->latest);
		if (res == 0)
			res = ended;
	}
	if (self->output.n_buffers != 0) {
		const int released = release_buffers(self);
		if (res == 0)
			res = released;
	}
	spa_buffer_latest_destroy(self->latest);
	self->latest = nullptr;
	self->~impl();
	return res;
}

size_t get_size(const struct spa_handle_factory *, const struct spa_dict *)
{
	return sizeof(impl);
}

void configure_node_props(impl *self)
{
	const auto &identity = self->camera->identity();
	uint32_t n_items = 0;
	const std::string stable = !identity.serial.empty() ? identity.serial :
			std::to_string(self->options.interface_index) + "." +
			std::to_string(self->options.device_index) + "." +
			std::to_string(self->options.stream_index);
	self->node_name = "egrabber_source." + stable;
	self->description = identity.vendor;
	if (!identity.model.empty()) {
		if (!self->description.empty())
			self->description += " ";
		self->description += identity.model;
	}
	if (self->description.empty())
		self->description = "eGrabber camera source";
	self->interface_index = std::to_string(self->options.interface_index);
	self->device_index = std::to_string(self->options.device_index);
	self->stream_index = std::to_string(self->options.stream_index);
	if (self->options.acquisition_domain)
		self->acquisition_domain = egrabber_pipewire::format_acquisition_domain(
				*self->options.acquisition_domain);
	self->acquisition_generation = std::to_string(
			self->options.acquisition_generation);
	self->acquisition_sequence_context = std::to_string(
			self->options.acquisition_sequence_context);

#define ADD_ITEM(key, value) \
	self->node_items[n_items++] = SPA_DICT_ITEM_INIT(key, value)
	ADD_ITEM(SPA_KEY_DEVICE_API, "egrabber");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS, "Video/Source");
	ADD_ITEM(SPA_KEY_MEDIA_ROLE, "Camera");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name.c_str());
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_PRODUCER, self->options.producer.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_INTERFACE_INDEX, self->interface_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_DEVICE_INDEX, self->device_index.c_str());
	ADD_ITEM(SPA_KEY_API_EGRABBER_STREAM_INDEX, self->stream_index.c_str());
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
	self->node_props = SPA_DICT_INIT(self->node_items, n_items);
}

int init(const struct spa_handle_factory *, struct spa_handle *handle,
		const struct spa_dict *info, const struct spa_support *support,
		uint32_t n_support)
{
	impl *self;
	struct spa_image_source_config config = {
		.version = SPA_VERSION_IMAGE_SOURCE_CONFIG,
		.min_buffers = 2,
		.max_buffers = max_buffers,
		.flags = SPA_IMAGE_SOURCE_FLAG_REQUIRE_HEADER |
			SPA_IMAGE_SOURCE_FLAG_REQUIRE_ACQUISITION,
	};

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	self = new (handle) impl{};
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	self->log = static_cast<spa_log *>(spa_support_find(support, n_support,
			SPA_TYPE_INTERFACE_Log));
	spa_hook_list_init(&self->hooks);
	try {
		read_options(self->options, info);
		self->acquisition_keys = AcquisitionKeySequence(
				self->options.acquisition_generation);
		self->camera = std::make_unique<Camera>(self->options, self->log);
#ifdef HAVE_EGRABBER_DRM
		if (self->camera->dma_buf_supported()) {
			self->dma_sync = std::make_unique<
					egrabber_pipewire::DmaBufSyncContext>();
			self->dma_buf_offered = self->dma_sync->available();
		}
#endif
		if (self->options.progressive ==
				egrabber_pipewire::ProgressivePolicy::require &&
				!self->camera->progressive_supported())
			throw std::invalid_argument(
					"required progressive acquisition is unavailable");
		self->progressive_offered = self->options.progressive !=
				egrabber_pipewire::ProgressivePolicy::disabled &&
				self->camera->progressive_supported();
		if (self->progressive_offered)
			config.flags |= SPA_IMAGE_SOURCE_FLAG_ALLOW_PROGRESSIVE;
		if (self->options.acquisition_domain &&
				!self->camera->progressive_supported())
			throw std::invalid_argument(
					"acquisition identity requires Grablink or Coaxlink StartOfCameraReadout events");
		if (self->camera->width() > std::numeric_limits<uint32_t>::max() ||
				self->camera->height() > std::numeric_limits<uint32_t>::max() ||
				self->camera->payload_size() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->natural_line_pitch() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->buffer_alignment() >
						static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				self->camera->announce_minimum() > max_buffers ||
				self->camera->buffer_count() > max_buffers)
			throw std::runtime_error("eGrabber layout exceeds the SPA image limits");
		self->video_format = video_format(*self->camera);
		if (const auto rate = self->camera->frame_rate(); rate && *rate > 0.0)
			self->frame_rate = frame_rate(*rate);
	} catch (const std::invalid_argument &error) {
		spa_log_error(self->log, "could not initialize eGrabber source: %s",
				error.what());
		self->~impl();
		return -EINVAL;
	} catch (const std::exception &error) {
		spa_log_error(self->log, "could not initialize eGrabber source: %s",
				error.what());
		self->~impl();
		return -EIO;
	} catch (...) {
		spa_log_error(self->log,
				"could not initialize eGrabber source: unknown exception");
		self->~impl();
		return -EIO;
	}
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_output_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_RTC_PROCESS;
	self->params[0] = SPA_PARAM_INFO(SPA_PARAM_PropInfo,
			SPA_PARAM_INFO_READ);
	self->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props,
			SPA_PARAM_INFO_READWRITE);
	self->info.params = self->params;
	self->info.n_params = SPA_N_ELEMENTS(self->params);
	configure_node_props(self);
	self->info.props = &self->node_props;
	self->output.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->output.info = SPA_PORT_INFO_INIT();
	self->output.info.flags = SPA_PORT_FLAG_LIVE;
	self->output.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	self->output.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	self->output.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	self->output.params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta,
			SPA_PARAM_INFO_READ);
	self->output.params[4] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	self->output.info.params = self->output.params;
	self->output.info.n_params = SPA_N_ELEMENTS(self->output.params);
	config.min_buffers = self->camera->announce_minimum();
	self->latest = spa_buffer_latest_new(SPA_DIRECTION_OUTPUT, self, self->log);
	if (self->latest == nullptr) {
		self->~impl();
		return -errno;
	}
	const int res = spa_image_source_latest_init(&self->transport, &self->source,
			self->latest, &config);
	if (res < 0) {
		spa_buffer_latest_destroy(self->latest);
		self->latest = nullptr;
		self->~impl();
	}
	return res;
}

const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

int enum_interface_info(const struct spa_handle_factory *,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

} // namespace

extern "C" const struct spa_handle_factory spa_egrabber_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_EGRABBER_SOURCE,
	nullptr,
	get_size,
	init,
	enum_interface_info,
};

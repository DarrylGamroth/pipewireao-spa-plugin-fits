/* SPDX-License-Identifier: MIT */

#include "camera.hpp"
#include "control_backend.hpp"
#include "egrabber.hpp"
#include "egrabber_control.hpp"

#include <spa/pod/parser.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace egrabber_pipewire {
namespace gc = Euresys::gc;
using Euresys::Buffer;
using Euresys::BufferIndexRange;
using Euresys::DmaBufMemory;
using Euresys::EGenTL;
using Euresys::EGrabber;
using Euresys::NewBufferData;
using Euresys::RemoteModule;
using Euresys::StreamModule;
using Euresys::UserMemory;

template<typename Function>
void collect_cleanup_error(std::exception_ptr &first_error,
                           Function &&function) noexcept {
    try {
        function();
    } catch (...) {
        if (!first_error) first_error = std::current_exception();
    }
}

struct CameraSelection {
    Euresys::EGrabberCameraInfo camera;
    CameraIdentity identity;
};

CameraIdentity camera_identity(const Euresys::EGrabberInfo &info) {
    return {
        info.deviceVendorName, info.deviceModelName, info.deviceSerialNumber,
        info.deviceUserID, info.tlType, info.interfaceID, info.deviceID, info.streamID,
    };
}

bool matches_selector(const Euresys::EGrabberCameraInfo &camera, const Options &options) {
    return std::any_of(camera.grabbers.begin(), camera.grabbers.end(), [&](const auto &grabber) {
        if (options.serial) return grabber.deviceSerialNumber == *options.serial;
        if (options.user_id) return grabber.deviceUserID == *options.user_id;
        return grabber.interfaceIndex == options.interface_index &&
               grabber.deviceIndex == options.device_index;
    });
}

CameraSelection select_camera(Euresys::EGrabberDiscovery &discovery, const Options &options) {
    discovery.discover();
    std::vector<CameraSelection> matches;
    for (int index = 0; index < discovery.cameraCount(); ++index) {
        auto camera = discovery.cameras(index, options.stream_index);
        if (!camera.grabbers.empty() && matches_selector(camera, options))
            matches.push_back({camera, camera_identity(camera.grabbers.front())});
    }
    if (matches.empty()) {
        const auto selector = options.serial ? "serial " + *options.serial
            : options.user_id ? "user ID " + *options.user_id
            : "interface/device indexes " + std::to_string(options.interface_index) + "/" +
                  std::to_string(options.device_index);
        throw std::runtime_error("No discovered camera matches " + selector +
                                 "; use --list-cameras to inspect available devices");
    }
    if (matches.size() != 1)
        throw std::runtime_error("Camera selector is ambiguous; use --serial with a unique value");
    return std::move(matches.front());
}

std::string producer_path(const std::string &producer) {
    if (producer == "gigelink") return Euresys::Gigelink();
    if (producer == "grablink") return Euresys::Grablink();
    if (producer == "coaxlink") return Euresys::Coaxlink();
    return producer;
}

bool supports_progressive_dma(const Options &options, const CameraIdentity &identity) {
    const auto named_frame_grabber = options.producer == "grablink" ||
        options.producer == "coaxlink" ||
        options.producer.find("grablink.cti") != std::string::npos ||
        options.producer.find("coaxlink.cti") != std::string::npos;
    return named_frame_grabber || identity.transport == "CL" ||
        identity.transport == "CameraLink" || identity.transport == "CXP" ||
        identity.transport == "CoaXPress";
}

class CallbackGrabber final : public EGrabber<Euresys::CallbackOnDemand> {
public:
    using FrameCallback = std::function<void(const NewBufferData &)>;
    using EventCallback = std::function<void(const TransportEvent &)>;

    explicit CallbackGrabber(const CameraSelection &selection)
        : EGrabber<Euresys::CallbackOnDemand>(selection.camera, gc::DEVICE_ACCESS_CONTROL) {}

    void set_frame_callback(FrameCallback callback) {
        frame_callback_ = std::move(callback);
    }

    void clear_frame_callback() {
        frame_callback_ = {};
    }

    void set_event_callback(EventCallback callback) {
        event_callback_ = std::move(callback);
    }

    bool process_event() {
        event_processed_ = false;
        callback_error_ = {};
        processEventFilter(event_filter());
        if (callback_error_)
            std::rethrow_exception(callback_error_);
        return event_processed_;
    }

protected:
    /* CallbackOnDemand invokes these callbacks synchronously on the caller. */
    void onNewBufferEvent(const NewBufferData &data) override {
        event_processed_ = true;
        try {
            if (frame_callback_) frame_callback_(data);
            else Buffer(data).push(*this);
        } catch (...) {
            remember_callback_error();
        }
    }

    void onDataStreamEvent(const Euresys::DataStreamData &data) override {
        dispatch_transport_event({TransportEventKind::data_stream, data.timestamp,
            data.numid, data.context1, data.context2, data.context3});
    }

    void onDeviceErrorEvent(const Euresys::DeviceErrorData &data) override {
        dispatch_transport_event({TransportEventKind::device_error, data.timestamp,
            data.numid, data.context1, data.context2, data.context3});
    }

    void onRemoteDeviceEvent(const Euresys::RemoteDeviceData &data) override {
        dispatch_transport_event({TransportEventKind::remote_device, data.timestamp,
            data.eventId, data.eventNs, 0, 0});
    }

private:
    static std::size_t event_filter() {
        return Euresys::Internal::getEventFilter<NewBufferData>() |
            Euresys::Internal::getEventFilter<Euresys::DataStreamData>() |
            Euresys::Internal::getEventFilter<Euresys::DeviceErrorData>() |
            Euresys::Internal::getEventFilter<Euresys::RemoteDeviceData>();
    }

    void remember_callback_error() noexcept {
        if (!callback_error_)
            callback_error_ = std::current_exception();
    }

    void dispatch_transport_event(const TransportEvent &event) noexcept {
        event_processed_ = true;
        try {
            if (event_callback_)
                event_callback_(event);
        } catch (...) {
            remember_callback_error();
        }
    }

    FrameCallback frame_callback_;
    EventCallback event_callback_;
    std::exception_ptr callback_error_;
    bool event_processed_ = false;
};

struct Camera::Impl {
public:
    Impl(const Options &options, struct spa_log *log)
        : log_(log), gentl_(producer_path(options.producer)),
          discovery_(std::make_unique<Euresys::EGrabberDiscovery>(gentl_)),
          selection_(select_camera(*discovery_, options)), grabber_(selection_),
          buffer_count_(options.buffer_count),
          progressive_supported_(supports_progressive_dma(options, selection_.identity)) {
        discovery_.reset();
        configure_control(options);
        update_identity_from_control();
        synchronize_transport_layout();
        width_ = read_dimension("Width", grabber_.getWidth());
        height_ = read_dimension("Height", grabber_.getHeight());
        offset_x_ = read_offset("OffsetX");
        offset_y_ = read_offset("OffsetY");
        payload_size_ = grabber_.getPayloadSize();
        pixel_format_ = grabber_.getPixelFormat();
        natural_line_pitch_ = width_ * gentl_.imageGetBytesPerPixel(pixel_format_);
        refresh_buffer_requirements();
        try { host_memory_type_ = grabber_.getString<StreamModule>("MemoryType"); }
        catch (...) {}
        features_ = control_->features();
    }

    std::size_t width() const { return width_; }
    std::size_t height() const { return height_; }
    std::size_t offset_x() const { return offset_x_; }
    std::size_t offset_y() const { return offset_y_; }
    std::size_t payload_size() const { return payload_size_; }
    std::size_t natural_line_pitch() const { return natural_line_pitch_; }
    const std::string &pixel_format() const { return pixel_format_; }
    const std::vector<Feature> &features() const { return features_; }
    const CameraIdentity &identity() const { return selection_.identity; }
    bool progressive_supported() const { return progressive_supported_; }
    std::size_t buffer_count() const { return buffer_count_; }
    std::size_t announce_minimum() const { return announce_minimum_; }
    std::size_t buffer_alignment() const { return buffer_alignment_; }

    bool dma_buf_supported() {
        std::lock_guard lock(mutex_);
        try {
            const auto original = grabber_.getString<StreamModule>("MemoryType");
            struct Restore {
                CallbackGrabber &grabber;
                std::string value;
                ~Restore() { try { grabber.setString<StreamModule>("MemoryType", value); } catch (...) {} }
            } restore{grabber_, original};
            grabber_.setString<StreamModule>("MemoryType", "DmaBuf");
            return grabber_.getInteger<StreamModule>("MemoryTypeSupported") != 0;
        } catch (...) {
            return false;
        }
    }

    void set_frame_callback(CallbackGrabber::FrameCallback callback) {
        grabber_.set_frame_callback(std::move(callback));
    }

    void clear_frame_callback() { grabber_.clear_frame_callback(); }

    void set_transport_event_callback(CallbackGrabber::EventCallback callback) {
        grabber_.set_event_callback(std::move(callback));
    }

    bool process_event() {
        return grabber_.process_event();
    }

    void select_memory_type(bool direct_dma_buf) {
        std::lock_guard lock(mutex_);
        if (direct_dma_buf) grabber_.setString<StreamModule>("MemoryType", "DmaBuf");
        else if (!host_memory_type_.empty())
            grabber_.setString<StreamModule>("MemoryType", host_memory_type_);
    }

    BufferIndexRange announce(void *base, int fd, std::size_t size, std::uint32_t offset,
                              bool direct_dma_buf, void *user_pointer) {
        std::lock_guard lock(mutex_);
        if (direct_dma_buf)
            return grabber_.announceAndQueue(DmaBufMemory(fd, size, offset, user_pointer));
        return grabber_.announceAndQueue(UserMemory(base, size, user_pointer));
    }

    enum class QuerySupport : std::uint8_t { unknown, supported, unavailable };

    static bool unavailable_query_error(gc::GC_ERROR error) noexcept {
        return error == gc::GC_ERR_NOT_IMPLEMENTED ||
            error == gc::GC_ERR_INVALID_ID ||
            error == gc::GC_ERR_INVALID_PARAMETER ||
            error == gc::GC_ERR_NOT_AVAILABLE;
    }

    template<typename T>
    std::optional<T> optional_buffer_info(Buffer &buffer,
                                         gc::BUFFER_INFO_CMD command,
                                         QuerySupport &support) {
        if (support == QuerySupport::unavailable) return std::nullopt;
        try {
            auto value = buffer.getInfo<T>(grabber_, command);
            support = QuerySupport::supported;
            return value;
        } catch (const Euresys::gentl_error &error) {
            if (unavailable_query_error(error.gc_err))
                support = QuerySupport::unavailable;
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<bool> optional_buffer_flag(Buffer &buffer,
                                             gc::BUFFER_INFO_CMD command,
                                             QuerySupport &support) {
        const auto value = optional_buffer_info<bool8_t>(buffer, command, support);
        if (!value) return std::nullopt;
        return *value != 0;
    }

    BufferMetadata buffer_metadata(Buffer &buffer) {
        BufferMetadata metadata;
        metadata.frame_id = optional_buffer_info<std::uint64_t>(
            buffer, gc::BUFFER_INFO_FRAMEID, frame_id_support_);
        metadata.timestamp_ns = optional_buffer_info<std::uint64_t>(
            buffer, gc::BUFFER_INFO_TIMESTAMP_NS, timestamp_support_);
        metadata.image_offset = optional_buffer_info<std::size_t>(
            buffer, gc::BUFFER_INFO_IMAGEOFFSET, image_offset_support_);
        metadata.size_filled = optional_buffer_info<std::size_t>(
            buffer, gc::BUFFER_INFO_SIZE_FILLED, size_filled_support_);
        metadata.data_size = optional_buffer_info<std::size_t>(
            buffer, gc::BUFFER_INFO_DATA_SIZE, data_size_support_);
        metadata.x_padding = optional_buffer_info<std::size_t>(
            buffer, gc::BUFFER_INFO_XPADDING, x_padding_support_);
        metadata.payload_type = optional_buffer_info<std::size_t>(
            buffer, gc::BUFFER_INFO_PAYLOADTYPE, payload_type_support_);
        metadata.image_present = optional_buffer_flag(
            buffer, gc::BUFFER_INFO_IMAGEPRESENT, image_present_support_);
        metadata.data_larger_than_buffer =
            optional_buffer_flag(buffer, gc::BUFFER_INFO_DATA_LARGER_THAN_BUFFER,
                                 data_larger_support_);
        metadata.incomplete = optional_buffer_flag(
            buffer, gc::BUFFER_INFO_IS_INCOMPLETE, incomplete_support_);
        return metadata;
    }

    std::optional<BufferProgress> buffer_progress(const BufferIndexRange &range) {
        const auto index = range.indexAt(0);
        try {
            if (grabber_.getBufferInfo<bool8_t>(
                    index, gc::BUFFER_INFO_IS_ACQUIRING) == 0)
                return std::nullopt;
            BufferProgress progress;
            progress.size_filled = grabber_.getBufferInfo<std::size_t>(
                index, gc::BUFFER_INFO_SIZE_FILLED);
            try {
                progress.frame_id = grabber_.getBufferInfo<std::uint64_t>(
                    index, gc::BUFFER_INFO_FRAMEID);
            } catch (...) {}
            return progress;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<double> frame_rate() {
        const auto *feature = frame_rate_feature();
        if (!feature) return std::nullopt;
        std::lock_guard lock(mutex_);
        try {
            if (!control_->readable(*feature)) return std::nullopt;
            const double value = feature->kind == FeatureKind::floating
                ? control_->get_float(*feature)
                : static_cast<double>(control_->get_integer(*feature));
            return value > 0.0 ? std::optional<double>(value) : std::nullopt;
        } catch (...) { return std::nullopt; }
    }

    std::optional<std::pair<double, double>> frame_rate_range() {
        const auto *feature = frame_rate_feature();
        if (!feature) return std::nullopt;
        std::lock_guard lock(mutex_);
        try {
            if (!control_->writeable(*feature)) return std::nullopt;
            double minimum = 0.0;
            double maximum = 0.0;
            if (feature->kind == FeatureKind::floating) {
                const auto range = control_->float_range(*feature);
                if (!range) return std::nullopt;
                minimum = range->first;
                maximum = range->second;
            } else {
                const auto range = control_->integer_range(*feature);
                if (!range) return std::nullopt;
                minimum = static_cast<double>(range->first);
                maximum = static_cast<double>(range->second);
            }
            if (maximum <= 0.0) return std::nullopt;
            return std::pair{std::max(0.001, minimum), maximum};
        } catch (...) { return std::nullopt; }
    }

    bool negotiate_frame_rate(double frames_per_second) {
        const auto *feature = frame_rate_feature();
        if (!feature || frames_per_second <= 0.0) return false;
        const auto integer_rate = std::llround(frames_per_second);
        if (feature->kind == FeatureKind::integer &&
            std::abs(frames_per_second - static_cast<double>(integer_rate)) > 0.001)
            return false;
        std::lock_guard lock(mutex_);
        if (!control_->writeable(*feature)) {
            const double current = feature->kind == FeatureKind::floating
                ? control_->get_float(*feature)
                : static_cast<double>(control_->get_integer(*feature));
            return std::abs(current - frames_per_second) <= std::max(0.001, current * 0.001);
        }
        const auto enable = std::find_if(features_.begin(), features_.end(), [](const Feature &candidate) {
            return candidate.name == "AcquisitionFrameRateEnable" &&
                   candidate.kind == FeatureKind::boolean;
        });
        if (enable != features_.end() && control_->writeable(*enable))
            control_->set_integer(*enable, 1);
        if (feature->kind == FeatureKind::floating)
            control_->set_float(*feature, frames_per_second);
        else
            control_->set_integer(*feature, integer_rate);
        return true;
    }

    void recycle(Buffer &buffer) {
        buffer.push(grabber_);
    }

    void release(const std::vector<BufferIndexRange> &ranges) {
        std::lock_guard lock(mutex_);
        std::exception_ptr first_error;
        collect_cleanup_error(first_error, [&] {
            grabber_.flushBuffers(gc::ACQ_QUEUE_UNQUEUED_TO_INPUT);
        });
        collect_cleanup_error(first_error, [&] {
            grabber_.flushBuffers(gc::ACQ_QUEUE_ALL_DISCARD);
        });
        for (const auto &range : ranges)
            collect_cleanup_error(first_error, [&] { grabber_.revoke(range); });
        if (first_error) std::rethrow_exception(first_error);
    }

    void start() {
        std::lock_guard lock(mutex_);
        if (started_) return;
        if (!buffer_event_enabled_) {
            grabber_.enableEvent<NewBufferData>();
            buffer_event_enabled_ = true;
            try {
                grabber_.enableEvent<Euresys::DataStreamData>();
                data_stream_event_enabled_ = true;
            } catch (...) {}
            try {
                grabber_.enableEvent<Euresys::DeviceErrorData>();
                device_error_event_enabled_ = true;
            } catch (...) {}
            try {
                grabber_.enableEvent<Euresys::RemoteDeviceData>();
                remote_device_event_enabled_ = true;
            } catch (...) {}
        }
        grabber_.start();
        started_ = true;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        if (!started_) return;
        grabber_.stop();
        started_ = false;
    }

    void disable_events() {
        std::lock_guard lock(mutex_);
        std::exception_ptr first_error;
        if (remote_device_event_enabled_) {
            collect_cleanup_error(first_error, [&] {
                grabber_.disableEvent<Euresys::RemoteDeviceData>();
                remote_device_event_enabled_ = false;
            });
        }
        if (device_error_event_enabled_) {
            collect_cleanup_error(first_error, [&] {
                grabber_.disableEvent<Euresys::DeviceErrorData>();
                device_error_event_enabled_ = false;
            });
        }
        if (data_stream_event_enabled_) {
            collect_cleanup_error(first_error, [&] {
                grabber_.disableEvent<Euresys::DataStreamData>();
                data_stream_event_enabled_ = false;
            });
        }
        if (buffer_event_enabled_) {
            collect_cleanup_error(first_error, [&] {
                grabber_.disableEvent<NewBufferData>();
                buffer_event_enabled_ = false;
            });
        }
        if (first_error) std::rethrow_exception(first_error);
    }

    void set_feature(const Feature &feature, const spa_pod *value,
                     bool allow_buffer_change) {
        std::lock_guard lock(mutex_);
        if (!control_->writeable(feature))
            throw std::runtime_error(feature.name + " is not currently writeable");
        if (started_)
            throw std::runtime_error("GenICam properties can only change while acquisition is stopped");
        std::function<void()> restore;
        bool value_applied = false;
        try {
            switch (feature.kind) {
            case FeatureKind::boolean: {
                const auto old = control_->get_integer(feature);
                bool v;
                if (spa_pod_get_bool(value, &v) < 0) throw std::runtime_error("expected a boolean");
                restore = [&, old] { control_->set_integer(feature, old); };
                control_->set_integer(feature, v ? 1 : 0);
                value_applied = true;
                break;
            }
            case FeatureKind::integer: {
                const auto old = control_->get_integer(feature);
                std::int64_t v;
                std::int32_t small;
                if (spa_pod_get_long(value, &v) < 0) {
                    if (spa_pod_get_int(value, &small) < 0) throw std::runtime_error("expected an integer");
                    v = small;
                }
                restore = [&, old] { control_->set_integer(feature, old); };
                control_->set_integer(feature, v);
                value_applied = true;
                break;
            }
            case FeatureKind::floating: {
                const auto old = control_->get_float(feature);
                double v;
                float small;
                if (spa_pod_get_double(value, &v) < 0) {
                    if (spa_pod_get_float(value, &small) < 0) throw std::runtime_error("expected a number");
                    v = small;
                }
                restore = [&, old] { control_->set_float(feature, old); };
                control_->set_float(feature, v);
                value_applied = true;
                break;
            }
            case FeatureKind::enumeration: {
                const auto old = control_->get_string(feature);
                std::int32_t index;
                if (spa_pod_get_int(value, &index) < 0) {
                    std::uint32_t id;
                    if (spa_pod_get_id(value, &id) < 0) throw std::runtime_error("expected an enum index");
                    index = static_cast<std::int32_t>(id);
                }
                if (index < 0 || static_cast<std::size_t>(index) >= feature.enum_entries.size())
                    throw std::runtime_error("enum index is out of range");
                restore = [&, old] { control_->set_string(feature, old); };
                control_->set_string(feature, feature.enum_entries[index]);
                value_applied = true;
                break;
            }
            case FeatureKind::string: {
                const auto old = control_->get_string(feature);
                const char *v;
                if (spa_pod_get_string(value, &v) < 0) throw std::runtime_error("expected a string");
                restore = [&, old] { control_->set_string(feature, old); };
                control_->set_string(feature, v);
                value_applied = true;
                break;
            }
            default:
                throw std::runtime_error("feature type cannot be assigned");
            }
            synchronize_transport_layout();
            const bool buffers_changed =
                grabber_.shouldReallocBuffers() || grabber_.shouldReannounceBuffers() ||
                (control_->requires_transport_layout_sync() && changes_payload_layout(feature));
            if (buffers_changed && !allow_buffer_change) {
                restore();
                synchronize_transport_layout();
                // Re-query after rollback so producers with edge-triggered
                // invalidation do not leak the tentative change into the next write.
                (void)grabber_.shouldReallocBuffers();
                (void)grabber_.shouldReannounceBuffers();
                throw BufferChangeRequired(
                    "change was rolled back because it requires PipeWire buffer renegotiation");
            }
            if (allow_buffer_change) {
                refresh_layout();
                if (width_ > std::numeric_limits<std::uint32_t>::max() ||
                    height_ > std::numeric_limits<std::uint32_t>::max() ||
                    payload_size_ > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()) ||
                    natural_line_pitch_ > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()) ||
                    buffer_alignment_ > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max()))
                    throw std::runtime_error(
                        "updated camera layout exceeds SPA integer limits");
            }
        } catch (...) {
            if (allow_buffer_change && value_applied && restore) {
                try {
                    restore();
                    synchronize_transport_layout();
                    refresh_layout();
                } catch (const std::exception &error) {
                    spa_log_warn(log_, "could not roll back %s: %s",
                                 feature.name.c_str(), error.what());
                }
            }
            throw;
        }
    }

    void execute_command(const Feature &feature) {
        if (feature.kind != FeatureKind::command)
            throw std::runtime_error(feature.name + " is not a command");
        std::lock_guard lock(mutex_);
        if (started_)
            throw std::runtime_error("GenICam commands can only execute while acquisition is stopped");
        if (!control_->writeable(feature))
            throw std::runtime_error(feature.name + " is not currently executable");
        control_->execute(feature);
    }

    std::string feature_text(const Feature &feature) {
        std::lock_guard lock(mutex_);
        if (!control_->readable(feature))
            throw std::runtime_error(feature.name + " is not currently readable");
        switch (feature.kind) {
        case FeatureKind::boolean:
            return control_->get_integer(feature) != 0 ? "true" : "false";
        case FeatureKind::integer:
            return std::to_string(control_->get_integer(feature));
        case FeatureKind::floating:
            return std::to_string(control_->get_float(feature));
        case FeatureKind::enumeration:
        case FeatureKind::string:
            return control_->get_string(feature);
        default:
            return {};
        }
    }

    FeatureValue feature_value(const Feature &feature) {
        std::lock_guard lock(mutex_);
        if (!control_->readable(feature))
            throw std::runtime_error(feature.name + " is not currently readable");
        switch (feature.kind) {
        case FeatureKind::boolean:
            return control_->get_integer(feature) != 0;
        case FeatureKind::integer:
            return control_->get_integer(feature);
        case FeatureKind::floating:
            return control_->get_float(feature);
        case FeatureKind::enumeration: {
            const auto value = control_->get_string(feature);
            const auto found = std::find(feature.enum_entries.begin(),
                                         feature.enum_entries.end(), value);
            if (found == feature.enum_entries.end())
                throw std::runtime_error(feature.name +
                                         " returned an unknown enumeration value");
            return static_cast<std::int32_t>(
                std::distance(feature.enum_entries.begin(), found));
        }
        case FeatureKind::string:
            return control_->get_string(feature);
        default:
            throw std::runtime_error(feature.name + " is not a scalar feature");
        }
    }

    std::optional<std::pair<std::int64_t, std::int64_t>>
    feature_integer_range(const Feature &feature) {
        std::lock_guard lock(mutex_);
        return control_->integer_range(feature);
    }

    std::optional<std::pair<double, double>>
    feature_float_range(const Feature &feature) {
        std::lock_guard lock(mutex_);
        return control_->float_range(feature);
    }

private:
    void configure_control(const Options &options) {
        if (options.control == "none") {
            control_ = make_null_control_backend();
            return;
        }
        if (options.control == "remote") {
            control_ = make_remote_control_backend(grabber_, log_);
            return;
        }
#ifdef HAVE_CLPROTOCOL_CONTROL
        CLProtocolControlOptions cl_options;
        cl_options.libraries = options.clprotocol_libraries;
        cl_options.device_template = options.clprotocol_device_template;
        cl_options.camera_serial = options.camera_serial;
        cl_options.genapi_runtime = options.genapi_runtime;
        cl_options.timeout_ms = options.control_timeout_ms;
        const bool grablink = options.producer == "grablink" ||
            options.producer.find("grablink.cti") != std::string::npos ||
            selection_.identity.transport == "CL" || selection_.identity.transport == "CameraLink";
        const bool installed = !discover_clprotocol_libraries(cl_options).empty();
        if (options.control == "clprotocol" || (options.control == "auto" && grablink && installed)) {
            serial_transport_ = make_grablink_serial_transport(grabber_);
            try {
                control_ = make_clprotocol_control_backend(*serial_transport_, cl_options);
                spa_log_info(log_, "camera control backend: CLProtocol");
                return;
            } catch (const std::exception &error) {
                serial_transport_.reset();
                if (options.control == "clprotocol" || options.camera_serial)
                    throw;
                spa_log_warn(log_, "CLProtocol auto-detection failed; using the "
                             "eGrabber RemoteModule: %s", error.what());
            }
        } else if (options.camera_serial) {
            throw std::runtime_error("--camera-serial requires an installed CLProtocol backend");
        }
#else
        if (options.control == "clprotocol" || options.camera_serial ||
            !options.clprotocol_libraries.empty() || options.clprotocol_device_template ||
            options.genapi_runtime)
            throw std::runtime_error(
                "this build has no CLProtocol support; configure Meson with -Dgenicam_root=PATH");
#endif
        control_ = make_remote_control_backend(grabber_, log_);
    }

    const Feature *control_feature(std::string_view name) const {
        const auto &available = control_->features();
        const auto found = std::find_if(available.begin(), available.end(),
            [&](const Feature &feature) { return feature.name == name; });
        return found == available.end() ? nullptr : &*found;
    }

    void update_identity_from_control() {
        const auto update = [&](std::string_view name, std::string &target) {
            const auto *feature = control_feature(name);
            if (!feature || feature->kind != FeatureKind::string) return;
            try {
                if (control_->readable(*feature)) {
                    const auto value = control_->get_string(*feature);
                    if (!value.empty()) target = value;
                }
            } catch (...) {}
        };
        update("DeviceVendorName", selection_.identity.vendor);
        update("DeviceModelName", selection_.identity.model);
        update("DeviceSerialNumber", selection_.identity.serial);
        update("DeviceUserID", selection_.identity.user_id);
    }

    std::size_t read_dimension(std::string_view name, std::size_t fallback) {
        const auto *feature = control_feature(name);
        if (!feature || feature->kind != FeatureKind::integer) return fallback;
        try {
            if (!control_->readable(*feature)) return fallback;
            return static_cast<std::size_t>(
                std::max<std::int64_t>(1, control_->get_integer(*feature)));
        } catch (...) { return fallback; }
    }

    void synchronize_transport_layout() {
        if (!control_->requires_transport_layout_sync()) return;
        const auto *pixel_format = control_feature("PixelFormat");
        if (!pixel_format || pixel_format->kind != FeatureKind::enumeration ||
            !control_->readable(*pixel_format))
            throw std::runtime_error(
                "CLProtocol node map must provide readable PixelFormat");
        const auto format = control_->get_string(*pixel_format);
        try {
            const auto current = grabber_.getString<RemoteModule>("PixelFormat");
            if (current != format) {
                if (grabber_.getInteger<RemoteModule>(
                        Euresys::query::writeable("PixelFormat")) == 0)
                    throw std::runtime_error("virtual RemoteModule feature is not writeable");
                grabber_.setString<RemoteModule>("PixelFormat", format);
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("cannot synchronize CLProtocol PixelFormat with the "
                                     "Grablink receiver: " + std::string(error.what()));
        }
        for (const std::string_view name : {
                 std::string_view("Width"), std::string_view("Height"),
                 std::string_view("OffsetX"), std::string_view("OffsetY")}) {
            const auto *feature = control_feature(name);
            const bool required = name == "Width" || name == "Height";
            if (!feature && !required) continue;
            if (!feature || feature->kind != FeatureKind::integer || !control_->readable(*feature))
                throw std::runtime_error("CLProtocol node map must provide readable " +
                                         std::string(name));
            const auto value = control_->get_integer(*feature);
            try {
                const auto current = grabber_.getInteger<RemoteModule>(std::string(name));
                if (current == value) continue;
                if (grabber_.getInteger<RemoteModule>(
                        Euresys::query::writeable(std::string(name))) == 0)
                    throw std::runtime_error("virtual RemoteModule feature is not writeable");
                grabber_.setInteger<RemoteModule>(std::string(name), value);
            } catch (const std::exception &error) {
                throw std::runtime_error("cannot synchronize CLProtocol " + std::string(name) +
                    " with the Grablink receiver: " + error.what());
            }
        }
    }

    std::size_t read_offset(const char *name) {
        const auto *feature = control_feature(name);
        if (!feature || feature->kind != FeatureKind::integer) return 0;
        try {
            return static_cast<std::size_t>(
                std::max<std::int64_t>(0, control_->get_integer(*feature)));
        } catch (...) { return 0; }
    }

    void refresh_buffer_requirements() {
        try {
            announce_minimum_ = std::max<std::size_t>(2,
                grabber_.getInfo<StreamModule, std::size_t>(gc::STREAM_INFO_BUF_ANNOUNCE_MIN));
        } catch (...) { announce_minimum_ = 2; }
        try {
            buffer_alignment_ = std::max<std::size_t>(1,
                grabber_.getInfo<StreamModule, std::size_t>(gc::STREAM_INFO_BUF_ALIGNMENT));
        } catch (...) { buffer_alignment_ = 1; }
        buffer_count_ = std::max(buffer_count_, announce_minimum_);
    }

    void refresh_layout() {
        width_ = read_dimension("Width", grabber_.getWidth());
        height_ = read_dimension("Height", grabber_.getHeight());
        offset_x_ = read_offset("OffsetX");
        offset_y_ = read_offset("OffsetY");
        payload_size_ = grabber_.getPayloadSize();
        pixel_format_ = grabber_.getPixelFormat();
        natural_line_pitch_ = width_ * gentl_.imageGetBytesPerPixel(pixel_format_);
        refresh_buffer_requirements();
    }

    const Feature *frame_rate_feature() const {
        static constexpr std::string_view names[] = {
            "AcquisitionFrameRate", "AcquisitionFrameRateAbs",
        };
        for (const auto name : names) {
            const auto feature = std::find_if(features_.begin(), features_.end(), [&](const Feature &candidate) {
                return candidate.name == name &&
                    (candidate.kind == FeatureKind::floating ||
                     candidate.kind == FeatureKind::integer);
            });
            if (feature != features_.end()) return &*feature;
        }
        return nullptr;
    }

    struct spa_log *log_;
    EGenTL gentl_;
    std::unique_ptr<Euresys::EGrabberDiscovery> discovery_;
    CameraSelection selection_;
    CallbackGrabber grabber_;
    std::unique_ptr<SerialTransport> serial_transport_;
    std::unique_ptr<ControlBackend> control_;
    std::size_t buffer_count_;
    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::size_t offset_x_ = 0;
    std::size_t offset_y_ = 0;
    std::size_t payload_size_ = 0;
    std::size_t natural_line_pitch_ = 0;
    std::size_t announce_minimum_ = 2;
    std::size_t buffer_alignment_ = 1;
    std::string pixel_format_;
    std::string host_memory_type_;
    bool progressive_supported_ = false;
    std::vector<Feature> features_;
    QuerySupport frame_id_support_ = QuerySupport::unknown;
    QuerySupport timestamp_support_ = QuerySupport::unknown;
    QuerySupport image_offset_support_ = QuerySupport::unknown;
    QuerySupport size_filled_support_ = QuerySupport::unknown;
    QuerySupport data_size_support_ = QuerySupport::unknown;
    QuerySupport x_padding_support_ = QuerySupport::unknown;
    QuerySupport payload_type_support_ = QuerySupport::unknown;
    QuerySupport image_present_support_ = QuerySupport::unknown;
    QuerySupport data_larger_support_ = QuerySupport::unknown;
    QuerySupport incomplete_support_ = QuerySupport::unknown;
    std::mutex mutex_;
    bool started_ = false;
    bool buffer_event_enabled_ = false;
    bool data_stream_event_enabled_ = false;
    bool device_error_event_enabled_ = false;
    bool remote_device_event_enabled_ = false;
};

Camera::Camera(const Options &options, struct spa_log *log)
    : impl_(std::make_unique<Impl>(options, log)) {}
Camera::~Camera() = default;
std::size_t Camera::width() const { return impl_->width(); }
std::size_t Camera::height() const { return impl_->height(); }
std::size_t Camera::offset_x() const { return impl_->offset_x(); }
std::size_t Camera::offset_y() const { return impl_->offset_y(); }
std::size_t Camera::payload_size() const { return impl_->payload_size(); }
std::size_t Camera::natural_line_pitch() const { return impl_->natural_line_pitch(); }
std::size_t Camera::buffer_count() const { return impl_->buffer_count(); }
std::size_t Camera::announce_minimum() const { return impl_->announce_minimum(); }
std::size_t Camera::buffer_alignment() const { return impl_->buffer_alignment(); }
const std::string &Camera::pixel_format() const { return impl_->pixel_format(); }
const std::vector<Feature> &Camera::features() const { return impl_->features(); }
const CameraIdentity &Camera::identity() const { return impl_->identity(); }
bool Camera::progressive_supported() const { return impl_->progressive_supported(); }
bool Camera::dma_buf_supported() { return impl_->dma_buf_supported(); }
void Camera::set_frame_callback(FrameCallback callback) { impl_->set_frame_callback(std::move(callback)); }
void Camera::clear_frame_callback() { impl_->clear_frame_callback(); }
void Camera::set_transport_event_callback(TransportEventCallback callback) {
    impl_->set_transport_event_callback(std::move(callback));
}
bool Camera::process_event() { return impl_->process_event(); }
void Camera::select_memory_type(bool direct_dma_buf) { impl_->select_memory_type(direct_dma_buf); }
Euresys::BufferIndexRange Camera::announce(void *base, int fd, std::size_t size,
                                           std::uint32_t offset, bool direct_dma_buf,
                                           void *user_pointer) {
    return impl_->announce(base, fd, size, offset, direct_dma_buf, user_pointer);
}
BufferMetadata Camera::buffer_metadata(Euresys::Buffer &buffer) {
    return impl_->buffer_metadata(buffer);
}
std::optional<BufferProgress> Camera::buffer_progress(
        const Euresys::BufferIndexRange &range) {
    return impl_->buffer_progress(range);
}
std::optional<double> Camera::frame_rate() { return impl_->frame_rate(); }
std::optional<std::pair<double, double>> Camera::frame_rate_range() { return impl_->frame_rate_range(); }
bool Camera::negotiate_frame_rate(double fps) { return impl_->negotiate_frame_rate(fps); }
void Camera::recycle(Euresys::Buffer &buffer) { impl_->recycle(buffer); }
void Camera::release(const std::vector<Euresys::BufferIndexRange> &ranges) { impl_->release(ranges); }
void Camera::start() { impl_->start(); }
void Camera::stop() { impl_->stop(); }
void Camera::disable_events() { impl_->disable_events(); }
void Camera::set_feature(const Feature &feature, const spa_pod *value,
                         bool allow_buffer_change) {
    impl_->set_feature(feature, value, allow_buffer_change);
}
void Camera::execute_command(const Feature &feature) { impl_->execute_command(feature); }
std::string Camera::feature_text(const Feature &feature) { return impl_->feature_text(feature); }
FeatureValue Camera::feature_value(const Feature &feature) {
    return impl_->feature_value(feature);
}
std::optional<std::pair<std::int64_t, std::int64_t>>
Camera::feature_integer_range(const Feature &feature) {
    return impl_->feature_integer_range(feature);
}
std::optional<std::pair<double, double>>
Camera::feature_float_range(const Feature &feature) {
    return impl_->feature_float_range(feature);
}

std::vector<DiscoveredCamera> discover_cameras(const Options &options) {
    EGenTL gentl(producer_path(options.producer));
    Euresys::EGrabberDiscovery discovery(gentl);
    discovery.discover();
    std::vector<DiscoveredCamera> result;
    result.reserve(discovery.cameraCount());
    for (int index = 0; index < discovery.cameraCount(); ++index) {
        const auto camera = discovery.cameras(index, options.stream_index);
        if (camera.grabbers.empty()) continue;
        const auto &info = camera.grabbers.front();
        result.push_back({
            camera_identity(info), info.interfaceIndex, info.deviceIndex,
            info.streamIndex,
        });
    }
    return result;
}

} // namespace egrabber_pipewire

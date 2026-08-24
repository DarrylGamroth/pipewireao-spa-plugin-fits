/* SPDX-License-Identifier: MIT */

#pragma once

#include "options.hpp"
#include "feature.hpp"

#include <EGrabber.h>

#include <spa/pod/builder.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct spa_log;

namespace egrabber_pipewire {

class BufferChangeRequired : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CameraIdentity {
    std::string vendor;
    std::string model;
    std::string serial;
    std::string user_id;
    std::string transport;
    std::string interface_id;
    std::string device_id;
    std::string stream_id;
};

struct DiscoveredCamera {
    CameraIdentity identity;
    int interface_index = 0;
    int device_index = 0;
    int stream_index = 0;
};

enum class TransportEventKind { data_stream, device_error, remote_device };

struct TransportEvent {
    TransportEventKind kind = TransportEventKind::data_stream;
    std::uint64_t timestamp = 0;
    std::uint32_t id = 0;
    std::uint32_t context1 = 0;
    std::uint32_t context2 = 0;
    std::uint32_t context3 = 0;
};

struct BufferMetadata {
    std::optional<std::uint64_t> frame_id;
    std::optional<std::uint64_t> timestamp_ns;
    std::optional<std::size_t> image_offset;
    std::optional<std::size_t> size_filled;
    std::optional<std::size_t> data_size;
    std::optional<std::size_t> x_padding;
    std::optional<std::size_t> payload_type;
    std::optional<bool> image_present;
    std::optional<bool> data_larger_than_buffer;
    std::optional<bool> incomplete;
};

struct BufferProgress {
    std::size_t size_filled = 0;
    std::optional<std::uint64_t> frame_id;
};

class Camera {
public:
    using FrameCallback = std::function<void(const Euresys::NewBufferData &)>;
    using TransportEventCallback = std::function<void(const TransportEvent &)>;

    Camera(const Options &options, struct spa_log *log);
    ~Camera();
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;

    std::size_t width() const;
    std::size_t height() const;
    std::size_t offset_x() const;
    std::size_t offset_y() const;
    std::size_t payload_size() const;
    std::size_t natural_line_pitch() const;
    std::size_t buffer_count() const;
    std::size_t announce_minimum() const;
    std::size_t buffer_alignment() const;
    const std::string &pixel_format() const;
    const std::vector<Feature> &features() const;
    const CameraIdentity &identity() const;
    bool progressive_supported() const;
    bool dma_buf_supported();
    void set_frame_callback(FrameCallback callback);
    void clear_frame_callback();
    void set_transport_event_callback(TransportEventCallback callback);
    bool process_event();
    void select_memory_type(bool direct_dma_buf);
    Euresys::BufferIndexRange announce(void *base, int fd, std::size_t size,
                                       std::uint32_t offset, bool direct_dma_buf,
                                       void *user_pointer);
    BufferMetadata buffer_metadata(Euresys::Buffer &buffer);
    std::optional<BufferProgress> buffer_progress(
        const Euresys::BufferIndexRange &range);
    std::optional<double> frame_rate();
    std::optional<std::pair<double, double>> frame_rate_range();
    bool negotiate_frame_rate(double frames_per_second);
    void recycle(Euresys::Buffer &buffer);
    void release(const std::vector<Euresys::BufferIndexRange> &ranges);
    void start();
    void stop();
    void disable_events();

    void set_feature(const Feature &feature, const spa_pod *value,
                     bool allow_buffer_change = false);
    void execute_command(const Feature &feature);
    std::string feature_text(const Feature &feature);
    FeatureValue feature_value(const Feature &feature);
    std::optional<std::pair<std::int64_t, std::int64_t>>
        feature_integer_range(const Feature &feature);
    std::optional<std::pair<double, double>>
        feature_float_range(const Feature &feature);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<DiscoveredCamera> discover_cameras(const Options &options);

} // namespace egrabber_pipewire

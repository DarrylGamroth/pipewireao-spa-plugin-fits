/* SPDX-License-Identifier: MIT */

#pragma once

#include "feature.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace egrabber_pipewire {

class SerialTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SerialTransport {
public:
    virtual ~SerialTransport() = default;
    virtual void open() = 0;
    virtual void close() noexcept = 0;
    virtual void flush() = 0;
    virtual std::size_t read(void *buffer, std::size_t size,
                             std::uint32_t timeout_ms) = 0;
    virtual std::size_t write(const void *buffer, std::size_t size,
                              std::uint32_t timeout_ms) = 0;
    virtual std::uint32_t supported_baud_rates() = 0;
    virtual void set_baud_rate(std::uint32_t baud_rate) = 0;
};

class ControlBackend {
public:
    virtual ~ControlBackend() = default;
    virtual const char *name() const = 0;
    virtual const std::vector<Feature> &features() const = 0;
    virtual bool readable(const Feature &feature) = 0;
    virtual bool writeable(const Feature &feature) = 0;
    virtual std::int64_t get_integer(const Feature &feature) = 0;
    virtual double get_float(const Feature &feature) = 0;
    virtual std::string get_string(const Feature &feature) = 0;
    virtual void set_integer(const Feature &feature, std::int64_t value) = 0;
    virtual void set_float(const Feature &feature, double value) = 0;
    virtual void set_string(const Feature &feature, const std::string &value) = 0;
    virtual void execute(const Feature &feature) = 0;
    virtual std::optional<std::pair<std::int64_t, std::int64_t>>
        integer_range(const Feature &feature) = 0;
    virtual std::optional<std::pair<double, double>>
        float_range(const Feature &feature) = 0;
    virtual bool requires_transport_layout_sync() const { return false; }
};

struct CLProtocolControlOptions {
    std::vector<std::string> libraries;
    std::optional<std::string> device_template;
    std::optional<std::string> camera_serial;
    std::optional<std::string> genapi_runtime;
    std::uint32_t timeout_ms = 1000;
};

std::vector<std::string> discover_clprotocol_libraries(
    const CLProtocolControlOptions &options);
std::unique_ptr<ControlBackend> make_clprotocol_control_backend(
    SerialTransport &serial, const CLProtocolControlOptions &options);

} // namespace egrabber_pipewire

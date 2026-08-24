/* SPDX-License-Identifier: MIT */

#include "egrabber_control.hpp"

#include <CLProtocol/ClSerialTypes.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace egrabber_pipewire {
namespace {

using Euresys::DeviceModule;

class GrablinkSerialTransport final : public SerialTransport {
public:
    explicit GrablinkSerialTransport(EGrabberOnDemand &grabber) : grabber_(grabber) {}

    void open() override {
        operation("Open");
        open_ = true;
        try { grabber_.setInteger<DeviceModule>("SerialAccessBufferLength", 4096); }
        catch (...) {}
        try {
            access_buffer_length_ = static_cast<std::size_t>(
                grabber_.getInteger<DeviceModule>("SerialAccessBufferLength"));
        } catch (...) { access_buffer_length_ = 4096; }
        access_buffer_length_ = std::max<std::size_t>(1, access_buffer_length_);
    }

    void close() noexcept override {
        if (!open_) return;
        try { operation("Close"); } catch (...) {}
        open_ = false;
    }

    void flush() override { operation("Flush"); }

    std::size_t read(void *buffer, std::size_t size, std::uint32_t timeout_ms) override {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms);
        std::size_t available = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            available = static_cast<std::size_t>(
                grabber_.getInteger<DeviceModule>("SerialReadQueueSize"));
            if (available != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (available == 0) throw SerialTimeout("Camera Link serial read timed out");
        const auto requested = std::min({size, access_buffer_length_, available});
        grabber_.setInteger<DeviceModule>("SerialTimeout", timeout_ms);
        grabber_.setInteger<DeviceModule>("SerialAccessLength", requested);
        operation("Read", true);
        const auto result = static_cast<std::size_t>(
            grabber_.getInteger<DeviceModule>("SerialOperationResult"));
        if (result == 0) throw SerialTimeout("Camera Link serial read timed out");
        const auto count = std::min(result, requested);
        grabber_.getRegister<DeviceModule>("SerialAccessBuffer", buffer, count);
        return count;
    }

    std::size_t write(const void *buffer, std::size_t size,
                      std::uint32_t timeout_ms) override {
        if (size > access_buffer_length_)
            throw std::runtime_error(
                "Camera Link serial write exceeds SerialAccessBufferLength");
        grabber_.setInteger<DeviceModule>("SerialTimeout", timeout_ms);
        grabber_.setInteger<DeviceModule>("SerialAccessLength", size);
        grabber_.setRegister<DeviceModule>("SerialAccessBuffer", buffer, size);
        operation("Write", true);
        try {
            return static_cast<std::size_t>(
                grabber_.getInteger<DeviceModule>("SerialOperationResult"));
        } catch (...) { return size; }
    }

    std::uint32_t supported_baud_rates() override {
        std::uint32_t result = 0;
        for (const auto &entry : grabber_.getStringList<DeviceModule>(
                 Euresys::query::enumEntries("SerialBaudRate", true))) {
            if (entry == "Baud9600") result |= CL_BAUDRATE_9600;
            else if (entry == "Baud19200") result |= CL_BAUDRATE_19200;
            else if (entry == "Baud38400") result |= CL_BAUDRATE_38400;
            else if (entry == "Baud57600") result |= CL_BAUDRATE_57600;
            else if (entry == "Baud115200") result |= CL_BAUDRATE_115200;
            else if (entry == "Baud230400") result |= CL_BAUDRATE_230400;
            else if (entry == "Baud460800") result |= CL_BAUDRATE_460800;
            else if (entry == "Baud921600") result |= CL_BAUDRATE_921600;
        }
        return result;
    }

    void set_baud_rate(std::uint32_t baud_rate) override {
        const char *value = nullptr;
        switch (baud_rate) {
        case CL_BAUDRATE_9600: value = "Baud9600"; break;
        case CL_BAUDRATE_19200: value = "Baud19200"; break;
        case CL_BAUDRATE_38400: value = "Baud38400"; break;
        case CL_BAUDRATE_57600: value = "Baud57600"; break;
        case CL_BAUDRATE_115200: value = "Baud115200"; break;
        case CL_BAUDRATE_230400: value = "Baud230400"; break;
        case CL_BAUDRATE_460800: value = "Baud460800"; break;
        case CL_BAUDRATE_921600: value = "Baud921600"; break;
        default:
            throw std::runtime_error("unsupported Camera Link serial baud-rate value");
        }
        grabber_.setString<DeviceModule>("SerialBaudRate", value);
    }

private:
    void operation(const char *selector, bool timeout_is_normal = false) {
        grabber_.setString<DeviceModule>("SerialOperationSelector", selector);
        grabber_.execute<DeviceModule>("SerialOperationExecute");
        const auto status = grabber_.getString<DeviceModule>("SerialOperationStatus");
        if (status == "Success") return;
        if (timeout_is_normal && status.find("Timeout") != std::string::npos)
            throw SerialTimeout(std::string(selector) + " " + status);
        throw std::runtime_error(
            std::string("Camera Link serial ") + selector + " " + status);
    }

    EGrabberOnDemand &grabber_;
    std::size_t access_buffer_length_ = 4096;
    bool open_ = false;
};

} // namespace

std::unique_ptr<SerialTransport> make_grablink_serial_transport(
        EGrabberOnDemand &grabber) {
    return std::make_unique<GrablinkSerialTransport>(grabber);
}

} // namespace egrabber_pipewire

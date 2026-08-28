/* SPDX-License-Identifier: MIT */

#include "control_backend.hpp"

#include <CLProtocol/ClSerialTypes.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace egrabber_pipewire;

namespace {

class FakeSerial final : public SerialTransport {
public:
    void open() override { open_ = true; }
    void close() noexcept override { open_ = false; }
    void flush() override {
        if (fail_flush)
            throw std::runtime_error("injected flush failure");
        response_.clear();
    }

    std::size_t read(void *buffer, std::size_t size, std::uint32_t) override {
        if (response_.empty()) throw SerialTimeout("no response");
        const auto count = std::min(size, response_.size());
        std::memcpy(buffer, response_.data(), count);
        response_.erase(0, count);
        return count;
    }

    std::size_t write(const void *buffer, std::size_t size, std::uint32_t) override {
        last_command.assign(static_cast<const char *>(buffer), size);
        if (!last_command.empty() && last_command.back() == '\n') last_command.pop_back();
        response_ = response_for(last_command) + "\r\nfli-cli>";
        return size;
    }

    std::uint32_t supported_baud_rates() override {
        return CL_BAUDRATE_9600 | CL_BAUDRATE_115200;
    }
    void set_baud_rate(std::uint32_t value) override { baud_rate = value; }

    bool open_ = false;
    bool fail_flush = false;
    std::uint32_t baud_rate = 0;
    std::string last_command;

private:
    std::string response_for(const std::string &command) {
        if (command == "cameratype raw") return "C-RED 2";
        if (command == "hwuid raw") return "CRED2-TEST-001";
        if (command == "version firmware raw") return "3.0";
        if (command == "fps raw") return std::to_string(fps_);
        if (command == "minfps raw") return "0.001";
        if (command == "maxfps raw") return "600";
        if (command == "tint raw") return "0.00125";
        if (command == "mintint raw") return "0.00005";
        if (command == "maxtint raw" || command == "maxtintitr raw") return "1.0";
        if (command == "cropping raw") return "on";
        if (command == "cropping columns raw")
            return std::to_string(first_column_) + " " + std::to_string(last_column_);
        if (command == "cropping rows raw") return "0 511";
        if (command.starts_with("set fps ")) {
            fps_ = std::stod(command.substr(8));
            return "Result: OK";
        }
        if (command.starts_with("set cropping columns ")) {
            const auto values = command.substr(std::strlen("set cropping columns "));
            const auto split = values.find(' ');
            first_column_ = std::stoi(values.substr(0, split));
            last_column_ = std::stoi(values.substr(split + 1));
            return "Result: OK";
        }
        if (command.starts_with("set ") || command.starts_with("exec ") ||
            command == "save" || command == "continue") return "Result: OK";
        if (command.ends_with(" raw")) return "0";
        return "Result: OK";
    }

    std::string response_;
    double fps_ = 120.0;
    int first_column_ = 64;
    int last_column_ = 319;
};

const Feature &feature(const ControlBackend &backend, std::string_view name) {
    const auto &features = backend.features();
    const auto found = std::find_if(features.begin(), features.end(),
        [&](const Feature &candidate) { return candidate.name == name; });
    if (found == features.end())
        throw std::runtime_error("missing feature " + std::string(name));
    return *found;
}

} // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    {
        FakeSerial failed_serial;
        failed_serial.fail_flush = true;
        CLProtocolControlOptions failed_options;
        failed_options.libraries.push_back(argv[1]);
        bool failed = false;
        try {
            auto unexpected = make_clprotocol_control_backend(
                failed_serial, failed_options);
            (void)unexpected;
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed);
        assert(!failed_serial.open_);
    }
    {
        FakeSerial rejected_serial;
        CLProtocolControlOptions rejected_options;
        rejected_options.libraries.push_back(argv[1]);
        rejected_options.camera_serial = "A-DIFFERENT-CAMERA";
        rejected_options.timeout_ms = 100;
        bool rejected = false;
        try {
            auto unexpected = make_clprotocol_control_backend(rejected_serial, rejected_options);
            (void)unexpected;
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
        assert(!rejected_serial.open_);
    }

    FakeSerial serial;
    CLProtocolControlOptions options;
    options.libraries.push_back(argv[1]);
    options.camera_serial = "CRED2-TEST-001";
    options.timeout_ms = 100;
    auto backend = make_clprotocol_control_backend(serial, options);

    assert(std::string(backend->name()) == "clprotocol");
    assert(serial.open_);
    assert(serial.baud_rate == CL_BAUDRATE_115200);
    assert(backend->get_string(feature(*backend, "DeviceVendorName")) ==
           "First Light Imaging");
    assert(backend->get_string(feature(*backend, "DeviceSerialNumber")) == "CRED2-TEST-001");
    assert(feature(*backend, "DeviceSerialNumber").property_name ==
           "genicam.DeviceSerialNumber");
    assert(feature(*backend, "DeviceSerialNumber").group == "DeviceControl");
    assert(feature(*backend, "DeviceSerialNumber").visibility == "Expert");
    assert(feature(*backend, "AcquisitionFrameRate").group == "AcquisitionControl");
    assert(backend->get_float(feature(*backend, "AcquisitionFrameRate")) == 120.0);
    const auto rate_range = backend->float_range(feature(*backend, "AcquisitionFrameRate"));
    assert(rate_range);
    assert(std::abs(rate_range->first - 0.001) < 1e-6);
    assert(rate_range->second == 600.0);
    assert(backend->get_float(feature(*backend, "ExposureTime")) == 1250.0);
    const auto exposure_range = backend->float_range(feature(*backend, "ExposureTime"));
    assert(exposure_range);
    assert(exposure_range->first == 50.0);
    assert(exposure_range->second == 1000000.0);
    assert(backend->get_string(feature(*backend, "PixelFormat")) == "Mono16");
    assert(backend->get_integer(feature(*backend, "OffsetX")) == 64);
    assert(backend->get_integer(feature(*backend, "Width")) == 256);

    backend->set_float(feature(*backend, "AcquisitionFrameRate"), 200.0);
    assert(serial.last_command.starts_with("set fps 200"));
    backend->set_integer(feature(*backend, "Width"), 320);
    assert(serial.last_command == "set cropping columns 64 383");
    backend->set_integer(feature(*backend, "CropEnable"), 0);
    assert(serial.last_command == "set cropping off");

    backend->set_string(feature(*backend, "IpAddress"), "192.0.2.1");
    assert(serial.last_command == "set ip address 192.0.2.1");

    const auto &sensibility = feature(*backend, "SensitivityMode");
    assert(sensibility.kind == FeatureKind::enumeration);
    assert(!sensibility.enum_entries.empty());
    backend->set_string(sensibility, "High");
    assert(serial.last_command == "set sensibility high");

    backend->set_integer(feature(*backend, "SoftwareSynchro"), 1);
    assert(serial.last_command == "set swsynchro on");
    backend->set_integer(feature(*backend, "NbFramesPerSwTrig"), 4);
    assert(serial.last_command == "set nbframesperswtrig 4");
    backend->set_string(feature(*backend, "TriggerSource"), "Software");
    assert(serial.last_command == "set swsynchro source swtrig");
    backend->execute(feature(*backend, "TriggerSoftware"));
    assert(serial.last_command == "swtrig");

    backend->set_integer(feature(*backend, "UnsignedPixels"), 1);
    assert(serial.last_command == "set unsigned on");
    backend->set_string(feature(*backend, "HdrCalibration"), "C2");
    assert(serial.last_command == "set hdr calibration c2");
    backend->set_string(feature(*backend, "Tuning"), "ShortExposure");
    assert(serial.last_command == "set tuning short_exposure");
    backend->set_integer(feature(*backend, "TargetTemperature"), -45);
    assert(serial.last_command == "set temperatures snake -45");

    backend->execute(feature(*backend, "ContinueAfterError"));
    assert(serial.last_command == "continue");

    backend.reset();
    assert(!serial.open_);
    std::cout << "clprotocol-control-test OK\n";
}

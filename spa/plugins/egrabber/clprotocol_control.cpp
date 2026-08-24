/* SPDX-License-Identifier: MIT */

#include "control_backend.hpp"

#include <CLProtocol/CLProtocol.h>
#include <CLProtocol/ISerial.h>
#include <GenApiC.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace egrabber_pipewire {
namespace {

namespace fs = std::filesystem;

#ifndef DEFAULT_GENAPI_RUNTIME
#define DEFAULT_GENAPI_RUNTIME ""
#endif

template<typename Function>
Function load_symbol(void *library, const char *name) {
    dlerror();
    auto result = reinterpret_cast<Function>(dlsym(library, name));
    if (const char *error = dlerror())
        throw std::runtime_error(std::string("missing symbol ") + name + ": " + error);
    return result;
}

std::vector<std::string> split_paths(const char *value) {
    std::vector<std::string> result;
    if (!value) return result;
    std::string input(value);
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto end = input.find(':', start);
        auto part = input.substr(start, end == std::string::npos ? end : end - start);
        if (!part.empty()) result.push_back(std::move(part));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

void add_clprotocol_path(const fs::path &input, std::vector<std::string> &result) {
    std::error_code error;
    if (fs::is_regular_file(input, error)) {
        result.push_back(input.string());
        return;
    }
    std::vector<fs::path> directories{input};
    if (sizeof(void *) == 8) directories.insert(directories.begin(), input / "Linux64_x64");
    else directories.insert(directories.begin(), input / "Linux32_i86");
    for (const auto &directory : directories) {
        if (!fs::is_directory(directory, error)) continue;
        std::vector<fs::path> libraries;
        for (const auto &entry : fs::directory_iterator(directory, error)) {
            if (error || !entry.is_regular_file()) continue;
            const auto filename = entry.path().filename().string();
            if (filename.starts_with("libCLProtocol_") && filename.ends_with(".so"))
                libraries.push_back(entry.path());
        }
        std::sort(libraries.begin(), libraries.end());
        for (const auto &library : libraries) result.push_back(library.string());
    }
}

std::vector<std::string> nul_list(const char *data, std::size_t size) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < size && data[offset] != '\0') {
        const std::size_t length = strnlen(data + offset, size - offset);
        if (length == size - offset) break;
        result.emplace_back(data + offset, length);
        offset += length + 1;
    }
    return result;
}

std::vector<std::string> tab_list(const std::string &value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('\t', start);
        auto item = value.substr(start, end == std::string::npos ? end : end - start);
        if (!item.empty()) result.push_back(std::move(item));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::string unqualified_node_name(const std::string &name) {
    const auto separator = name.rfind("::");
    return separator == std::string::npos ? name : name.substr(separator + 2);
}

class SerialAdapter final : public ISerial {
public:
    explicit SerialAdapter(SerialTransport &transport) : transport_(transport) {}

    CLINT32 CLPROTOCOL clSerialRead(CLINT8 *buffer, CLUINT32 *buffer_size,
                                    CLUINT32 timeout) override {
        if (!buffer || !buffer_size) return CL_ERR_INVALID_PTR;
        try {
            const auto read = transport_.read(buffer, *buffer_size, timeout);
            if (read == 0) {
                *buffer_size = 0;
                return CL_ERR_TIMEOUT;
            }
            *buffer_size = static_cast<CLUINT32>(read);
            return CL_ERR_NO_ERR;
        } catch (const SerialTimeout &) {
            *buffer_size = 0;
            return CL_ERR_TIMEOUT;
        } catch (...) {
            *buffer_size = 0;
            return CL_ERR_INVALID_REFERENCE;
        }
    }

    CLINT32 CLPROTOCOL clSerialWrite(CLINT8 *buffer, CLUINT32 *buffer_size,
                                     CLUINT32 timeout) override {
        if (!buffer || !buffer_size) return CL_ERR_INVALID_PTR;
        try {
            const auto written = transport_.write(buffer, *buffer_size, timeout);
            *buffer_size = static_cast<CLUINT32>(written);
            return written == 0 ? CL_ERR_TIMEOUT : CL_ERR_NO_ERR;
        } catch (const SerialTimeout &) {
            return CL_ERR_TIMEOUT;
        } catch (...) {
            return CL_ERR_INVALID_REFERENCE;
        }
    }

    CLINT32 CLPROTOCOL clGetSupportedBaudRates(CLUINT32 *baud_rates) override {
        if (!baud_rates) return CL_ERR_INVALID_PTR;
        try {
            *baud_rates = transport_.supported_baud_rates();
            return CL_ERR_NO_ERR;
        } catch (...) {
            return CL_ERR_INVALID_REFERENCE;
        }
    }

    CLINT32 CLPROTOCOL clSetBaudRate(CLUINT32 baud_rate) override {
        try {
            transport_.set_baud_rate(baud_rate);
            return CL_ERR_NO_ERR;
        } catch (...) {
            return CL_ERR_BAUD_RATE_NOT_SUPPORTED;
        }
    }

private:
    SerialTransport &transport_;
};

class SerialConnection {
public:
    explicit SerialConnection(SerialTransport &transport) : transport_(transport) {
        transport_.open();
        open_ = true;
        try {
            transport_.flush();
        } catch (...) {
            transport_.close();
            open_ = false;
            throw;
        }
    }
    ~SerialConnection() { if (open_) transport_.close(); }
    SerialConnection(const SerialConnection &) = delete;
    SerialConnection &operator=(const SerialConnection &) = delete;

private:
    SerialTransport &transport_;
    bool open_ = false;
};

class ProtocolLibrary {
public:
    explicit ProtocolLibrary(const std::string &path) : path_(path) {
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) throw std::runtime_error("cannot load " + path + ": " + dlerror());
        try {
            init = load_symbol<decltype(init)>(handle_, "clpInitLib");
            close = load_symbol<decltype(close)>(handle_, "clpCloseLib");
            templates = load_symbol<decltype(templates)>(handle_, "clpGetShortDeviceIDTemplates");
            probe = load_symbol<decltype(probe)>(handle_, "clpProbeDevice");
            xml_ids = load_symbol<decltype(xml_ids)>(handle_, "clpGetXMLIDs");
            xml_description = load_symbol<decltype(xml_description)>(handle_, "clpGetXMLDescription");
            read_register = load_symbol<decltype(read_register)>(handle_, "clpReadRegister");
            write_register = load_symbol<decltype(write_register)>(handle_, "clpWriteRegister");
            continue_write = load_symbol<decltype(continue_write)>(handle_, "clpContinueWriteRegister");
            error_text = load_symbol<decltype(error_text)>(handle_, "clpGetErrorText");
            disconnect = load_symbol<decltype(disconnect)>(handle_, "clpDisconnect");
            version = load_symbol<decltype(version)>(handle_, "clpGetCLProtocolVersion");
            const CLINT32 rc = init(nullptr, CLP_LOG_WARN);
            if (rc != CL_ERR_NO_ERR)
                throw std::runtime_error("clpInitLib failed with status " + std::to_string(rc));
            initialized_ = true;
            CLUINT32 major = 0, minor = 0;
            check(version(&major, &minor), 0, "clpGetCLProtocolVersion");
            if (major != 1)
                throw std::runtime_error("unsupported CLProtocol ABI " +
                                         std::to_string(major) + "." + std::to_string(minor));
        } catch (...) {
            if (initialized_) close();
            dlclose(handle_);
            handle_ = nullptr;
            throw;
        }
    }

    ~ProtocolLibrary() {
        if (initialized_) close();
        if (handle_) dlclose(handle_);
    }
    ProtocolLibrary(const ProtocolLibrary &) = delete;
    ProtocolLibrary &operator=(const ProtocolLibrary &) = delete;

    std::string error(CLINT32 status, CLUINT32 cookie) const {
        std::array<CLINT8, 512> buffer{};
        CLUINT32 size = buffer.size();
        if (error_text(status, buffer.data(), &size, cookie) == CL_ERR_NO_ERR)
            return reinterpret_cast<const char *>(buffer.data());
        return "status " + std::to_string(status);
    }

    void check(CLINT32 status, CLUINT32 cookie, std::string_view operation) const {
        if (status != CL_ERR_NO_ERR)
            throw std::runtime_error(std::string(operation) + " failed: " + error(status, cookie));
    }

    const std::string &path() const { return path_; }

    decltype(&clpInitLib) init = nullptr;
    decltype(&clpCloseLib) close = nullptr;
    decltype(&clpGetShortDeviceIDTemplates) templates = nullptr;
    decltype(&clpProbeDevice) probe = nullptr;
    decltype(&clpGetXMLIDs) xml_ids = nullptr;
    decltype(&clpGetXMLDescription) xml_description = nullptr;
    decltype(&clpReadRegister) read_register = nullptr;
    decltype(&clpWriteRegister) write_register = nullptr;
    decltype(&clpContinueWriteRegister) continue_write = nullptr;
    decltype(&clpGetErrorText) error_text = nullptr;
    decltype(&clpDisconnect) disconnect = nullptr;
    decltype(&clpGetCLProtocolVersion) version = nullptr;

private:
    std::string path_;
    void *handle_ = nullptr;
    bool initialized_ = false;
};

std::string find_genapi_runtime(const CLProtocolControlOptions &options) {
    std::vector<std::string> candidates;
    if (options.genapi_runtime) candidates.push_back(*options.genapi_runtime);
    for (const char *variable : {"GENICAM_ROOT_V3_5", "GENICAM_ROOT"}) {
        if (const char *root = std::getenv(variable))
            candidates.push_back((fs::path(root) / "bin" / "Linux64_x64" /
                                  "libGenApiC_v3.so").string());
    }
    if (std::string(DEFAULT_GENAPI_RUNTIME).size() != 0)
        candidates.emplace_back(DEFAULT_GENAPI_RUNTIME);
    candidates.emplace_back("libGenApiC_v3.so");
    std::ostringstream errors;
    for (const auto &candidate : candidates) {
        void *handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            dlclose(handle);
            return candidate;
        }
        errors << "\n  " << candidate << ": " << dlerror();
    }
    throw std::runtime_error("could not locate the GenApi C runtime; tried:" + errors.str());
}

class GenApiRuntime {
public:
    explicit GenApiRuntime(const std::string &path) : path_(path) {
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) throw std::runtime_error("cannot load " + path + ": " + dlerror());
        try {
            POpenGenApi open = nullptr;
            for (const char *symbol : {"OpenGenApiV_3_5", "OpenGenApiV_3", "OpenGenApi"}) {
                open = reinterpret_cast<POpenGenApi>(dlsym(handle_, symbol));
                if (open) break;
            }
            if (!open) throw std::runtime_error("GenApi runtime has no compatible OpenGenApi entry point");
            functions_.Size = sizeof(functions_);
            const auto rc = open(&functions_);
            if (rc != GenApiSuccess)
                throw std::runtime_error("OpenGenApi failed with status " + std::to_string(rc));
        } catch (...) {
            dlclose(handle_);
            handle_ = nullptr;
            throw;
        }
    }

    ~GenApiRuntime() { if (handle_) dlclose(handle_); }
    GenApiRuntime(const GenApiRuntime &) = delete;
    GenApiRuntime &operator=(const GenApiRuntime &) = delete;

    void check(GenApiError status, std::string_view operation) const {
        if (status == GenApiSuccess) return;
        std::string detail;
        if (functions_.GetLastErrorString) {
            std::size_t size = 0;
            if (functions_.GetLastErrorString(nullptr, &size) == GenApiSuccess && size) {
                detail.resize(size);
                if (functions_.GetLastErrorString(detail.data(), &size) == GenApiSuccess &&
                    !detail.empty() && detail.back() == '\0') detail.pop_back();
            }
        }
        throw std::runtime_error(std::string(operation) + " failed" +
            (detail.empty() ? " with status " + std::to_string(status) : ": " + detail));
    }

    GenApiFunctions &functions() { return functions_; }
    const GenApiFunctions &functions() const { return functions_; }

private:
    std::string path_;
    void *handle_ = nullptr;
    GenApiFunctions functions_{};
};

class CLProtocolBackend final : public ControlBackend {
public:
    CLProtocolBackend(SerialTransport &serial, const std::string &library_path,
                      const CLProtocolControlOptions &options)
        : serial_(serial), connection_(serial), serial_adapter_(serial),
          protocol_(library_path), genapi_(find_genapi_runtime(options)),
          timeout_ms_(options.timeout_ms) {
        try {
            connect_protocol(options);
            create_node_map();
            features_ = inspect_features();
            if (options.camera_serial) {
                const auto feature = find_feature("DeviceSerialNumber");
                if (!feature)
                    throw std::runtime_error(
                        "CLProtocol GenApi node map does not expose DeviceSerialNumber");
                const auto actual = get_string(**feature);
                if (actual != *options.camera_serial)
                    throw std::runtime_error(
                        "attached camera serial does not match "
                        "api.egrabber.camera-serial " +
                        *options.camera_serial + " (reported " +
                        (actual.empty() ? "no serial" : actual) + ")");
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~CLProtocolBackend() override { cleanup(); }

    const char *name() const override { return "clprotocol"; }
    const std::vector<Feature> &features() const override { return features_; }
    bool requires_transport_layout_sync() const override { return true; }

    bool readable(const Feature &feature) override {
        const auto mode = access_mode(node(feature));
        return (mode & GenApiReadOnly) != 0;
    }

    bool writeable(const Feature &feature) override {
        const auto mode = access_mode(node(feature));
        return (mode & GenApiWriteOnly) != 0;
    }

    std::int64_t get_integer(const Feature &feature) override {
        return get_value<std::int64_t>(node(feature), GenApiInt64);
    }

    double get_float(const Feature &feature) override {
        return get_value<double>(node(feature), GenApiFloat64);
    }

    std::string get_string(const Feature &feature) override {
        return get_string_value(node(feature));
    }

    void set_integer(const Feature &feature, std::int64_t value) override {
        set_value(node(feature), GenApiInt64, &value, sizeof(value));
    }

    void set_float(const Feature &feature, double value) override {
        set_value(node(feature), GenApiFloat64, &value, sizeof(value));
    }

    void set_string(const Feature &feature, const std::string &value) override {
        set_value(node(feature), GenApiString, value.data(), value.size());
    }

    void execute(const Feature &feature) override {
        const std::int64_t command = 1;
        set_value(node(feature), GenApiInt64, &command, sizeof(command));
    }

    std::optional<std::pair<std::int64_t, std::int64_t>>
    integer_range(const Feature &feature) override {
        try {
            const auto handle = node(feature);
            return std::pair{get_property<std::int64_t>(handle, GenApiNodeMinValue, GenApiInt64),
                             get_property<std::int64_t>(handle, GenApiNodeMaxValue, GenApiInt64)};
        } catch (...) { return std::nullopt; }
    }

    std::optional<std::pair<double, double>> float_range(const Feature &feature) override {
        try {
            const auto handle = node(feature);
            return std::pair{get_property<double>(handle, GenApiNodeMinValue, GenApiFloat64),
                             get_property<double>(handle, GenApiNodeMaxValue, GenApiFloat64)};
        } catch (...) { return std::nullopt; }
    }

private:
    void cleanup() noexcept {
        if (node_map_.Handle) {
            genapi_.functions().DestroyNodeMap(node_map_);
            node_map_ = {};
        }
        if (cookie_) {
            protocol_.disconnect(cookie_);
            cookie_ = 0;
        }
    }

    void connect_protocol(const CLProtocolControlOptions &options) {
        std::vector<std::string> templates;
        if (options.device_template) {
            templates.push_back(*options.device_template);
        } else {
            CLUINT32 size = 0;
            CLINT32 rc = protocol_.templates(nullptr, &size);
            if (rc != CL_ERR_BUFFER_TOO_SMALL && rc != CL_ERR_NO_ERR)
                protocol_.check(rc, 0, "clpGetShortDeviceIDTemplates");
            std::vector<CLINT8> buffer(std::max<CLUINT32>(size, 1));
            protocol_.check(protocol_.templates(buffer.data(), &size), 0,
                            "clpGetShortDeviceIDTemplates");
            templates = tab_list(reinterpret_cast<const char *>(buffer.data()));
        }
        if (templates.empty()) throw std::runtime_error("CLProtocol library advertises no device templates");

        std::vector<std::string> failures;
        for (const auto &candidate : templates) {
            serial_.flush();
            std::array<CLINT8, 2048> device_id{};
            CLUINT32 size = device_id.size();
            CLUINT32 cookie = 0;
            const CLINT32 rc = protocol_.probe(&serial_adapter_,
                reinterpret_cast<const CLINT8 *>(candidate.c_str()), device_id.data(),
                &size, &cookie, timeout_ms_);
            if (rc == CL_ERR_NO_ERR) {
                cookie_ = cookie;
                device_id_ = reinterpret_cast<const char *>(device_id.data());
                load_xml();
                return;
            }
            failures.push_back(candidate + ": " + protocol_.error(rc, cookie));
        }
        std::ostringstream message;
        message << "no attached camera matched " << protocol_.path();
        for (const auto &failure : failures) message << "\n  " << failure;
        throw std::runtime_error(message.str());
    }

    void load_xml() {
        CLUINT32 ids_size = 0;
        CLINT32 rc = protocol_.xml_ids(&serial_adapter_, cookie_, nullptr, &ids_size, timeout_ms_);
        if (rc != CL_ERR_BUFFER_TOO_SMALL && rc != CL_ERR_NO_ERR)
            protocol_.check(rc, cookie_, "clpGetXMLIDs");
        std::vector<CLINT8> ids(std::max<CLUINT32>(ids_size, 1));
        protocol_.check(protocol_.xml_ids(&serial_adapter_, cookie_, ids.data(), &ids_size,
                                         timeout_ms_), cookie_, "clpGetXMLIDs");
        const auto candidates = tab_list(reinterpret_cast<const char *>(ids.data()));
        if (candidates.empty()) throw std::runtime_error("CLProtocol device provides no GenApi XML IDs");
        xml_id_ = candidates.front();

        CLUINT32 xml_size = 0;
        rc = protocol_.xml_description(&serial_adapter_, cookie_,
            reinterpret_cast<const CLINT8 *>(xml_id_.c_str()), nullptr, &xml_size, timeout_ms_);
        if (rc != CL_ERR_BUFFER_TOO_SMALL && rc != CL_ERR_NO_ERR)
            protocol_.check(rc, cookie_, "clpGetXMLDescription");
        xml_.resize(std::max<CLUINT32>(xml_size, 1));
        protocol_.check(protocol_.xml_description(&serial_adapter_, cookie_,
            reinterpret_cast<const CLINT8 *>(xml_id_.c_str()), xml_.data(), &xml_size,
            timeout_ms_), cookie_, "clpGetXMLDescription");
        xml_.resize(xml_size);
        while (!xml_.empty() && xml_.back() == 0) xml_.pop_back();
    }

    void create_node_map() {
        auto &api = genapi_.functions();
        genapi_.check(api.CreateNodeMapFromMemory(xml_.data(), xml_.size(),
                                                 GenApiCreateFlagDefault, &node_map_),
                      "CreateNodeMapFromMemory");
        GenApiTLPortHandle port{this};
        genapi_.check(api.NodeMapConnectPort(node_map_, "Device", port,
            &CLProtocolBackend::port_read, &CLProtocolBackend::port_write,
            &CLProtocolBackend::port_access), "NodeMapConnectPort(Device)");
    }

    static GenApiError port_read(GenApiTLPortHandle port, std::int64_t address,
                                 void *buffer, std::int64_t length) {
        auto *self = static_cast<CLProtocolBackend *>(port.Handle);
        const CLINT32 rc = self->protocol_.read_register(&self->serial_adapter_, self->cookie_,
            address, static_cast<CLINT8 *>(buffer), length, self->timeout_ms_);
        return rc == CL_ERR_NO_ERR ? GenApiSuccess : GenApiErrorRuntime;
    }

    static GenApiError port_write(GenApiTLPortHandle port, std::int64_t address,
                                  const void *buffer, std::int64_t length) {
        auto *self = static_cast<CLProtocolBackend *>(port.Handle);
        CLINT32 rc = self->protocol_.write_register(&self->serial_adapter_, self->cookie_,
            address, static_cast<const CLINT8 *>(buffer), length, self->timeout_ms_);
        while (rc == CL_ERR_PENDING_WRITE)
            rc = self->protocol_.continue_write(&self->serial_adapter_, self->cookie_, true,
                                                self->timeout_ms_);
        return rc == CL_ERR_NO_ERR ? GenApiSuccess : GenApiErrorRuntime;
    }

    static GenApiError port_access(GenApiTLPortHandle, GenApiAccessMode *mode) {
        if (!mode) return GenApiErrorInvalidArgument;
        *mode = GenApiReadWrite;
        return GenApiSuccess;
    }

    GenApiNodeHandle node(const Feature &feature) const {
        GenApiNodeHandle result{};
        genapi_.check(genapi_.functions().NodeMapGetNode(node_map_, feature.name.c_str(), &result),
                      "NodeMapGetNode(" + feature.name + ")");
        return result;
    }

    std::optional<const Feature *> find_feature(std::string_view name) const {
        const auto found = std::find_if(features_.begin(), features_.end(),
            [&](const Feature &feature) { return feature.name == name; });
        if (found == features_.end()) return std::nullopt;
        return &*found;
    }

    GenApiAccessMode access_mode(GenApiNodeHandle handle) const {
        return static_cast<GenApiAccessMode>(
            get_property<std::int64_t>(handle, GenApiNodeAccessMode, GenApiInt64));
    }

    template<typename T>
    T get_property(GenApiNodeHandle handle, GenApiNodeProperty property,
                   GenApiDataType type) const {
        T result{};
        std::size_t size = sizeof(result);
        auto requested = type;
        genapi_.check(genapi_.functions().NodeGetProperty(handle, property, &requested,
                                                         &result, &size),
                      "NodeGetProperty");
        return result;
    }

    std::string get_string_property(GenApiNodeHandle handle,
                                    GenApiNodeProperty property) const {
        std::size_t size = 0;
        auto type = GenApiString;
        auto &api = genapi_.functions();
        genapi_.check(api.NodeGetProperty(handle, property, &type, nullptr, &size),
                      "NodeGetProperty(size)");
        std::string result(size, '\0');
        genapi_.check(api.NodeGetProperty(handle, property, &type, result.data(), &size),
                      "NodeGetProperty(string)");
        if (!result.empty() && result.back() == '\0') result.pop_back();
        return result;
    }

    template<typename T>
    T get_value(GenApiNodeHandle handle, GenApiDataType type) const {
        T result{};
        std::size_t size = sizeof(result);
        auto requested = type;
        genapi_.check(genapi_.functions().NodeGetValue(handle, GenApiIgnoreCache, &requested,
                                                      &result, &size), "NodeGetValue");
        return result;
    }

    std::string get_string_value(GenApiNodeHandle handle) const {
        std::size_t size = 0;
        auto type = GenApiString;
        auto &api = genapi_.functions();
        genapi_.check(api.NodeGetValue(handle, GenApiIgnoreCache, &type, nullptr, &size),
                      "NodeGetValue(size)");
        std::string result(size, '\0');
        genapi_.check(api.NodeGetValue(handle, GenApiIgnoreCache, &type, result.data(), &size),
                      "NodeGetValue(string)");
        if (!result.empty() && result.back() == '\0') result.pop_back();
        return result;
    }

    void set_value(GenApiNodeHandle handle, GenApiDataType type,
                   const void *value, std::size_t size) {
        genapi_.check(genapi_.functions().NodeSetValue(handle, GenApiWriteValueDefault,
                                                      type, value, size), "NodeSetValue");
    }

    std::vector<std::string> links(GenApiNodeHandle handle, GenApiNodeLinkType type) const {
        auto &api = genapi_.functions();
        std::size_t size = 0;
        genapi_.check(api.NodeGetLinks(handle, type, nullptr, &size), "NodeGetLinks(size)");
        std::vector<char> buffer(size);
        genapi_.check(api.NodeGetLinks(handle, type, buffer.data(), &size), "NodeGetLinks");
        return nul_list(buffer.data(), buffer.size());
    }

    std::vector<Feature> inspect_features() const {
        auto &api = genapi_.functions();
        std::vector<std::string> names;
        std::set<std::string> visited_categories;
        std::set<std::string> visited_features;
        std::function<void(const std::string &)> visit_category = [&](const std::string &name) {
            if (!visited_categories.insert(name).second) return;
            GenApiNodeHandle category{};
            genapi_.check(api.NodeMapGetNode(node_map_, name.c_str(), &category),
                          "NodeMapGetNode(" + name + ")");
            for (const auto &child_name : links(category, GenApiFeatureNodes)) {
                GenApiNodeHandle child{};
                genapi_.check(api.NodeMapGetNode(node_map_, child_name.c_str(), &child),
                              "NodeMapGetNode(" + child_name + ")");
                if (child.NodeType == GenApiCategoryNode) visit_category(child_name);
                else if (visited_features.insert(child_name).second) names.push_back(child_name);
            }
        };
        visit_category("Root");
        if (std::getenv("EGRABBER_CLPROTOCOL_DEBUG")) {
            std::cerr << "CLProtocol feature nodes:";
            for (const auto &name : names) std::cerr << ' ' << name;
            std::cerr << '\n';
        }
        std::vector<Feature> result;
        for (const auto &name : names) {
            try {
                GenApiNodeHandle handle{};
                genapi_.check(api.NodeMapGetNode(node_map_, name.c_str(), &handle),
                              "NodeMapGetNode(" + name + ")");
                const auto interface_type = static_cast<GenApiPrincipalInterfaceType>(
                    get_property<std::int64_t>(handle, GenApiNodePrincipalInterfaceType,
                                               GenApiInt64));
                Feature feature;
                feature.name = unqualified_node_name(name);
                switch (interface_type) {
                case GenApiIBoolean: feature.kind = FeatureKind::boolean; break;
                case GenApiIInteger: feature.kind = FeatureKind::integer; break;
                case GenApiIFloat: feature.kind = FeatureKind::floating; break;
                case GenApiIEnumeration: feature.kind = FeatureKind::enumeration; break;
                case GenApiIString: feature.kind = FeatureKind::string; break;
                case GenApiICommand: feature.kind = FeatureKind::command; break;
                default: continue;
                }
                const auto mode = access_mode(handle);
                feature.readable = (mode & GenApiReadOnly) != 0;
                feature.writeable = (mode & GenApiWriteOnly) != 0;
                feature.property_name = feature.kind == FeatureKind::command
                    ? "genicam-command." + feature.name : "genicam." + feature.name;
                try { feature.description = get_string_property(handle, GenApiNodeToolTip); }
                catch (...) {
                    try { feature.description = get_string_property(handle, GenApiNodeDescription); }
                    catch (...) { feature.description = name; }
                }
                if (feature.description.empty()) feature.description = name;
                if (feature.kind == FeatureKind::enumeration)
                    feature.enum_entries = links(handle, GenApiEntries);
                for (auto &entry : feature.enum_entries)
                    entry = unqualified_node_name(entry);
                result.push_back(std::move(feature));
            } catch (const std::exception &error) {
                std::cerr << "Skipping CLProtocol GenApi feature " << name << ": "
                          << error.what() << '\n';
            }
        }
        return result;
    }

    SerialTransport &serial_;
    SerialConnection connection_;
    SerialAdapter serial_adapter_;
    ProtocolLibrary protocol_;
    GenApiRuntime genapi_;
    std::uint32_t timeout_ms_;
    CLUINT32 cookie_ = 0;
    std::string device_id_;
    std::string xml_id_;
    std::vector<CLINT8> xml_;
    GenApiNodeMapHandle node_map_{};
    std::vector<Feature> features_;
};

} // namespace

std::vector<std::string> discover_clprotocol_libraries(
    const CLProtocolControlOptions &options) {
    std::vector<std::string> result;
    for (const auto &path : options.libraries) add_clprotocol_path(path, result);
    for (const auto &path : split_paths(std::getenv("EGRABBER_CLPROTOCOL_LIBRARY")))
        add_clprotocol_path(path, result);
    for (const auto &path : split_paths(std::getenv("GENICAM_CLPROTOCOL")))
        add_clprotocol_path(path, result);
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (auto &library : result) {
        if (seen.insert(library).second) unique.push_back(std::move(library));
    }
    return unique;
}

std::unique_ptr<ControlBackend> make_clprotocol_control_backend(
    SerialTransport &serial, const CLProtocolControlOptions &options) {
    const auto libraries = discover_clprotocol_libraries(options);
    if (libraries.empty())
        throw std::runtime_error("no CLProtocol backend libraries were found; set "
                                 "GENICAM_CLPROTOCOL or "
                                 "api.egrabber.clprotocol-libraries");
    std::ostringstream failures;
    for (const auto &library : libraries) {
        try {
            return std::make_unique<CLProtocolBackend>(serial, library, options);
        } catch (const std::exception &error) {
            failures << "\n  " << library << ": " << error.what();
        }
    }
    throw std::runtime_error("no installed CLProtocol backend matched the attached camera:" +
                             failures.str());
}

} // namespace egrabber_pipewire

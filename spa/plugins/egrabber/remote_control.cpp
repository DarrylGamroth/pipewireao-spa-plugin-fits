/* SPDX-License-Identifier: MIT */

#include "egrabber_control.hpp"
#include "egrabber.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace egrabber_pipewire {
namespace {

using Euresys::RemoteModule;

FeatureKind feature_kind(const std::vector<std::string> &interfaces, bool command) {
    if (command) return FeatureKind::command;
    if (std::find(interfaces.begin(), interfaces.end(), "IBoolean") != interfaces.end())
        return FeatureKind::boolean;
    if (std::find(interfaces.begin(), interfaces.end(), "IEnumeration") != interfaces.end())
        return FeatureKind::enumeration;
    if (std::find(interfaces.begin(), interfaces.end(), "IInteger") != interfaces.end())
        return FeatureKind::integer;
    if (std::find(interfaces.begin(), interfaces.end(), "IFloat") != interfaces.end())
        return FeatureKind::floating;
    if (std::find(interfaces.begin(), interfaces.end(), "IString") != interfaces.end())
        return FeatureKind::string;
    return FeatureKind::unsupported;
}

class RemoteControlBackend final : public ControlBackend {
public:
    RemoteControlBackend(EGrabberOnDemand &grabber, struct spa_log *log)
        : grabber_(grabber), log_(log) {
        features_ = inspect_features();
    }

    const char *name() const override { return "remote"; }
    const std::vector<Feature> &features() const override { return features_; }
    bool readable(const Feature &feature) override {
        return grabber_.getInteger<RemoteModule>(Euresys::query::readable(feature.name)) != 0;
    }
    bool writeable(const Feature &feature) override {
        return grabber_.getInteger<RemoteModule>(Euresys::query::writeable(feature.name)) != 0;
    }
    std::int64_t get_integer(const Feature &feature) override {
        return grabber_.getInteger<RemoteModule>(feature.name);
    }
    double get_float(const Feature &feature) override {
        return grabber_.getFloat<RemoteModule>(feature.name);
    }
    std::string get_string(const Feature &feature) override {
        return grabber_.getString<RemoteModule>(feature.name);
    }
    void set_integer(const Feature &feature, std::int64_t value) override {
        grabber_.setInteger<RemoteModule>(feature.name, value);
    }
    void set_float(const Feature &feature, double value) override {
        grabber_.setFloat<RemoteModule>(feature.name, value);
    }
    void set_string(const Feature &feature, const std::string &value) override {
        grabber_.setString<RemoteModule>(feature.name, value);
    }
    void execute(const Feature &feature) override {
        grabber_.execute<RemoteModule>(feature.name);
    }
    std::optional<std::pair<std::int64_t, std::int64_t>>
    integer_range(const Feature &feature) override {
        try {
            return std::pair{grabber_.getInteger<RemoteModule>(feature.name + ".Min"),
                             grabber_.getInteger<RemoteModule>(feature.name + ".Max")};
        } catch (...) { return std::nullopt; }
    }
    std::optional<std::pair<double, double>>
    float_range(const Feature &feature) override {
        try {
            return std::pair{grabber_.getFloat<RemoteModule>(feature.name + ".Min"),
                             grabber_.getFloat<RemoteModule>(feature.name + ".Max")};
        } catch (...) { return std::nullopt; }
    }

private:
    std::vector<Feature> inspect_features() {
        std::vector<Feature> result;
        for (const auto &name : grabber_.getStringList<RemoteModule>(
                 Euresys::query::features(false))) {
            try {
                Feature feature;
                feature.name = name;
                feature.readable = readable(feature);
                feature.writeable = writeable(feature);
                const bool command = grabber_.getInteger<RemoteModule>(
                    Euresys::query::command(name)) != 0;
                feature.kind = feature_kind(
                    grabber_.getStringList<RemoteModule>(
                        Euresys::query::interfaces(name)), command);
                feature.property_name = feature.kind == FeatureKind::command
                    ? "genicam-command." + name : "genicam." + name;
                try {
                    feature.description = grabber_.getString<RemoteModule>(
                        Euresys::query::info(name, "Tooltip"));
                } catch (...) { feature.description = name; }
                if (feature.description.empty()) feature.description = name;
                if (feature.kind == FeatureKind::enumeration)
                    feature.enum_entries = grabber_.getStringList<RemoteModule>(
                        Euresys::query::enumEntries(name, false));
                result.push_back(std::move(feature));
            } catch (const std::exception &error) {
                spa_log_warn(log_, "skipping GenICam feature %s: %s",
                             name.c_str(), error.what());
            }
        }
        return result;
    }

    EGrabberOnDemand &grabber_;
    struct spa_log *log_;
    std::vector<Feature> features_;
};

class NullControlBackend final : public ControlBackend {
public:
    const char *name() const override { return "none"; }
    const std::vector<Feature> &features() const override { return features_; }
    bool readable(const Feature &) override { return false; }
    bool writeable(const Feature &) override { return false; }
    std::int64_t get_integer(const Feature &) override { throw unavailable(); }
    double get_float(const Feature &) override { throw unavailable(); }
    std::string get_string(const Feature &) override { throw unavailable(); }
    void set_integer(const Feature &, std::int64_t) override { throw unavailable(); }
    void set_float(const Feature &, double) override { throw unavailable(); }
    void set_string(const Feature &, const std::string &) override { throw unavailable(); }
    void execute(const Feature &) override { throw unavailable(); }
    std::optional<std::pair<std::int64_t, std::int64_t>>
    integer_range(const Feature &) override { return std::nullopt; }
    std::optional<std::pair<double, double>>
    float_range(const Feature &) override { return std::nullopt; }

private:
    static std::runtime_error unavailable() {
        return std::runtime_error("camera control is disabled");
    }
    std::vector<Feature> features_;
};

} // namespace

std::unique_ptr<ControlBackend> make_remote_control_backend(
        EGrabberOnDemand &grabber, struct spa_log *log) {
    return std::make_unique<RemoteControlBackend>(grabber, log);
}

std::unique_ptr<ControlBackend> make_null_control_backend() {
    return std::make_unique<NullControlBackend>();
}

} // namespace egrabber_pipewire

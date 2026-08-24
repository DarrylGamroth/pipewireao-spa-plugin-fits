/* SPDX-License-Identifier: MIT */

#pragma once

#include "control_backend.hpp"

#include <EGrabber.h>

#include <memory>

struct spa_log;

namespace egrabber_pipewire {

using EGrabberOnDemand = Euresys::EGrabber<Euresys::CallbackOnDemand>;

std::unique_ptr<ControlBackend> make_remote_control_backend(
    EGrabberOnDemand &grabber, struct spa_log *log);
std::unique_ptr<ControlBackend> make_null_control_backend();
#ifdef HAVE_CLPROTOCOL_CONTROL
std::unique_ptr<SerialTransport> make_grablink_serial_transport(
    EGrabberOnDemand &grabber);
#endif

} // namespace egrabber_pipewire

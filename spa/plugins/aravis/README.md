# Aravis comparison source

`api.aravis.source` is retained as an experimental comparison backend. It is
not part of the supported PipeWireAO RTC camera set. Measurements with the
Euresys Gigelink GenTL producer showed substantially higher completion-poll
cost than the direct eGrabber and BGAPI2 integrations.

The plugin remains useful for functional GenTL compatibility experiments and
for comparing a GLib-based acquisition stack with the vendor-specific
adapters. It supports complete frames in mapped host memory. It does not
provide row-block acquisition, native Aravis GigE Vision qualification,
hotplug discovery, or a real-time performance claim.

Build it explicitly against Aravis 0.10:

```console
meson setup build-aravis -Daravis=enabled
meson compile -C build-aravis
meson test -C build-aravis spa-aravis-factory --print-errorlogs
```

Hardware tests are built but are not registered as automatic tests because a
device identifier and GenTL producer environment are site-specific:

```console
GENICAM_GENTL64_PATH=/path/to/cti-directory \
  build-aravis/spa/plugins/aravis/spa-aravis-camera-test DEVICE-ID

GENICAM_GENTL64_PATH=/path/to/cti-directory \
  build-aravis/spa/plugins/aravis/spa-aravis-capture-test \
  build-aravis/spa/plugins/aravis/libspa-aravis.so DEVICE-ID
```

`spa-aravis-completion-benchmark` remains available for controlled comparison
work. Results from it must not be presented as qualification of the supported
eGrabber or BGAPI2 paths.

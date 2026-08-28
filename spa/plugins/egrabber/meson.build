egrabber_sources = [
  'acquisition_key.cpp',
  'buffer_memory.cpp',
  'camera.cpp',
  'device.cpp',
  'feature.cpp',
  'frame_layout.cpp',
  'frame_sequence.cpp',
  'manager.cpp',
  'options.cpp',
  'params.cpp',
  'plugin.cpp',
  'remote_control.cpp',
  'source.cpp',
  'timestamp_mapper.cpp',
]

egrabber_dependencies = [spa_dep, egrabber_dep, threads_dep,
  genicam_feature_dep]
egrabber_cpp_args = ['-Wno-pedantic']
egrabber_includes = []

genicam_root = get_option('genicam-root')
if genicam_root != ''
  genicam_c_include = genicam_root / 'Reference Implementation' / 'library' / 'C' / 'include'
  genicam_cpp_include = genicam_root / 'Reference Implementation' / 'library' / 'CPP' / 'include'
  genapi_runtime = genicam_root / 'Reference Implementation' / 'bin' / 'Linux64_x64' / 'libGenApiC_v3.so'
  if not fs.exists(genicam_c_include / 'GenApiC.h') or not fs.exists(genicam_cpp_include / 'CLProtocol' / 'ISerial.h')
    error('genicam-root does not contain the GenICam Reference Implementation C and C++ headers')
  endif
  if not fs.exists(genapi_runtime)
    error('genicam-root does not contain Reference Implementation/bin/Linux64_x64/libGenApiC_v3.so')
  endif
  egrabber_sources += ['clprotocol_control.cpp', 'grablink_serial.cpp']
  egrabber_includes += include_directories(genicam_c_include, genicam_cpp_include)
  egrabber_cpp_args += [
    '-DHAVE_CLPROTOCOL_CONTROL=1',
    '-DDEFAULT_GENAPI_RUNTIME="' + genapi_runtime + '"',
  ]
endif

if drm_dep.found()
  egrabber_sources += 'dma_buf_sync.cpp'
  egrabber_dependencies += drm_dep
  egrabber_cpp_args += '-DHAVE_EGRABBER_DRM'
endif

egrabber_lib = shared_library(
  'spa-egrabber',
  egrabber_sources,
  include_directories: egrabber_includes + [plugin_include],
  dependencies: egrabber_dependencies,
  cpp_args: egrabber_cpp_args,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'egrabber',
)

if get_option('tests').allowed()
  egrabber_feature_test = executable(
    'spa-egrabber-feature-test',
    ['test-feature.cpp', 'feature.cpp'],
    dependencies: [spa_dep, genicam_feature_dep],
  )
  test('spa-egrabber-feature', egrabber_feature_test)

  egrabber_buffer_memory_test = executable(
    'spa-egrabber-buffer-memory-test',
    ['test-buffer-memory.cpp', 'buffer_memory.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-buffer-memory', egrabber_buffer_memory_test)

  if drm_dep.found()
    egrabber_dma_buf_sync_test = executable(
      'spa-egrabber-dma-buf-sync-test',
      ['test-dma-buf-sync.cpp', 'dma_buf_sync.cpp'],
      dependencies: [spa_dep, drm_dep],
    )
    test('spa-egrabber-dma-buf-sync', egrabber_dma_buf_sync_test)
  endif

  egrabber_acquisition_key_test = executable(
    'spa-egrabber-acquisition-key-test',
    ['test-acquisition-key.cpp', 'acquisition_key.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-acquisition-key', egrabber_acquisition_key_test)

  egrabber_frame_layout_test = executable(
    'spa-egrabber-frame-layout-test',
    ['test-frame-layout.cpp', 'frame_layout.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-frame-layout', egrabber_frame_layout_test)

  egrabber_frame_sequence_test = executable(
    'spa-egrabber-frame-sequence-test',
    ['test-frame-sequence.cpp', 'frame_sequence.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-frame-sequence', egrabber_frame_sequence_test)

  egrabber_timestamp_mapper_test = executable(
    'spa-egrabber-timestamp-mapper-test',
    ['test-timestamp-mapper.cpp', 'timestamp_mapper.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-timestamp-mapper', egrabber_timestamp_mapper_test)

  egrabber_options_test = executable(
    'spa-egrabber-options-test',
    ['test-options.cpp', 'options.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-options', egrabber_options_test)

  clprotocol_test_backend = get_option('clprotocol-test-backend')
  if genicam_root != '' and clprotocol_test_backend != ''
    egrabber_clprotocol_control_test = executable(
      'spa-egrabber-clprotocol-control-test',
      ['test-clprotocol-control.cpp', 'clprotocol_control.cpp'],
      include_directories: egrabber_includes,
      dependencies: [dl_dep],
      cpp_args: egrabber_cpp_args,
    )
    test(
      'spa-egrabber-clprotocol-control',
      egrabber_clprotocol_control_test,
      args: [clprotocol_test_backend],
    )
  endif

  egrabber_factory_test = executable(
    'spa-egrabber-factory-test',
    'test-factory.cpp',
    dependencies: [spa_dep, dl_dep],
  )

  egrabber_device_test = executable(
    'spa-egrabber-device-test',
    'test-device.cpp',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-egrabber-device',
    egrabber_device_test,
    args: [egrabber_lib.full_path()],
    depends: egrabber_lib,
    is_parallel: false,
  )
  test(
    'spa-egrabber-factory',
    egrabber_factory_test,
    args: [egrabber_lib.full_path()],
    depends: egrabber_lib,
  )

  egrabber_capture_test = executable(
    'spa-egrabber-capture-test',
    'test-capture.cpp',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-egrabber-capture',
    egrabber_capture_test,
    args: [egrabber_lib.full_path()],
    depends: egrabber_lib,
    is_parallel: false,
    timeout: 10,
  )

  pipewire_dep = dependency('libpipewire-ao-0.3', required: false)
  if pipewire_dep.found()
    executable(
      'spa-egrabber-host-client',
      'test-host-client.c',
      dependencies: [pipewire_dep],
    )
  endif
endif

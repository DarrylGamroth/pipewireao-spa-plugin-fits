bgapi2_sources = [
  'plugin.c',
  'manager.c',
  'device.c',
  'source.c',
  'camera.cpp',
  'params.c',
]

bgapi2_dependencies = [spa_dep, bgapi2_dep]
bgapi2_install_rpath = get_option('bgapi2-prefix') / 'lib'
bgapi2_c_args = ['-Wno-strict-prototypes']
bgapi2_cpp_args = ['-Wno-pedantic']

bgapi2_lib = shared_library(
  'spa-bgapi2',
  bgapi2_sources,
  dependencies: bgapi2_dependencies,
  c_args: bgapi2_c_args,
  cpp_args: bgapi2_cpp_args,
  install: true,
  install_dir: spa_dep.get_variable(pkgconfig: 'plugindir') / 'bgapi2',
  install_rpath: bgapi2_install_rpath,
)

if get_option('tests').allowed()
  bgapi2_factory_test = executable(
    'spa-bgapi2-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-bgapi2-factory',
    bgapi2_factory_test,
    args: [bgapi2_lib.full_path()],
    depends: bgapi2_lib,
  )

  bgapi2_device_test = executable(
    'spa-bgapi2-device-test',
    'test-device.c',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-bgapi2-device-euresys',
    bgapi2_device_test,
    args: [bgapi2_lib.full_path(),
      '/opt/euresys/egrabber/lib/x86_64/gigelink.cti'],
    depends: bgapi2_lib,
    is_parallel: false,
    timeout: 10,
  )
  test(
    'spa-bgapi2-device-baumer',
    bgapi2_device_test,
    args: [bgapi2_lib.full_path(),
      get_option('bgapi2-prefix') / 'lib/libbgapi2_gige.cti'],
    depends: bgapi2_lib,
    is_parallel: false,
    timeout: 10,
  )

  bgapi2_capture_test = executable(
    'spa-bgapi2-capture-test',
    'test-capture.c',
    dependencies: [spa_dep, dl_dep],
  )
  test(
    'spa-bgapi2-capture-euresys',
    bgapi2_capture_test,
    args: [bgapi2_lib.full_path(),
      '/opt/euresys/egrabber/lib/x86_64/gigelink.cti'],
    depends: bgapi2_lib,
    is_parallel: false,
    timeout: 10,
  )
  test(
    'spa-bgapi2-capture-baumer',
    bgapi2_capture_test,
    args: [bgapi2_lib.full_path(),
      get_option('bgapi2-prefix') / 'lib/libbgapi2_gige.cti'],
    depends: bgapi2_lib,
    is_parallel: false,
    timeout: 10,
  )
  bgapi2_camera_test = executable(
    'spa-bgapi2-camera-test',
    ['test-camera.c', 'camera.cpp'],
    dependencies: bgapi2_dependencies,
    c_args: bgapi2_c_args,
    cpp_args: bgapi2_cpp_args,
    build_rpath: bgapi2_install_rpath,
  )
  test(
    'spa-bgapi2-camera-euresys',
    bgapi2_camera_test,
    args: ['/opt/euresys/egrabber/lib/x86_64/gigelink.cti'],
    is_parallel: false,
    timeout: 10,
  )
  test(
    'spa-bgapi2-camera-baumer',
    bgapi2_camera_test,
    args: [get_option('bgapi2-prefix') / 'lib/libbgapi2_gige.cti'],
    is_parallel: false,
    timeout: 10,
  )

  executable(
    'spa-bgapi2-completion-benchmark',
    ['benchmark-completion.c', 'camera.cpp'],
    dependencies: bgapi2_dependencies,
    c_args: bgapi2_c_args,
    cpp_args: bgapi2_cpp_args,
    build_rpath: bgapi2_install_rpath,
  )
endif

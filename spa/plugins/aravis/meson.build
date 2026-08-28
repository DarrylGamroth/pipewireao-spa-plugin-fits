aravis_lib = shared_library(
  'spa-aravis',
  ['plugin.c', 'source.c', 'camera.c', 'params.c'],
  dependencies: [spa_dep, aravis_dep],
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'aravis',
)

if get_option('tests').allowed()
  aravis_factory_test = executable(
    'spa-aravis-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-aravis-factory',
    aravis_factory_test,
    args: [aravis_lib.full_path()],
  )

  executable(
    'spa-aravis-capture-test',
    'test-capture.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )

  executable(
    'spa-aravis-camera-test',
    ['test-camera.c', 'camera.c'],
    dependencies: [spa_dep, aravis_dep],
  )

  executable(
    'spa-aravis-completion-benchmark',
    ['benchmark-completion.c', 'camera.c'],
    dependencies: [spa_dep, aravis_dep],
  )
endif

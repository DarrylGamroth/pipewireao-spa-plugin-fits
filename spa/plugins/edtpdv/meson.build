if edtpdv_found
  edtpdv_lib = shared_library(
    'spa-edtpdv',
    ['plugin.c', 'source.c', 'camera.c'],
    dependencies: [spa_dep, edtpdv_dep],
    c_args: ['-Wno-pedantic'],
    install: true,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'edtpdv',
    install_rpath: edtpdv_install_rpath,
    install_tag: 'edtpdv',
  )
endif

if get_option('tests').allowed()
  edtpdv_mock_lib = shared_library(
    'spa-edtpdv-mock',
    ['plugin.c', 'source.c', 'camera-mock.c'],
    dependencies: spa_dep,
    c_args: ['-Wno-pedantic'],
    install: false,
  )

  edtpdv_factory_test = executable(
    'spa-edtpdv-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-edtpdv-factory',
    edtpdv_factory_test,
    args: [edtpdv_mock_lib.full_path()],
    depends: edtpdv_mock_lib,
  )

  edtpdv_source_test = executable(
    'spa-edtpdv-source-test',
    'test-source.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-edtpdv-source',
    edtpdv_source_test,
    args: [edtpdv_mock_lib.full_path()],
    depends: edtpdv_mock_lib,
  )
endif

hamamatsu_common_sources = ['plugin.c', 'source.c', 'params.c']
hamamatsu_c_args = ['-Wno-pedantic'] + cc.get_supported_arguments(
  ['-fcf-protection=full'],
)
if hamamatsu_found
  hamamatsu_lib = shared_library(
    'spa-hamamatsu',
    hamamatsu_common_sources + ['camera.c', 'phoenix-queue.c'],
    dependencies: [spa_dep, threads_dep, m_dep, dl_dep, hamamatsu_dep],
    c_args: hamamatsu_c_args,
    build_rpath: hamamatsu_runtime_dir,
    install: true,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'hamamatsu',
    install_rpath: hamamatsu_library_dir,
    install_tag: 'hamamatsu',
  )
endif

if get_option('tests').allowed()
  hamamatsu_mock_lib = shared_library(
    'spa-hamamatsu-mock',
    hamamatsu_common_sources + ['camera-mock.c'],
    dependencies: [spa_dep],
    c_args: hamamatsu_c_args,
    install: false,
  )

  hamamatsu_factory_test = executable(
    'spa-hamamatsu-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: hamamatsu_c_args,
  )
  test(
    'spa-hamamatsu-factory',
    hamamatsu_factory_test,
    args: [hamamatsu_mock_lib.full_path()],
    depends: hamamatsu_mock_lib,
  )

  hamamatsu_source_test = executable(
    'spa-hamamatsu-source-test',
    'test-source.c',
    dependencies: [spa_dep, dl_dep],
    c_args: hamamatsu_c_args,
  )
  test(
    'spa-hamamatsu-source',
    hamamatsu_source_test,
    args: [hamamatsu_mock_lib.full_path()],
    depends: hamamatsu_mock_lib,
  )

  hamamatsu_phoenix_queue_test = executable(
    'spa-hamamatsu-phoenix-queue-test',
    ['test-phoenix-queue.c', 'phoenix-queue.c'],
    dependencies: [spa_dep, threads_dep, dl_dep],
    c_args: hamamatsu_c_args + ['-DPHOENIX_QUEUE_TESTING'],
  )
  test('spa-hamamatsu-phoenix-queue', hamamatsu_phoenix_queue_test)

endif

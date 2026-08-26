andor3_common_sources = ['plugin.c', 'source.c', 'params.c']
andor3_c_args = ['-Wno-pedantic']
andor3_cpp_args = ['-Wno-pedantic']

if andor3_found
  andor3_lib = shared_library(
    'spa-andor3',
    andor3_common_sources + ['camera.cpp'],
    dependencies: [spa_dep, threads_dep, andor3_dep],
    c_args: andor3_c_args,
    cpp_args: andor3_cpp_args,
    build_rpath: andor3_runtime_dir,
    install: true,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'andor3',
    install_rpath: andor3_library_dir,
  )
endif

if get_option('tests').allowed()
  andor3_mock_lib = shared_library(
    'spa-andor3-mock',
    andor3_common_sources + ['camera-mock.c'],
    dependencies: [spa_dep],
    c_args: andor3_c_args,
    install: false,
  )

  andor3_factory_test = executable(
    'spa-andor3-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: andor3_c_args,
  )
  test(
    'spa-andor3-factory',
    andor3_factory_test,
    args: [andor3_mock_lib.full_path()],
    depends: andor3_mock_lib,
  )

  andor3_source_test = executable(
    'spa-andor3-source-test',
    'test-source.c',
    dependencies: [spa_dep, dl_dep],
    c_args: andor3_c_args,
  )
  test(
    'spa-andor3-source',
    andor3_source_test,
    args: [andor3_mock_lib.full_path()],
    depends: andor3_mock_lib,
  )

  if andor3_found
    andor3_camera_test = executable(
      'spa-andor3-camera-test',
      ['test-camera.c', 'camera.cpp'],
      dependencies: [spa_dep, threads_dep, andor3_dep],
      c_args: andor3_c_args,
      cpp_args: andor3_cpp_args,
      build_rpath: andor3_runtime_dir,
    )
    andor3_test_env = environment()
    andor3_test_env.set('LD_LIBRARY_PATH', andor3_runtime_dir)
    test(
      'spa-andor3-camera',
      andor3_camera_test,
      env: andor3_test_env,
      is_parallel: false,
      timeout: 10,
    )
    test(
      'spa-andor3-factory-sdk',
      andor3_factory_test,
      args: [andor3_lib.full_path(), 'real'],
      depends: andor3_lib,
      env: andor3_test_env,
      is_parallel: false,
      timeout: 10,
    )
  endif
endif

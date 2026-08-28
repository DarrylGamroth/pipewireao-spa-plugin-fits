flisdk_sources = ['plugin.c', 'source.c', 'camera.c']
flisdk_mock_sources = ['plugin.c', 'source.c', 'camera-mock.c']
flisdk_dependencies = [spa_dep, threads_dep, genicam_feature_dep]
flisdk_includes = []
flisdk_c_args = ['-Wno-pedantic']
flisdk_cpp_args = ['-Wno-pedantic']

flisdk_genicam_root = get_option('genicam-root')
if flisdk_genicam_root != ''
  flisdk_genicam_c_include = flisdk_genicam_root / 'Reference Implementation' / 'library' / 'C' / 'include'
  flisdk_genicam_cpp_include = flisdk_genicam_root / 'Reference Implementation' / 'library' / 'CPP' / 'include'
  flisdk_genapi_runtime = flisdk_genicam_root / 'Reference Implementation' / 'bin' / 'Linux64_x64' / 'libGenApiC_v3.so'
  if not fs.exists(flisdk_genicam_c_include / 'GenApiC.h') or not fs.exists(flisdk_genicam_cpp_include / 'CLProtocol' / 'ISerial.h')
    error('genicam-root does not contain the GenICam Reference Implementation C and C++ headers')
  endif
  if not fs.exists(flisdk_genapi_runtime)
    error('genicam-root does not contain Reference Implementation/bin/Linux64_x64/libGenApiC_v3.so')
  endif
  flisdk_control_sources = [
    'control.cpp',
    '../egrabber/clprotocol_control.cpp',
    '../egrabber/feature.cpp',
  ]
  flisdk_sources += flisdk_control_sources
  flisdk_mock_sources += flisdk_control_sources
  flisdk_includes += include_directories(
    flisdk_genicam_c_include,
    flisdk_genicam_cpp_include,
  )
  flisdk_c_args += '-DHAVE_CLPROTOCOL_CONTROL=1'
  flisdk_cpp_args += [
    '-DHAVE_CLPROTOCOL_CONTROL=1',
    '-DDEFAULT_GENAPI_RUNTIME="' + flisdk_genapi_runtime + '"',
  ]
  flisdk_dependencies += dl_dep
endif

if flisdk_found
  flisdk_lib = shared_library(
    'spa-flisdk',
    flisdk_sources,
    include_directories: flisdk_includes,
    dependencies: flisdk_dependencies + [flisdk_dep],
    c_args: flisdk_c_args,
    cpp_args: flisdk_cpp_args,
    install: true,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'flisdk',
    install_rpath: flisdk_install_rpath,
  )
endif

if get_option('tests').allowed()
  flisdk_mock_lib = shared_library(
    'spa-flisdk-mock',
    flisdk_mock_sources,
    include_directories: flisdk_includes,
    dependencies: flisdk_dependencies,
    c_args: flisdk_c_args,
    cpp_args: flisdk_cpp_args,
    install: false,
  )

  flisdk_factory_test = executable(
    'spa-flisdk-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-flisdk-factory',
    flisdk_factory_test,
    args: [flisdk_mock_lib.full_path()],
    depends: flisdk_mock_lib,
  )

  flisdk_source_test = executable(
    'spa-flisdk-source-test',
    'test-source.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-flisdk-source',
    flisdk_source_test,
    args: [flisdk_mock_lib.full_path()],
    depends: flisdk_mock_lib,
  )

  flisdk_clprotocol_test_backend = get_option('clprotocol-test-backend')
  if flisdk_genicam_root != '' and flisdk_clprotocol_test_backend != ''
    test(
      'spa-flisdk-clprotocol-control',
      flisdk_source_test,
      args: [flisdk_mock_lib.full_path(), flisdk_clprotocol_test_backend],
      depends: flisdk_mock_lib,
    )
  endif
endif

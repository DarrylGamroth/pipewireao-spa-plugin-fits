hermes_common_sources = ['plugin.c', 'source.c']
hermes_c_args = ['-Wno-pedantic'] + cc.get_supported_arguments(
  ['-fcf-protection=full', '-fvisibility=hidden'],
)

if hermes_found
  hermes_source_lib = shared_library(
    'spa-hermes-source',
    hermes_common_sources + ['camera.c'],
    dependencies: [spa_dep, hermes_dep, dl_dep],
    include_directories: [plugin_include],
    c_args: hermes_c_args,
    build_rpath: hermes_library_dir,
    install: true,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'hermes',
    install_rpath: hermes_library_dir,
    install_tag: 'hermes',
  )
endif

if get_option('tests').allowed()
  hermes_mock_lib = shared_library(
    'spa-hermes-source-mock',
    hermes_common_sources + ['camera-mock.c'],
    dependencies: [spa_dep],
    include_directories: [plugin_include],
    c_args: hermes_c_args,
    install: false,
  )

  hermes_source_test = executable(
    'spa-hermes-source-test',
    'test-source.c',
    dependencies: [spa_dep, dl_dep],
    c_args: hermes_c_args,
  )
  test(
    'spa-hermes-source',
    hermes_source_test,
    args: [hermes_mock_lib.full_path()],
    depends: hermes_mock_lib,
  )
endif

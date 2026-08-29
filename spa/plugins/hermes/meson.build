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
  )
endif

hermes_cargo = find_program('cargo')
hermes_python = import('python').find_installation('python3')
hermes_cargo_profile = get_option('buildtype') in ['release', 'minsize'] ? 'release' : 'debug'
hermes_pkg_config_path = ':'.join(get_option('pkg_config_path'))
hermes_decoder_lib = custom_target(
  'spa-hermes-decoder',
  output: 'libspa-hermes-decoder.so',
  command: [
    hermes_python,
    files('../../../scripts/build-cargo.py'),
    '--cargo', hermes_cargo,
    '--manifest', meson.project_source_root() / 'Cargo.toml',
    '--target-dir', '@PRIVATE_DIR@' / 'cargo-target',
    '--output', '@OUTPUT@',
    '--profile', hermes_cargo_profile,
    '--spa-include', spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    '--pkg-config-path', hermes_pkg_config_path,
    '--package', 'hermes-spa-decoder',
  ],
  build_always_stale: true,
  build_by_default: hermes_found,
  install: hermes_found,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'hermes',
)

if get_option('tests').allowed()

  hermes_nm = find_program('nm')
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
  if hermes_found
    test(
      'spa-hermes-decoder-exports',
      hermes_python,
      args: [
        files('../../../scripts/check-exports.py'),
        '--nm', hermes_nm.full_path(),
        hermes_decoder_lib.full_path(),
      ],
      depends: hermes_decoder_lib,
    )
  endif
endif

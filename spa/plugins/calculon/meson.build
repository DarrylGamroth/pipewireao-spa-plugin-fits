cargo = find_program('cargo')
python = import('python').find_installation('python3')
cargo_profile = get_option('buildtype') in ['release', 'minsize'] ? 'release' : 'debug'
configured_pkg_config_path = run_command(
  python,
  '-c',
  'import os; print(os.environ.get("PKG_CONFIG_PATH", ""))',
  check: true,
).stdout().strip()

calculon_lib = custom_target(
  'spa-calculon',
  output: 'libspa-calculon.so',
  command: [
    python,
    files('../../../scripts/build-cargo.py'),
    '--cargo', cargo,
    '--manifest', meson.project_source_root() / 'Cargo.toml',
    '--target-dir', '@PRIVATE_DIR@' / 'cargo-target',
    '--output', '@OUTPUT@',
    '--profile', cargo_profile,
    '--spa-include', spa_pkg_dep.get_variable(pkgconfig: 'spa_includedir'),
    '--pkg-config-path', configured_pkg_config_path,
  ],
  build_always_stale: true,
  build_by_default: true,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'calculon',
)

if get_option('tests').allowed()
  nm = find_program('nm')
  calculon_test = executable(
    'spa-calculon-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  calculon_full_system_test = executable(
    'spa-calculon-full-system-test',
    'test-full-system.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-calculon-factory',
    calculon_test,
    args: [calculon_lib.full_path()],
    depends: calculon_lib,
  )
  test(
    'spa-calculon-exports',
    python,
    args: [
      files('../../../scripts/check-exports.py'),
      '--nm', nm.full_path(),
      calculon_lib.full_path(),
    ],
    depends: calculon_lib,
  )
  test(
    'spa-calculon-full-system',
    calculon_full_system_test,
    args: [calculon_lib.full_path(), alpao_mock_lib.full_path()],
    depends: [calculon_lib, alpao_mock_lib],
  )
  benchmark(
    'spa-calculon-full-system-latency',
    calculon_full_system_test,
    args: [
      calculon_lib.full_path(),
      alpao_mock_lib.full_path(),
      'mock',
      '100000',
    ],
    depends: [calculon_lib, alpao_mock_lib],
    timeout: 120,
  )
endif

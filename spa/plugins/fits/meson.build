fits_cube_sources = ['cube.c']

fits_lib = shared_library(
  'spa-fits',
  ['plugin.c', 'source.c'] + fits_cube_sources,
  dependencies: [spa_dep, cfitsio_dep],
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'fits',
  install_tag: 'fits',
)

if get_option('tests').allowed()
  fits_factory_test = executable(
    'spa-fits-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
  )
  test(
    'spa-fits-factory',
    fits_factory_test,
    args: [fits_lib.full_path()],
    depends: fits_lib,
  )

  fits_cube_test = executable(
    'spa-fits-cube-test',
    ['test-cube.c'] + fits_cube_sources,
    dependencies: [spa_dep, cfitsio_dep],
  )
  test('spa-fits-cube', fits_cube_test)

  fits_source_test = executable(
    'spa-fits-source-test',
    'test-source.c',
    dependencies: [spa_dep, pipewire_dep, cfitsio_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  fits_source_test_env = environment()
  fits_source_test_env.set(
    'PIPEWIREAO_SPA_PLUGIN_DIR',
    spa_pkg_dep.get_variable(pkgconfig: 'plugindir'),
  )
  test(
    'spa-fits-source',
    fits_source_test,
    args: [fits_lib.full_path()],
    depends: fits_lib,
    env: fits_source_test_env,
  )

  pipewire_daemon = find_program(
    pipewire_pkg_dep.get_variable(pkgconfig: 'confdatadir') / 'pipewire-ao',
    required: false,
  )
  if pipewire_daemon.found() and is_variable('discard_lib')
    python = import('python').find_installation('python3')
    fits_discard_live_test = executable(
      'spa-fits-discard-live-test',
      'test-discard-live.c',
      dependencies: [spa_dep, pipewire_dep, cfitsio_dep],
      include_directories: plugin_include,
    )
    fits_discard_live_env = environment()
    fits_discard_live_env.set(
      'PIPEWIREAO_SPA_PLUGIN_DIR',
      ':'.join([
        meson.project_build_root() / 'spa/plugins',
        spa_pkg_dep.get_variable(pkgconfig: 'plugindir'),
      ]),
    )
    fits_discard_live_env.set(
      'PIPEWIREAO_CONFIG_DIR',
      pipewire_pkg_dep.get_variable(pkgconfig: 'confdatadir'),
    )
    fits_discard_live_env.set(
      'PIPEWIREAO_MODULE_DIR',
      pipewire_pkg_dep.get_variable(pkgconfig: 'moduledir'),
    )
    test(
      'spa-fits-discard-live',
      python,
      args: [
        files('test-discard-live.py'),
        fits_discard_live_test.full_path(),
        pipewire_daemon.full_path(),
        files('test-discard-live.conf.in'),
      ],
      depends: [fits_discard_live_test, fits_lib, discard_lib],
      env: fits_discard_live_env,
      timeout: 20,
    )
  endif
endif

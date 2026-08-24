fits_cube_sources = ['cube.c']

fits_lib = shared_library(
  'spa-fits',
  ['plugin.c', 'source.c'] + fits_cube_sources,
  dependencies: [spa_dep, cfitsio_dep],
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'fits',
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
    dependencies: [spa_dep, cfitsio_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-fits-source',
    fits_source_test,
    args: [fits_lib.full_path()],
    depends: fits_lib,
  )

  fits_calculon_test = executable(
    'spa-fits-calculon-test',
    'test-calculon.c',
    dependencies: [spa_dep, cfitsio_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-fits-calculon-ingress',
    fits_calculon_test,
    args: [fits_lib.full_path(), calculon_lib.full_path()],
    depends: [fits_lib, calculon_lib],
  )
endif

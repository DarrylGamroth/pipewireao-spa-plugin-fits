fits_cube_sources = ['cube.c']

if get_option('tests').allowed()
  fits_cube_test = executable(
    'spa-fits-cube-test',
    ['test-cube.c'] + fits_cube_sources,
    dependencies: [spa_dep, cfitsio_dep],
  )
  test('spa-fits-cube', fits_cube_test)
endif

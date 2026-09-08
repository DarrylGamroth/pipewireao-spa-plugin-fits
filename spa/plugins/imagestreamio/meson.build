imagestreamio_lib = shared_library(
  'spa-imagestreamio',
  ['plugin.c', 'node.c', 'stream.c'],
  dependencies: [spa_dep, imagestreamio_dep],
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  gnu_symbol_visibility: 'hidden',
  install_rpath: imagestreamio_install_rpath,
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'imagestreamio',
  install_tag: 'imagestreamio',
)

if get_option('tests').allowed()
  imagestreamio_factory_test = executable(
    'spa-imagestreamio-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
  )
  test(
    'spa-imagestreamio-factory',
    imagestreamio_factory_test,
    args: [imagestreamio_lib.full_path()],
    depends: imagestreamio_lib,
  )

  imagestreamio_stream_test = executable(
    'spa-imagestreamio-stream-test',
    ['test-stream.c', 'stream.c'],
    dependencies: [spa_dep, imagestreamio_dep],
  )
  test('spa-imagestreamio-stream', imagestreamio_stream_test)

  imagestreamio_node_test = executable(
    'spa-imagestreamio-node-test',
    'test-node.c',
    dependencies: [spa_dep, dl_dep, imagestreamio_dep],
    include_directories: plugin_include,
  )
  test(
    'spa-imagestreamio-node',
    imagestreamio_node_test,
    args: [imagestreamio_lib.full_path()],
    depends: imagestreamio_lib,
  )
endif

project(
  'pipewireao-spa-plugins',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')

spa_dep = dependency('libspa-ao-0.2', version: '>= 0.2')
cc = meson.get_compiler('c')
dl_dep = cc.find_library('dl', required: false)
m_dep = cc.find_library('m')
plugin_include = include_directories('include')

install_headers(
  'include/pipewireao-plugins/alpao.h',
  subdir: 'pipewireao-plugins-0.1/pipewireao-plugins',
)

subdir('spa/plugins/alpao')

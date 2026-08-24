project(
  'pipewireao-spa-plugins',
  ['c', 'cpp'],
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=3',
  ],
)

fs = import('fs')

add_project_arguments('-D_GNU_SOURCE', language: 'c')

spa_dep = dependency('libspa-ao-0.2', version: '>= 0.2')
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
dl_dep = cc.find_library('dl', required: false)
m_dep = cc.find_library('m')
threads_dep = dependency('threads')
drm_dep = dependency('libdrm', required: false)
plugin_include = include_directories('include')

install_headers(
  'include/pipewireao-plugins/alpao.h',
  'include/pipewireao-plugins/calculon.h',
  subdir: 'pipewireao-plugins-0.1/pipewireao-plugins',
)

subdir('spa/plugins/alpao')
subdir('spa/plugins/calculon')

egrabber_found = false
if get_option('egrabber').allowed()
  egrabber_include = include_directories(get_option('egrabber-prefix') / 'include')
  egrabber_dep = declare_dependency(include_directories: egrabber_include)
  egrabber_found = cxx.has_header(
    'EGrabber.h',
    dependencies: egrabber_dep,
    required: get_option('egrabber'),
  )
endif
if egrabber_found
  subdir('spa/plugins/egrabber')
endif

bgapi2_found = false
if get_option('bgapi2').allowed()
  bgapi2_prefix = get_option('bgapi2-prefix')
  bgapi2_include = include_directories(bgapi2_prefix / 'include')
  bgapi2_library = cc.find_library(
    'bgapi2_genicam',
    dirs: [bgapi2_prefix / 'lib'],
    required: get_option('bgapi2'),
  )
  bgapi2_dep = declare_dependency(
    include_directories: bgapi2_include,
    dependencies: bgapi2_library,
  )
  bgapi2_found = bgapi2_library.found() and cc.has_header(
    'bgapi2_genicam/bgapi2_genicam.h',
    dependencies: bgapi2_dep,
    required: get_option('bgapi2'),
  )
endif
if bgapi2_found
  subdir('spa/plugins/bgapi2')
endif

summary('eGrabber plugin', egrabber_found, section: 'SPA plugins')
summary('BGAPI2 plugin', bgapi2_found, section: 'SPA plugins')

alpao_sources = [
  'backend.c',
  'plugin.c',
  'sink.c',
]
alpao_dependencies = [spa_dep, m_dep]
alpao_c_args = ['-Wno-pedantic']

alpao_fgn_lib = shared_library(
  'alpao-fgn',
  'fgn-command-normalization.c',
  dependencies: [spa_dep, m_dep],
  include_directories: plugin_include,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: get_option('libdir') / 'pipewire-ao/filter-graph',
  install_tag: 'alpao',
)

alpao_sdk = get_option('alpao-sdk')
alpao_sdk_root = get_option('alpao-sdk-root')
alpao_sdk_enabled = false
alpao_sdk_libdir = ''
alpao_capture_benchmark_enabled = false

if not alpao_sdk.disabled()
  if alpao_sdk_root != ''
    alpao_sdk_include = include_directories(alpao_sdk_root / 'Include')
    alpao_sdk_libdir = alpao_sdk_root / 'Linux' / 'Lib' / 'x64'
    alpao_sdk_library = cc.find_library(
      'asdk',
      dirs: alpao_sdk_libdir,
      required: alpao_sdk,
    )
    if alpao_sdk_library.found()
      alpao_dependencies += alpao_sdk_library
      alpao_sources += 'backend-asdk.c'
      alpao_c_args += '-DHAVE_ALPAO_ASDK'
      alpao_sdk_enabled = true
    endif
  else
    alpao_sdk_library = cc.find_library(
      'asdk',
      required: alpao_sdk,
    )
    alpao_sdk_header = cc.has_header(
      'asdkWrapper.h',
      required: alpao_sdk,
    )
    if alpao_sdk_library.found() and alpao_sdk_header
      alpao_dependencies += alpao_sdk_library
      alpao_sources += 'backend-asdk.c'
      alpao_c_args += '-DHAVE_ALPAO_ASDK'
      alpao_sdk_enabled = true
    endif
  endif
endif

if alpao_sdk_enabled and alpao_sdk_root != ''
  alpao_lib = shared_library(
    'spa-alpao',
    alpao_sources,
    dependencies: alpao_dependencies,
    include_directories: [plugin_include, alpao_sdk_include],
    c_args: alpao_c_args,
    gnu_symbol_visibility: 'hidden',
    build_by_default: alpao_sdk_enabled,
    install: alpao_sdk_enabled,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'alpao',
    install_tag: 'alpao',
  )
else
  alpao_lib = shared_library(
    'spa-alpao',
    alpao_sources,
    dependencies: alpao_dependencies,
    include_directories: plugin_include,
    c_args: alpao_c_args,
    gnu_symbol_visibility: 'hidden',
    build_by_default: alpao_sdk_enabled,
    install: alpao_sdk_enabled,
    install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'alpao',
    install_tag: 'alpao',
  )
endif

if get_option('tests').allowed()
  alpao_fgn_test = executable(
    'spa-alpao-fgn-test',
    'test-fgn-command-normalization.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-alpao-fgn-command-normalization',
    alpao_fgn_test,
    args: [alpao_fgn_lib.full_path()],
    depends: alpao_fgn_lib,
  )

  alpao_mock_lib = shared_library(
    'spa-alpao-mock',
    ['backend.c', 'plugin.c', 'sink.c'],
    dependencies: [spa_dep, m_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic', '-DHAVE_ALPAO_MOCK'],
    gnu_symbol_visibility: 'hidden',
    install: false,
  )
  alpao_test = executable(
    'spa-alpao-test',
    'test-sink.c',
    dependencies: [spa_dep, dl_dep],
    include_directories: plugin_include,
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-alpao-factory',
    alpao_test,
    args: [alpao_lib.full_path(), 'factory'],
    depends: alpao_lib,
  )
  test(
    'spa-alpao-mock',
    alpao_test,
    args: [alpao_mock_lib.full_path(), 'mock'],
  )

  binary_config_option = get_option('alpao-binary-config')
  if alpao_sdk_enabled and binary_config_option != ''
    python = import('python').find_installation('python3')
    binary_config = find_program(binary_config_option)
    simulator_binary = custom_target(
      'alpao-simulator-binary-config',
      output: 'SIM001',
      command: [
        python,
        binary_config,
        'new',
        '@OUTPUT@',
        '--nact', '8',
        '--nc', '8',
        '--nb-steps', '1',
      ],
      build_by_default: true,
    )
    simulator_ascii = custom_target(
      'alpao-simulator-ascii-config',
      output: 'SIM001.acfg',
      command: [
        python,
        '-c',
        'import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b"interface sim interfaceId 0 deVersion 7 ")',
        '@OUTPUT@',
      ],
      build_by_default: true,
    )
    simulator_environment = environment()
    simulator_environment.set('ACECFG', meson.current_build_dir())
    if alpao_sdk_libdir != ''
      simulator_environment.prepend('LD_LIBRARY_PATH', alpao_sdk_libdir)
    endif
    test(
      'spa-alpao-asdk-simulator',
      alpao_test,
      args: [alpao_lib.full_path(), 'asdk'],
      env: simulator_environment,
      depends: [simulator_binary, simulator_ascii],
    )

    capture_plugin_dir = get_option('alpao-capture-plugin-dir')
    if capture_plugin_dir != ''
      capture_interface = cc.find_library(
        'ait_capture',
        dirs: capture_plugin_dir,
        required: false,
      )
      if capture_interface.found()
        alpao_capture_benchmark_enabled = true
        threads_dep = dependency('threads')
        capture_benchmark = executable(
          'spa-alpao-capture-benchmark',
          'bench-capture.c',
          dependencies: [spa_dep, dl_dep, m_dep, threads_dep],
          include_directories: plugin_include,
          c_args: ['-Wno-pedantic'],
        )
        capture_binary = custom_target(
          'alpao-capture-binary-config',
          output: 'CAP468',
          command: [
            python,
            binary_config,
            'new',
            '@OUTPUT@',
            '--nact', '468',
            '--nc', '468',
            '--nb-steps', '1',
          ],
          build_by_default: true,
        )
        capture_ascii = custom_target(
          'alpao-capture-ascii-config',
          output: 'CAP468.acfg',
          command: [
            python,
            '-c',
            'import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b"interface capture interfaceId 0 startChannel 0 deVersion 7 daqFreq 0\\n")',
            '@OUTPUT@',
          ],
          build_by_default: true,
        )
        capture_environment = environment()
        capture_environment.set('ACECFG', meson.current_build_dir() / '')
        capture_environment.prepend('LD_LIBRARY_PATH', capture_plugin_dir)
        if alpao_sdk_libdir != ''
          capture_environment.prepend('LD_LIBRARY_PATH', alpao_sdk_libdir)
        endif
        test(
          'spa-alpao-asdk-capture',
          capture_benchmark,
          args: [
            '--warmup', '100',
            '--samples', '1000',
            alpao_lib.full_path(),
          ],
          env: capture_environment,
          depends: [capture_binary, capture_ascii],
          timeout: 30,
        )
        benchmark(
          'spa-alpao-asdk-capture-latency',
          capture_benchmark,
          args: [alpao_lib.full_path()],
          env: capture_environment,
          depends: [capture_binary, capture_ascii],
          timeout: 120,
        )
      else
        warning('alpao-capture-plugin-dir does not contain libait_capture.so')
      endif
    endif
  endif
endif

summary('ALPAO ASDK backend', alpao_sdk_enabled, section: 'SPA plugins')
summary(
  'ALPAO capture benchmark',
  alpao_capture_benchmark_enabled,
  section: 'SPA plugins',
)

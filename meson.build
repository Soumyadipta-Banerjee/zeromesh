project('zeromesh', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release'
  ]
)

cpp = meson.get_compiler('cpp')

threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required: false)
openssl_dep = dependency('openssl')
protobuf_dep = dependency('protobuf')
grpc_dep = dependency('grpc++')
sodium_dep = dependency('libsodium')

inc = include_directories('src', '.')

common_deps = [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep, sodium_dep]

core_sources = files(
  'src/cas/store.cpp',
  'src/cas/merkle.cpp',
  'src/discovery/peer_table.cpp',
  'src/discovery/mdns.cpp',
  'src/executor/runner.cpp',
  'src/executor/bwrap.cpp',
  'src/executor/landlock.cpp',
  'src/ui/tui.cpp',
)

zm_core = static_library('zm_core', core_sources,
  include_directories: inc,
  dependencies: common_deps,
)

zm_bin = executable('zm', 'src/main.cpp',
  include_directories: inc,
  link_with: zm_core,
  dependencies: common_deps,
  install: true,
)

test_bin = executable('unit_tests',
  files(
    'tests/test_main.cpp',
    'tests/test_cas.cpp',
    'tests/test_merkle.cpp',
    'tests/test_peer_table.cpp',
    'tests/test_mdns.cpp',
    'tests/test_bwrap.cpp',
    'tests/test_executor.cpp',
    'tests/test_tui.cpp',
  ),
  include_directories: inc,
  link_with: zm_core,
  dependencies: common_deps,
)

test('all_unit_tests', test_bin)

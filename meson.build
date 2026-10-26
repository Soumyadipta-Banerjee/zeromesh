project('zeromesh', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release'
  ]
)

# Compiler configuration
cpp = meson.get_compiler('cpp')

# Core system dependencies
threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required: false)

# Optional third-party dependencies with fallback
openssl_dep = dependency('openssl', required: false)
protobuf_dep = dependency('protobuf', required: false)
grpc_dep = dependency('grpc++', required: false)
fuse3_dep = dependency('fuse3', required: false)

# Include directories
inc = include_directories('include', 'src', '.')

# Header-only architecture library interface
# All subsystem contracts are defined under src/ and include/
zeromesh_headers = declare_dependency(
  include_directories: inc,
  dependencies: [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep, fuse3_dep]
)

# Core REAPI Gateway and Daemon sources (Option 1)
sources = files(
  'src/main.cpp',
  'src/cas/store.cpp',
  'src/cas/merkle.cpp',
  'src/discovery/peer_table.cpp',
  'src/dht/kademlia.cpp',
  'src/dht/routing_table.cpp',
  'src/dht/local_store.cpp',
  'src/dht/transport.cpp',
  'src/reapi/action_cache.cpp',
  'src/reapi/cas_service.cpp',
  'src/reapi/execution_service.cpp',
  'src/reapi/server.cpp',
  'src/scheduler/heuristics.cpp',
  'src/scheduler/scheduler.cpp',
  'src/executor/runner.cpp',
  'src/executor/bwrap.cpp',
  'src/executor/landlock.cpp',
  'src/verify/byzantine_spot_checker.cpp'
)

zm_bin = executable('zm',
  sources,
  include_directories: inc,
  dependencies: [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep],
  install: true
)

alias_target('zmd', zm_bin)

# Unit test suite
test_sources = files(
  'tests/test_main.cpp',
  'tests/test_cas.cpp',
  'tests/test_merkle.cpp',
  'tests/test_reapi.cpp',
  'tests/test_scheduler.cpp',
  'tests/test_peer_table.cpp',
  'tests/test_bwrap.cpp',
  'tests/test_verify.cpp',
  'tests/test_host_guard.cpp',
  'tests/test_executor.cpp',
  'src/cas/store.cpp',
  'src/cas/merkle.cpp',
  'src/discovery/peer_table.cpp',
  'src/dht/kademlia.cpp',
  'src/dht/routing_table.cpp',
  'src/dht/local_store.cpp',
  'src/dht/transport.cpp',
  'src/reapi/action_cache.cpp',
  'src/reapi/cas_service.cpp',
  'src/reapi/execution_service.cpp',
  'src/reapi/server.cpp',
  'src/scheduler/heuristics.cpp',
  'src/scheduler/scheduler.cpp',
  'src/executor/runner.cpp',
  'src/executor/bwrap.cpp',
  'src/executor/landlock.cpp',
  'src/verify/byzantine_spot_checker.cpp'
)

test_bin = executable('unit_tests',
  test_sources,
  include_directories: inc,
  dependencies: [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep]
)

test('all_unit_tests', test_bin)

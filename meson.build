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

# Include directories
inc = include_directories('src', '.')

# Header-only architecture library interface
# All subsystem contracts are defined under src/
zeromesh_headers = declare_dependency(
  include_directories: inc,
  dependencies: [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep]
)

# Note for team developers:
# Once you begin implementing .cpp files for your respective workstreams,
# uncomment the executable target below:
#
# sources = files(
#   'src/main.cpp',
#   'src/discovery/mdns.cpp',
#   'src/discovery/peer_table.cpp',
#   'src/reapi/server.cpp',
#   'src/reapi/execution_service.cpp',
#   'src/reapi/cas_service.cpp',
#   'src/reapi/action_cache.cpp',
#   'src/cas/store.cpp',
#   'src/cas/merkle.cpp',
#   'src/cas/p2p_syncer.cpp',
#   'src/scheduler/scheduler.cpp',
#   'src/scheduler/heuristics.cpp',
#   'src/executor/runner.cpp',
#   'src/executor/bwrap.cpp',
#   'src/executor/landlock.cpp',
#   'src/executor/host_guard.cpp',
#   'src/ui/tui.cpp'
# )
# zm_bin = executable('zm', sources, dependencies: zeromesh_headers, install: true)
# alias_target('zmd', zm_bin)

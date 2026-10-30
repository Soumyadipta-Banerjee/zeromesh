project('zeromesh', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release'
  ]
)

add_project_arguments('-DZM_INSTALL_PREFIX="' + get_option('prefix') + '"', language: 'cpp')

cpp = meson.get_compiler('cpp')

threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required: false)
openssl_dep = dependency('openssl')
protobuf_dep = dependency('protobuf')
grpc_dep = dependency('grpc++')
sodium_dep = dependency('libsodium')
fuse3_dep = dependency('fuse3')

protoc = find_program('protoc')
grpc_cpp_plugin = find_program('grpc_cpp_plugin')
python3 = find_program('python3')

subdir('proto/zeromesh/v1')

inc = include_directories('core', '.', 'proto')

zm_proto = static_library('zm_proto', zm_proto_cpp,
  include_directories: inc,
  dependencies: [protobuf_dep, grpc_dep],
)
zm_proto_dep = declare_dependency(
  sources: zm_proto_cpp,
  include_directories: inc,
  link_with: zm_proto,
  dependencies: [protobuf_dep, grpc_dep],
)

common_deps = [threads_dep, dl_dep, openssl_dep, protobuf_dep, grpc_dep, sodium_dep, fuse3_dep, zm_proto_dep]

core_sources = files(
  'core/store/store.cpp',
  'core/store/merkle.cpp',
  'core/store/ignore.cpp',
  'core/net/peer_table.cpp',
  'core/net/discovery.cpp',
  'core/net/telemetry.cpp',
  'core/node/state.cpp',
  'core/exec/bwrap.cpp',
  'core/exec/landlock.cpp',
  'core/exec/priority.cpp',
  'core/exec/toolchain_digest.cpp',
  'core/exec/sandbox_runner.cpp',
  'core/plugin/loader.cpp',
  'core/plugin/client_api_handle.cpp',
  'core/node/task_key.cpp',
  'core/net/team.cpp',
  'core/net/identity.cpp',
  'core/net/pin_store.cpp',
  'core/net/tls.cpp',
  'core/node/admission.cpp',
  'core/node/node_service.cpp',
  'core/node/node_client.cpp',
  'core/node/donor_policy.cpp',
  'core/node/comfort_governor.cpp',
  'core/node/departure.cpp',
  'core/node/blob_transfer.cpp',
  'core/node/mirror_manifest.cpp',
  'core/node/requester_blob_source.cpp',
  'core/sched/rank.cpp',
  'core/sched/hazard.cpp',
  'core/sched/bandwidth.cpp',
  'core/sched/placer.cpp',
  'plugins/env-host/env_host.cpp',
  'plugins/env-mirror/deny_list.cpp',
  'plugins/env-mirror/mirror_cache.cpp',
  'plugins/env-mirror/env_profile.cpp',
  'plugins/env-mirror/mirror_reader.cpp',
  'plugins/env-mirror/mirror_prefetch.cpp',
  'plugins/env-mirror/mirror_fuse.cpp',
  'plugins/env-mirror/env_mirror.cpp',
  'core/node/action_cache.cpp',
  'core/node/local_executor.cpp',
  'core/node/job_manager.cpp',
  'core/node/client_service.cpp',
  'core/node/daemon.cpp',
)

zm_core = static_library('zm_core', core_sources,
  include_directories: inc,
  dependencies: common_deps,
)

# CLI pieces live in their own library so unit tests can reach the TUI renderer.
zm_cli = static_library('zm_cli',
  files('cli/client.cpp', 'cli/run_cmd.cpp', 'cli/status_cmd.cpp', 'cli/team_cmd.cpp', 'cli/tui.cpp'),
  include_directories: inc,
  dependencies: common_deps,
)

zm_bin = executable('zm', 'cli/main.cpp',
  include_directories: inc,
  link_with: [zm_cli, zm_core],
  dependencies: common_deps,
  install: true,
)

zmd_bin = executable('zmd', 'zmd/main.cpp',
  include_directories: inc,
  link_with: zm_core,
  dependencies: common_deps,
  # dlopen()ed plugins (frontend-reapi) resolve zeromesh::v1 protobuf symbols from the daemon.
  export_dynamic: true,
  install: true,
)

install_data('packaging/systemd/zmd.service',
  install_dir: get_option('prefix') / 'lib/systemd/user',
)


test_bin = executable('unit_tests',
  files(
    'tests/unit/test_main.cpp',
    'tests/unit/test_cas.cpp',
    'tests/unit/test_merkle.cpp',
    'tests/unit/test_peer_table.cpp',
    'tests/unit/test_discovery.cpp',
    'tests/unit/test_telemetry.cpp',
    'tests/unit/test_state.cpp',
    'tests/unit/test_bwrap.cpp',
    'tests/unit/test_sandbox_runner.cpp',
    'tests/unit/test_tui.cpp',
    'tests/unit/test_proto.cpp',
    'tests/unit/test_plugin.cpp',
    'tests/unit/test_task_key.cpp',
    'tests/unit/test_toolchain_digest.cpp',
    'tests/unit/test_job_manager.cpp',
    'tests/unit/test_ignore.cpp',
    'tests/unit/test_env_host.cpp',
    'tests/unit/test_team.cpp',
    'tests/unit/test_admission.cpp',
    'tests/unit/test_donor_policy.cpp',
    'tests/unit/test_node_api.cpp',
    'tests/unit/test_rank.cpp',
    'tests/unit/test_placer.cpp',
    'tests/unit/test_byte_counters.cpp',
    'tests/unit/test_comfort.cpp',
    'tests/unit/test_hedging.cpp',
    'tests/unit/test_seeding.cpp',
    'tests/unit/test_hazard.cpp',
    'tests/unit/test_departure.cpp',
    'tests/unit/test_units.cpp',
    'tests/unit/test_env_mirror.cpp',
    'tests/unit/test_env_profile.cpp',
    'tests/unit/test_bandwidth.cpp',
    'tests/unit/test_reapi_heuristics.cpp',
    'plugins/frontend-reapi/heuristics.cpp',
  ),
  include_directories: inc,
  link_with: [zm_cli, zm_core],
  dependencies: common_deps,
)

fake_env = shared_module('zm_fake_env', 'tests/plugins/fake_env.cpp',
  include_directories: inc, name_prefix: 'lib')
fake_env_badabi = shared_module('zm_fake_env_badabi', 'tests/plugins/fake_env.cpp',
  include_directories: inc, name_prefix: 'lib', cpp_args: ['-DFAKE_ABI=99'])

env_host_module = shared_module('zm_env_host',
  files('plugins/env-host/env_host.cpp', 'plugins/env-host/entry.cpp'),
  include_directories: inc, name_prefix: 'lib')

# Optional loadable .so (daemon also registers mirror statically). Avoid linking
# static abseil from common_deps into a shared object (needs -fPIC).
env_mirror_module = shared_module('zm_env_mirror',
  files('plugins/env-mirror/deny_list.cpp',
        'plugins/env-mirror/mirror_cache.cpp',
        'plugins/env-mirror/env_profile.cpp',
        'plugins/env-mirror/mirror_reader.cpp',
        'plugins/env-mirror/mirror_prefetch.cpp',
        'plugins/env-mirror/mirror_fuse.cpp',
        'plugins/env-mirror/env_mirror.cpp',
        'plugins/env-mirror/entry.cpp'),
  include_directories: inc,
  link_with: zm_core,
  dependencies: [threads_dep, fuse3_dep],
  name_prefix: 'lib',
  build_by_default: false)

# REAPI v2 proto + frontend-reapi plugin (disabled unless ZM_ENABLE_REAPI=1).
protobuf_include = run_command(python3, '-c',
  'import subprocess; print(subprocess.check_output(["pkg-config","--variable=includedir","protobuf"], text=True).strip() or "/usr/include")',
  check: false)
protobuf_inc_path = protobuf_include.stdout().strip()
if protobuf_inc_path == ''
  protobuf_inc_path = '/usr/include'
endif

reapi_proto_cpp = custom_target('proto_cpp_reapi',
  input: 'plugins/frontend-reapi/proto/remote_execution.proto',
  output: ['remote_execution.pb.cc', 'remote_execution.pb.h', 'remote_execution.grpc.pb.cc', 'remote_execution.grpc.pb.h'],
  command: [protoc, '-I', meson.project_source_root() / 'plugins/frontend-reapi/proto',
            '-I', protobuf_inc_path, '-I', '/usr/include',
            '--cpp_out=' + meson.project_build_root(),
            '--grpc_out=' + meson.project_build_root(),
            '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
            'remote_execution.proto'],
)

bytestream_proto_cpp = custom_target('proto_cpp_bytestream',
  input: 'plugins/frontend-reapi/proto/bytestream.proto',
  output: ['bytestream.pb.cc', 'bytestream.pb.h', 'bytestream.grpc.pb.cc', 'bytestream.grpc.pb.h'],
  command: [protoc, '-I', meson.project_source_root() / 'plugins/frontend-reapi/proto',
            '-I', protobuf_inc_path, '-I', '/usr/include',
            '--cpp_out=' + meson.project_build_root(),
            '--grpc_out=' + meson.project_build_root(),
            '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
            'bytestream.proto'],
)

# Shared module must link shared gRPC/protobuf — static absl from -dev is not PIC.
grpc_shared = cpp.find_library('grpc++', static: false)
grpc_c_shared = cpp.find_library('grpc', static: false)
protobuf_shared = cpp.find_library('protobuf', static: false)
frontend_hdr_deps = [
  protobuf_dep.partial_dependency(compile_args: true, includes: true),
  grpc_dep.partial_dependency(compile_args: true, includes: true),
]
frontend_reapi_module = shared_module('zm_frontend_reapi',
  sources: files('plugins/frontend-reapi/frontend_reapi.cpp',
                 'plugins/frontend-reapi/entry.cpp',
                 'plugins/frontend-reapi/reapi_server.cpp',
                 'plugins/frontend-reapi/heuristics.cpp') + reapi_proto_cpp + bytestream_proto_cpp,
  include_directories: inc, name_prefix: 'lib',
  dependencies: [grpc_shared, grpc_c_shared, protobuf_shared] + frontend_hdr_deps)

test('all_unit_tests', test_bin,
  depends: [fake_env, fake_env_badabi, env_host_module],
  env: {'ZM_TEST_PLUGIN_DIR': meson.current_build_dir()},
  # M4 placer retry tests sleep through real donor loss/drain (~20s together).
  timeout: 120,
)

# End-to-end tests against real zmd/zm binaries (plain unittest + grpcio).
test('integration_local', python3,
  args: [files('tests/integration/test_local_daemon.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 120,
  is_parallel: false,
)

test('integration_remote', python3,
  args: [files('tests/integration/test_remote_exec.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 180,
  is_parallel: false,
)

test('integration_fanout', python3,
  args: [files('tests/integration/test_fanout.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 240,
  is_parallel: false,
)

test('sdk_python', python3,
  args: ['-m', 'pytest', '-v', meson.project_source_root() / 'sdk/python/tests'],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.project_source_root() / 'sdk/python' + ':' + meson.project_source_root() / 'tests/integration',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 120,
  is_parallel: false,
)

test('integration_mirror', python3,
  args: [files('tests/integration/test_mirror.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 180,
  is_parallel: false,
)

test('integration_reapi', python3,
  args: [files('tests/integration/test_reapi.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin, frontend_reapi_module, env_host_module] + zm_proto_py,
  timeout: 180,
  is_parallel: false,
)

test('app_zm_render', python3,
  args: ['-m', 'pytest', '-v', meson.project_source_root() / 'apps/zm-render/tests'],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.project_source_root() / 'apps/zm-render' + ':' + meson.project_source_root() / 'sdk/python' + ':' + meson.project_source_root() / 'tests/integration',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 180,
  is_parallel: false,
)

test('app_zm_cfd', python3,
  args: [files('apps/zm-cfd/tests/test_cases.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.project_source_root() / 'apps/zm-cfd' + ':' + meson.project_source_root() / 'sdk/python',
  },
  depends: [zmd_bin, zm_bin] + zm_proto_py,
  timeout: 120,
  is_parallel: false,
)

test('integration_cfd', python3,
  args: [files('tests/integration/test_cfd.py')],
  env: {
    'ZM_BUILD_DIR': meson.current_build_dir(),
    'PYTHONPATH': meson.project_source_root() / 'apps/zm-cfd' + ':' + meson.project_source_root() / 'sdk/python' + ':' + meson.project_source_root() / 'tests/integration' + ':' + meson.current_build_dir() / 'proto',
  },
  depends: [zmd_bin, zm_bin, env_host_module] + zm_proto_py,
  timeout: 180,
  is_parallel: false,
)

test('verdict_analysis', python3,
  args: ['-m', 'pytest', '-q', meson.project_source_root() / 'scripts/verdict/tests'],
  timeout: 300,
  is_parallel: false,
)

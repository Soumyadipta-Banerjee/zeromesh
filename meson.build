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
  'core/exec/sandbox_runner.cpp',
  'core/plugin/loader.cpp',
  'core/node/task_key.cpp',
  'core/net/team.cpp',
  'core/net/identity.cpp',
  'core/net/pin_store.cpp',
  'core/net/tls.cpp',
  'core/node/admission.cpp',
  'core/node/node_service.cpp',
  'core/node/node_client.cpp',
  'core/node/donor_policy.cpp',
  'core/node/blob_transfer.cpp',
  'core/sched/rank.cpp',
  'core/sched/placer.cpp',
  'plugins/env-host/env_host.cpp',
  'plugins/env-mirror/deny_list.cpp',
  'plugins/env-mirror/mirror_cache.cpp',
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
  install: true,
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
    'tests/unit/test_job_manager.cpp',
    'tests/unit/test_ignore.cpp',
    'tests/unit/test_env_host.cpp',
    'tests/unit/test_team.cpp',
    'tests/unit/test_admission.cpp',
    'tests/unit/test_donor_policy.cpp',
    'tests/unit/test_node_api.cpp',
    'tests/unit/test_rank.cpp',
    'tests/unit/test_placer.cpp',
    'tests/unit/test_env_mirror.cpp',
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

env_mirror_module = shared_module('zm_env_mirror',
  files('plugins/env-mirror/deny_list.cpp',
        'plugins/env-mirror/mirror_cache.cpp',
        'plugins/env-mirror/mirror_fuse.cpp',
        'plugins/env-mirror/env_mirror.cpp',
        'plugins/env-mirror/entry.cpp'),
  include_directories: inc,
  dependencies: common_deps,
  name_prefix: 'lib')

test('all_unit_tests', test_bin,
  depends: [fake_env, fake_env_badabi, env_host_module, env_mirror_module],
  env: {'ZM_TEST_PLUGIN_DIR': meson.current_build_dir()},
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
  depends: [zmd_bin, zm_bin, env_mirror_module] + zm_proto_py,
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


project('zeromesh', 'cpp',
  version: '0.1.0',
  license: 'Apache-2.0',
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

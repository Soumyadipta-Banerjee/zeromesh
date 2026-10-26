# Generates C++ (protobuf + gRPC) and Python stubs for the zeromesh.v1 API into
# <builddir>/proto/zeromesh/v1/, so includes read "zeromesh/v1/<name>.pb.h" and
# Python imports read "from zeromesh.v1 import <name>_pb2".
proto_root_src = meson.project_source_root() / 'proto'
proto_root_out = meson.project_build_root() / 'proto'

zm_proto_cpp = []
zm_proto_py = []
foreach name : ['common', 'client', 'node']
  zm_proto_cpp += custom_target('proto_cpp_' + name,
    input: name + '.proto',
    output: [name + '.pb.cc', name + '.pb.h', name + '.grpc.pb.cc', name + '.grpc.pb.h'],
    command: [protoc, '-I', proto_root_src,
              '--cpp_out=' + proto_root_out,
              '--grpc_out=' + proto_root_out,
              '--plugin=protoc-gen-grpc=' + grpc_cpp_plugin.full_path(),
              'zeromesh/v1/' + name + '.proto'],
  )
  zm_proto_py += custom_target('proto_py_' + name,
    input: name + '.proto',
    output: [name + '_pb2.py', name + '_pb2_grpc.py'],
    command: [python3, '-m', 'grpc_tools.protoc', '-I', proto_root_src,
              '--python_out=' + proto_root_out,
              '--grpc_python_out=' + proto_root_out,
              'zeromesh/v1/' + name + '.proto'],
    build_by_default: true,
  )
endforeach

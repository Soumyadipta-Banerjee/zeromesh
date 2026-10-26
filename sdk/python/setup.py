import os
import subprocess
import sys
from setuptools import setup, find_packages
from setuptools.command.build_py import build_py
from setuptools.command.develop import develop


def generate_protos():
    """Generates Python stubs from proto/zeromesh/v1/ into zeromesh/v1/."""
    sdk_dir = os.path.dirname(os.path.abspath(__file__))
    proto_root = os.path.abspath(os.path.join(sdk_dir, "..", "..", "proto"))
    out_dir = os.path.join(sdk_dir, "zeromesh", "v1")

    if not os.path.isdir(proto_root):
        return  # In a pre-built wheel or standalone install

    os.makedirs(out_dir, exist_ok=True)
    init_file = os.path.join(out_dir, "__init__.py")
    if not os.path.exists(init_file):
        with open(init_file, "w") as f:
            f.write('"""Generated gRPC and Protobuf bindings for zeromesh.v1."""\n')

    proto_files = ["common.proto", "client.proto", "node.proto"]
    for pf in proto_files:
        proto_path = os.path.join(proto_root, "zeromesh", "v1", pf)
        if not os.path.exists(proto_path):
            continue
        cmd = [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            "-I",
            proto_root,
            f"--python_out={sdk_dir}",
            f"--grpc_python_out={sdk_dir}",
            f"zeromesh/v1/{pf}",
        ]
        try:
            subprocess.check_call(cmd, cwd=sdk_dir)
        except Exception as e:
            print(f"Warning: could not run protoc on {pf}: {e}", file=sys.stderr)


class CustomBuildPy(build_py):
    def run(self):
        generate_protos()
        super().run()


class CustomDevelop(develop):
    def run(self):
        generate_protos()
        super().run()


# Runtime deps are declared in pyproject.toml ([project].dependencies).
setup(
    name="zeromesh",
    version="0.1.0",
    description="ZeroMesh Python SDK for zero-setup compute mesh",
    author="ZeroMesh Team",
    packages=find_packages(),
    python_requires=">=3.9",
    cmdclass={
        "build_py": CustomBuildPy,
        "develop": CustomDevelop,
    },
)

#!/bin/bash
# Compila Pulse 4.4.0 y pulse_probe para QNX 8.0 x86_64.
#
# Requisitos: el entorno del SDP cargado y un build de Pulse para el host en ~/pulse/build, que
# aporta Eigen y los datos. Con 4 GB de RAM usa -j2. Tarda unos 40 min la primera vez.
#
# Se usa protobuf 21.12 porque es la última versión sin dependencia de absl y tiene port en
# qnx-ports. No se usa la receta de make del port: declara zlib y eso pone usr/include antes que
# los headers de libc++, lo que rompe la compilación.
set -euo pipefail

: "${QNX_TARGET:?Falta el entorno: source ~/qnx800/qnxsdp-env.sh}"

JOBS=2
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN="$REPO/port/qnx-x86_64.cmake"
PULSE="$HOME/pulse"
QNX="$PULSE/qnx"
STAGING="$QNX/staging"
EIGEN_DIR="$PULSE/build/External/Eigen3/install/share/eigen3/cmake"

mkdir -p "$QNX"
cd "$QNX"

[ -d protobuf ] || git clone --depth 1 --recurse-submodules --shallow-submodules \
    -b qnx-v21.12 https://github.com/qnx-ports/protobuf.git protobuf

# protoc para el host: genera los bindings C++ con la misma versión que la biblioteca del target.
cmake -S protobuf -B host-protoc -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX="$QNX/host-install" \
    -DCMAKE_CXX_FLAGS="-include cstdint"
cmake --build host-protoc --target install -j"$JOBS"

# libprotobuf estática para QNX.
cmake -S protobuf -B qnx-protobuf -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_STANDARD=17 -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_WITH_ZLIB=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$STAGING"
cmake --build qnx-protobuf --target install -j"$JOBS"

# Pulse para QNX, sin superbuild ni APIs de Java y Python.
cmake -S "$PULSE/engine" -B qnx-pulse -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DPulse_SUPERBUILD=OFF -DPulse_JAVA_API=OFF -DPulse_PYTHON_API=OFF -DPulse_GEN_DATA=OFF \
    -DEigen3_DIR="$EIGEN_DIR" -Dprotobuf_DIR="$STAGING/lib/cmake/protobuf" \
    -DProtobuf_PROTOC_EXECUTABLE="$QNX/host-install/bin/protoc" -Dprotobuf_SRC="$QNX/protobuf" \
    -DCMAKE_INSTALL_PREFIX="$STAGING"
cmake --build qnx-pulse --target Pulse -j"$JOBS"

# pulse_probe para QNX.
mkdir -p "$REPO/build"
q++ -Vgcc_ntox86_64_cxx -std=c++20 -O2 -Wall -Wextra -D_QNX_SOURCE \
    -I"$PULSE/engine/src/cpp" -I"$QNX/qnx-pulse/src/cpp" -I"$STAGING/include" \
    "$REPO/tools/pulse_probe/pulse_probe.cpp" \
    "$QNX/qnx-pulse/src/cpp/libPulse.a" "$STAGING/lib/libprotobuf.a" \
    -o "$REPO/build/pulse_probe_qnx"
ntox86_64-strip "$REPO/build/pulse_probe_qnx"
ls -la "$REPO/build/pulse_probe_qnx"

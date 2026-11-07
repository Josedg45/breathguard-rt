# Toolchain de CMake para compilar C++ hacia QNX 8.0 x86_64. Requiere el entorno del SDP.
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER qcc)
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_C_COMPILER_TARGET gcc_ntox86_64)
# La variante _cxx usa libc++, la biblioteca estándar de C++ de QNX 8.0.
set(CMAKE_CXX_COMPILER_TARGET gcc_ntox86_64_cxx)

set(CMAKE_C_FLAGS_INIT "-D_QNX_SOURCE")
set(CMAKE_CXX_FLAGS_INIT "-D_QNX_SOURCE")

# Solo buscar bibliotecas y headers del target; los programas se buscan en el host.
set(CMAKE_FIND_ROOT_PATH "$ENV{QNX_TARGET}" "$ENV{QNX_TARGET}/x86_64" "$ENV{HOME}/pulse/qnx/staging")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Programa de sondeo de Pulse para el host.
#   cmake -S tools/pulse_probe -B ~/pulse/probe-build -DCMAKE_PREFIX_PATH=$HOME/pulse/build/install
cmake_minimum_required(VERSION 3.31)
project(pulse_probe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Pulse REQUIRED)

add_executable(pulse_probe pulse_probe.cpp)
target_compile_options(pulse_probe PRIVATE -Wall -Wextra)
target_link_libraries(pulse_probe PRIVATE Pulse::Pulse)

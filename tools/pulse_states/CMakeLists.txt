# Catálogo de pacientes virtuales (estados de Pulse) para el host.
#   cmake -S tools/pulse_states -B ~/pulse/states-build -DCMAKE_PREFIX_PATH=$HOME/pulse/build/install
cmake_minimum_required(VERSION 3.31)
project(pulse_states CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Pulse REQUIRED)

add_executable(pulse_states pulse_states.cpp)
target_compile_options(pulse_states PRIVATE -Wall -Wextra)
target_link_libraries(pulse_states PRIVATE Pulse::Pulse)

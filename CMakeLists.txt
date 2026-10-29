cmake_minimum_required(VERSION 3.20)
project(p2p-messenger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

add_executable(p2pd src/main.cpp src/protocol/packet.cpp)

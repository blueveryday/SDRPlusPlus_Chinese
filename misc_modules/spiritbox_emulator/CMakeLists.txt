cmake_minimum_required(VERSION 3.13)
project(spiritbox_emulator)

file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(spiritbox_emulator PRIVATE "src/")

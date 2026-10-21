cmake_minimum_required(VERSION 3.13)
project(kiwisdr_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(kiwisdr_source PRIVATE "src/")

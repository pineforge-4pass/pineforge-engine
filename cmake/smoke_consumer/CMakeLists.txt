cmake_minimum_required(VERSION 3.16)
project(pineforge_smoke_consumer LANGUAGES C CXX)

find_package(PineForge 0.1 REQUIRED)

add_executable(smoke_version main.c)
target_link_libraries(smoke_version PRIVATE PineForge::pineforge)

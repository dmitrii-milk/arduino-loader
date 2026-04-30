cmake_minimum_required(VERSION 3.20)
project(loader)

set(CMAKE_CXX_STANDARD 17)

add_executable(loader main.cpp)

target_link_libraries(loader "-framework IOKit" "-framework CoreFoundation")
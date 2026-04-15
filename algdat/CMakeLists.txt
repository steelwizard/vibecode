cmake_minimum_required(VERSION 3.16)
project(algdat_linked_list LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(linked_list linked_list.cpp)
target_include_directories(linked_list PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(linked_list_test linked_list_test.cpp)
target_link_libraries(linked_list_test PRIVATE linked_list)

enable_testing()
add_test(NAME linked_list_test COMMAND linked_list_test)

cmake_minimum_required(VERSION 3.16)
project(wingrep C)

add_executable(wingrep main.c)

if(MSVC)
  target_compile_options(wingrep PRIVATE /W4)
else()
  target_compile_options(wingrep PRIVATE -Wall -Wextra -Wpedantic)
endif()

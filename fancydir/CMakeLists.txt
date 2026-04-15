cmake_minimum_required(VERSION 3.16)
project(fancydir LANGUAGES CXX)

add_executable(fancydir main.cpp)
target_compile_features(fancydir PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(fancydir PRIVATE /W4)
else()
  target_compile_options(fancydir PRIVATE -Wall -Wextra -Wpedantic)
endif()

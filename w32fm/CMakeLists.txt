cmake_minimum_required(VERSION 3.20)
project(w32fm LANGUAGES CXX)

add_executable(w32fm
    w32fm_main.cpp
    w32fm_model.cpp
    w32fm_fs.cpp
    w32fm_ui.cpp
    w32fm_viewer.cpp
)
target_compile_features(w32fm PRIVATE cxx_std_17)
target_link_libraries(w32fm PRIVATE shell32)
target_compile_definitions(w32fm PRIVATE NOMINMAX UNICODE _UNICODE)
set_target_properties(w32fm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
)

cmake_minimum_required(VERSION 3.20)
project(PlatformerDX LANGUAGES CXX)

add_executable(PlatformerDX WIN32 src/main.cpp)
target_compile_features(PlatformerDX PRIVATE cxx_std_17)
target_compile_definitions(PlatformerDX PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(PlatformerDX PRIVATE d2d1 dwrite)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT PlatformerDX)
set_target_properties(PlatformerDX PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

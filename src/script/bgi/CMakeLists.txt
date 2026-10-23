cmake_minimum_required(VERSION 3.16)

project(DangoBGIHookHost LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BGI Hook组件仅支持64位目标")
endif()

add_executable(BGIHost64 ../livemaker/main.cpp)
add_executable(BGIInjector64 ../livemaker/injector.cpp)
target_compile_features(BGIHost64 PRIVATE cxx_std_17)
target_compile_features(BGIInjector64 PRIVATE cxx_std_17)
target_compile_definitions(BGIHost64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_BGI_ONLY=1)
target_compile_definitions(BGIInjector64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(BGIHost64 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(BGIInjector64 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET BGIHost64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET BGIInjector64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(BGIHost64 PROPERTIES
    OUTPUT_NAME "BGIHost64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(BGIInjector64 PROPERTIES
    OUTPUT_NAME "BGIInjector64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

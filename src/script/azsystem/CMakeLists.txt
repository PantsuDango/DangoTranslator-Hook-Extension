cmake_minimum_required(VERSION 3.16)

project(AZSystemHookHost LANGUAGES CXX)

add_executable(AZSystemHost32 ../livemaker/main.cpp)
add_executable(AZSystemInjector32 ../livemaker/injector.cpp)
target_compile_features(AZSystemHost32 PRIVATE cxx_std_17)
target_compile_features(AZSystemInjector32 PRIVATE cxx_std_17)
target_compile_definitions(AZSystemHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_AZSYSTEM_ONLY=1)
target_compile_definitions(AZSystemInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AZSystem Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(AZSystemHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(AZSystemInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET AZSystemHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET AZSystemInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(AZSystemHost32 PROPERTIES
    OUTPUT_NAME "AZSystemHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(AZSystemInjector32 PROPERTIES
    OUTPUT_NAME "AZSystemInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

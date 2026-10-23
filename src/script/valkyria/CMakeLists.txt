cmake_minimum_required(VERSION 3.16)

project(DangoValkyriaHookHost LANGUAGES CXX)

add_executable(ValkyriaHost32 ../livemaker/main.cpp)
add_executable(ValkyriaInjector32 ../livemaker/injector.cpp)
target_compile_features(ValkyriaHost32 PRIVATE cxx_std_17)
target_compile_features(ValkyriaInjector32 PRIVATE cxx_std_17)
target_compile_definitions(ValkyriaHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_VALKYRIA_ONLY=1)
target_compile_definitions(ValkyriaInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Valkyria Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(ValkyriaHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(ValkyriaInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET ValkyriaHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET ValkyriaInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(ValkyriaHost32 PROPERTIES OUTPUT_NAME "ValkyriaHost32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(ValkyriaInjector32 PROPERTIES OUTPUT_NAME "ValkyriaInjector32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")


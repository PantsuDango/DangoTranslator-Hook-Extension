cmake_minimum_required(VERSION 3.16)

project(DangoQLIEHookHost LANGUAGES CXX)

add_executable(QLIEHost32 ../livemaker/main.cpp)
add_executable(QLIEInjector32 ../livemaker/injector.cpp)
target_compile_features(QLIEHost32 PRIVATE cxx_std_17)
target_compile_features(QLIEInjector32 PRIVATE cxx_std_17)
target_compile_definitions(QLIEHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_QLIE_ONLY=1)
target_compile_definitions(QLIEInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "QLIE Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(QLIEHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(QLIEInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET QLIEHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET QLIEInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(QLIEHost32 PROPERTIES OUTPUT_NAME "QLIEHost32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(QLIEInjector32 PROPERTIES OUTPUT_NAME "QLIEInjector32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

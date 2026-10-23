cmake_minimum_required(VERSION 3.16)

project(DangoRenPyHookHost LANGUAGES CXX)

add_executable(RenPyHost64 ../livemaker/main.cpp)
add_executable(RenPyInjector64 ../livemaker/injector.cpp)
target_compile_features(RenPyHost64 PRIVATE cxx_std_17)
target_compile_features(RenPyInjector64 PRIVATE cxx_std_17)
target_compile_definitions(RenPyHost64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_RENPY_ONLY=1)
target_compile_definitions(RenPyInjector64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "RenPy Hook 组件必须使用x64工具链构建")
endif()

if(MSVC)
    target_compile_options(RenPyHost64 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(RenPyInjector64 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET RenPyHost64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET RenPyInjector64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(RenPyHost64 PROPERTIES
    OUTPUT_NAME "RenPyHost64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(RenPyInjector64 PROPERTIES
    OUTPUT_NAME "RenPyInjector64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

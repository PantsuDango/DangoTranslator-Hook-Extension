cmake_minimum_required(VERSION 3.16)

project(DangoRenPyHookHost LANGUAGES CXX)

add_executable(RenPyHost ../livemaker/main.cpp)
add_executable(RenPyInjector ../livemaker/injector.cpp)
target_compile_features(RenPyHost PRIVATE cxx_std_17)
target_compile_features(RenPyInjector PRIVATE cxx_std_17)
target_compile_definitions(RenPyHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_RENPY_ONLY=1)
target_compile_definitions(RenPyInjector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(RENPY_BIT_SUFFIX "32")
else()
    set(RENPY_BIT_SUFFIX "64")
endif()

if(MSVC)
    target_compile_options(RenPyHost PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(RenPyInjector PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET RenPyHost PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET RenPyInjector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(RenPyHost PROPERTIES
    OUTPUT_NAME "RenPyHost${RENPY_BIT_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(RenPyInjector PROPERTIES
    OUTPUT_NAME "RenPyInjector${RENPY_BIT_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

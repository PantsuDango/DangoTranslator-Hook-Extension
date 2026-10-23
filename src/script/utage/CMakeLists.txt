cmake_minimum_required(VERSION 3.16)

project(DangoUtageHookHost LANGUAGES CXX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(UTAGE_BIT_SUFFIX "64")
else()
    set(UTAGE_BIT_SUFFIX "32")
endif()

add_executable(UtageHost ../livemaker/main.cpp)
add_executable(UtageInjector ../livemaker/injector.cpp)
target_compile_features(UtageHost PRIVATE cxx_std_17)
target_compile_features(UtageInjector PRIVATE cxx_std_17)
target_compile_definitions(UtageHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_UTAGE_ONLY=1)
target_compile_definitions(UtageInjector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(UtageHost PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(UtageInjector PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET UtageHost PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET UtageInjector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(UtageHost PROPERTIES
    OUTPUT_NAME "UtageHost${UTAGE_BIT_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(UtageInjector PROPERTIES
    OUTPUT_NAME "UtageInjector${UTAGE_BIT_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

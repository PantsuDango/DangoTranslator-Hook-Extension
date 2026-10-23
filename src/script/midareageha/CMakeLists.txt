cmake_minimum_required(VERSION 3.16)

project(MIDAREAGEHAHookHost LANGUAGES CXX)

add_executable(MidareagehaHost32 ../livemaker/main.cpp)
add_executable(MidareagehaInjector32 ../livemaker/injector.cpp)
target_compile_features(MidareagehaHost32 PRIVATE cxx_std_17)
target_compile_features(MidareagehaInjector32 PRIVATE cxx_std_17)
target_compile_definitions(MidareagehaHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_MIDAREAGEHA_ONLY=1)
target_compile_definitions(MidareagehaInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MIDAREAGEHA Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(MidareagehaHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(MidareagehaInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET MidareagehaHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET MidareagehaInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(MidareagehaHost32 PROPERTIES
    OUTPUT_NAME "MidareagehaHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(MidareagehaInjector32 PROPERTIES
    OUTPUT_NAME "MidareagehaInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

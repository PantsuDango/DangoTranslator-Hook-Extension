cmake_minimum_required(VERSION 3.16)

project(DangoLiveMakerHookHost LANGUAGES CXX)

add_executable(LiveMakerHost32 main.cpp)
add_executable(LiveMakerInjector32 injector.cpp)
target_compile_features(LiveMakerHost32 PRIVATE cxx_std_17)
target_compile_features(LiveMakerInjector32 PRIVATE cxx_std_17)
target_compile_definitions(LiveMakerHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(LiveMakerInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LiveMaker Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(LiveMakerHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(LiveMakerInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET LiveMakerHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET LiveMakerInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(LiveMakerHost32 PROPERTIES
    OUTPUT_NAME "LiveMakerHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(LiveMakerInjector32 PROPERTIES
    OUTPUT_NAME "LiveMakerInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

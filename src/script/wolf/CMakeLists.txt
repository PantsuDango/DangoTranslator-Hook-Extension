cmake_minimum_required(VERSION 3.16)

project(WolfRPGHookHost LANGUAGES CXX)

add_executable(WolfHost32 ../livemaker/main.cpp)
add_executable(WolfInjector32 ../livemaker/injector.cpp)
target_compile_features(WolfHost32 PRIVATE cxx_std_17)
target_compile_features(WolfInjector32 PRIVATE cxx_std_17)
target_compile_definitions(WolfHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_WOLF_ONLY=1)
target_compile_definitions(WolfInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WOLF RPG Editor Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(WolfHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(WolfInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET WolfHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET WolfInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(WolfHost32 PROPERTIES
    OUTPUT_NAME "WolfHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(WolfInjector32 PROPERTIES
    OUTPUT_NAME "WolfInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

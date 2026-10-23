cmake_minimum_required(VERSION 3.16)

project(DangoYurisHookHost LANGUAGES CXX)

add_executable(YurisHost32 ../livemaker/main.cpp)
add_executable(YurisInjector32 ../livemaker/injector.cpp)
target_compile_features(YurisHost32 PRIVATE cxx_std_17)
target_compile_features(YurisInjector32 PRIVATE cxx_std_17)
target_compile_definitions(YurisHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_YURIS_ONLY=1)
target_compile_definitions(YurisInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "YU-RIS Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(YurisHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(YurisInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET YurisHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET YurisInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(YurisHost32 PROPERTIES
    OUTPUT_NAME "YurisHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(YurisInjector32 PROPERTIES
    OUTPUT_NAME "YurisInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

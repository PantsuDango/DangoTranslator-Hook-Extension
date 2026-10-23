cmake_minimum_required(VERSION 3.16)

project(Taskforce2HookHost LANGUAGES CXX)

add_executable(Taskforce2Host32 ../livemaker/main.cpp)
add_executable(Taskforce2Injector32 ../livemaker/injector.cpp)
target_compile_features(Taskforce2Host32 PRIVATE cxx_std_17)
target_compile_features(Taskforce2Injector32 PRIVATE cxx_std_17)
target_compile_definitions(Taskforce2Host32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_TASKFORCE2_ONLY=1)
target_compile_definitions(Taskforce2Injector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Taskforce2 Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(Taskforce2Host32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(Taskforce2Injector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET Taskforce2Host32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET Taskforce2Injector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(Taskforce2Host32 PROPERTIES
    OUTPUT_NAME "Taskforce2Host32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(Taskforce2Injector32 PROPERTIES
    OUTPUT_NAME "Taskforce2Injector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

cmake_minimum_required(VERSION 3.16)

project(DangoWillPlusHookHost LANGUAGES CXX)

add_executable(WillPlusHost32 ../livemaker/main.cpp)
add_executable(WillPlusInjector32 ../livemaker/injector.cpp)
target_compile_features(WillPlusHost32 PRIVATE cxx_std_17)
target_compile_features(WillPlusInjector32 PRIVATE cxx_std_17)
target_compile_definitions(WillPlusHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_WILLPLUS_ONLY=1)
target_compile_definitions(WillPlusInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WillPlus Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(WillPlusHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(WillPlusInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET WillPlusHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET WillPlusInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(WillPlusHost32 PROPERTIES OUTPUT_NAME "WillPlusHost32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(WillPlusInjector32 PROPERTIES OUTPUT_NAME "WillPlusInjector32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

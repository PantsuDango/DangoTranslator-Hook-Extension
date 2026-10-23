cmake_minimum_required(VERSION 3.16)

project(DangoDDWorksHookHost LANGUAGES CXX)

add_executable(DDWorksHost32 ../livemaker/main.cpp)
add_executable(DDWorksInjector32 ../livemaker/injector.cpp)
target_compile_features(DDWorksHost32 PRIVATE cxx_std_17)
target_compile_features(DDWorksInjector32 PRIVATE cxx_std_17)
target_compile_definitions(DDWorksHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_DDWORKS_ONLY=1)
target_compile_definitions(DDWorksInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DDWorks Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(DDWorksHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(DDWorksInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET DDWorksHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET DDWorksInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(DDWorksHost32 PROPERTIES OUTPUT_NAME "DDWorksHost32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(DDWorksInjector32 PROPERTIES OUTPUT_NAME "DDWorksInjector32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

cmake_minimum_required(VERSION 3.16)

project(DangoPJADVHookHost LANGUAGES CXX)

add_executable(PJADVHost32 ../livemaker/main.cpp)
add_executable(PJADVInjector32 ../livemaker/injector.cpp)
target_compile_features(PJADVHost32 PRIVATE cxx_std_17)
target_compile_features(PJADVInjector32 PRIVATE cxx_std_17)
target_compile_definitions(PJADVHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_PJADV_ONLY=1)
target_compile_definitions(PJADVInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PJADV Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(PJADVHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(PJADVInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET PJADVHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET PJADVInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(PJADVHost32 PROPERTIES OUTPUT_NAME "PJADVHost32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set_target_properties(PJADVInjector32 PROPERTIES OUTPUT_NAME "PJADVInjector32" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

cmake_minimum_required(VERSION 3.16)

project(BoleroHookHost LANGUAGES CXX)

add_executable(BoleroHost32 ../livemaker/main.cpp)
add_executable(BoleroInjector32 ../livemaker/injector.cpp)
target_compile_features(BoleroHost32 PRIVATE cxx_std_17)
target_compile_features(BoleroInjector32 PRIVATE cxx_std_17)
target_compile_definitions(BoleroHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_BOLERO_ONLY=1)
target_compile_definitions(BoleroInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Bolero ADV System Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(BoleroHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(BoleroInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET BoleroHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET BoleroInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(BoleroHost32 PROPERTIES
    OUTPUT_NAME "BoleroHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(BoleroInjector32 PROPERTIES
    OUTPUT_NAME "BoleroInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

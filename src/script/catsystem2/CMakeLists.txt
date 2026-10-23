cmake_minimum_required(VERSION 3.16)

project(CatSystem2HookHost LANGUAGES CXX)

add_executable(CatSystem2Host32 ../livemaker/main.cpp)
add_executable(CatSystem2Injector32 ../livemaker/injector.cpp)
target_compile_features(CatSystem2Host32 PRIVATE cxx_std_17)
target_compile_features(CatSystem2Injector32 PRIVATE cxx_std_17)
target_compile_definitions(CatSystem2Host32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_CATSYSTEM2_ONLY=1)
target_compile_definitions(CatSystem2Injector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CatSystem2 Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(CatSystem2Host32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(CatSystem2Injector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET CatSystem2Host32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET CatSystem2Injector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(CatSystem2Host32 PROPERTIES
    OUTPUT_NAME "CatSystem2Host32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(CatSystem2Injector32 PROPERTIES
    OUTPUT_NAME "CatSystem2Injector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

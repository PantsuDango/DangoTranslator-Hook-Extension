cmake_minimum_required(VERSION 3.16)

project(DangoKiriKiriHookHost LANGUAGES CXX)

add_executable(KiriKiriHost32 ../livemaker/main.cpp)
add_executable(KiriKiriInjector32 ../livemaker/injector.cpp)
add_library(KiriKiriRuntime32 SHARED runtime/main.cpp runtime/display_plan.cpp runtime/sdk_abi.cpp runtime/sdk_current.cpp runtime/sdk_legacy.cpp runtime/version.rc)
set_property(SOURCE runtime/sdk_current.cpp runtime/sdk_legacy.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/sdk_impl.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/display_entry.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/display_inspect.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/display_script.inc"
)
target_compile_features(KiriKiriRuntime32 PRIVATE cxx_std_17)
target_compile_definitions(KiriKiriRuntime32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(KiriKiriRuntime32 PRIVATE ws2_32 iphlpapi bcrypt)
target_compile_features(KiriKiriHost32 PRIVATE cxx_std_17)
target_compile_features(KiriKiriInjector32 PRIVATE cxx_std_17)
target_compile_definitions(KiriKiriHost32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_KIRIKIRI_ONLY=1)
target_compile_definitions(KiriKiriInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "KiriKiri Hook 组件必须使用 Win32 工具链构建")
endif()

if(MSVC)
    target_compile_options(KiriKiriRuntime32 PRIVATE /utf-8 /W3 /EHsc)
    set_property(TARGET KiriKiriRuntime32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(KiriKiriHost32 PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(KiriKiriInjector32 PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET KiriKiriHost32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET KiriKiriInjector32 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(KiriKiriHost32 PROPERTIES
    OUTPUT_NAME "KiriKiriHost32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(KiriKiriInjector32 PROPERTIES
    OUTPUT_NAME "KiriKiriInjector32"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

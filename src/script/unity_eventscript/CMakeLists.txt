cmake_minimum_required(VERSION 3.16)

project(DangoUnityEventScriptHookHost LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Unity EventScript Hook仅支持64位目标")
endif()

add_executable(UnityEventScriptHost ../livemaker/main.cpp)
add_executable(UnityEventScriptInjector ../livemaker/injector.cpp)
target_compile_features(UnityEventScriptHost PRIVATE cxx_std_17)
target_compile_features(UnityEventScriptInjector PRIVATE cxx_std_17)
target_compile_definitions(UnityEventScriptHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX DANGO_UNITY_EVENTSCRIPT_ONLY=1)
target_compile_definitions(UnityEventScriptInjector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(UnityEventScriptHost PRIVATE /utf-8 /W4 /EHsc)
    target_compile_options(UnityEventScriptInjector PRIVATE /utf-8 /W4 /EHsc)
    set_property(TARGET UnityEventScriptHost PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(TARGET UnityEventScriptInjector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(UnityEventScriptHost PROPERTIES
    OUTPUT_NAME "UnityEventScriptHost64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set_target_properties(UnityEventScriptInjector PROPERTIES
    OUTPUT_NAME "UnityEventScriptInjector64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

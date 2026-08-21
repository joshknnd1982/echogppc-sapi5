cmake_minimum_required(VERSION 3.15)
project(EchoGPPCSAPI VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ECHO_ARCH "x64")
    set(ECHO_EMULATOR_DLL "echotalk64.dll")
else()
    set(ECHO_ARCH "x86")
    set(ECHO_EMULATOR_DLL "echotalk32.dll")
endif()
message(STATUS "Echo GPPC SAPI5: building ${ECHO_ARCH} against ${ECHO_EMULATOR_DLL}")

# The emulator is loaded at run time with LoadLibrary rather than linked, so
# there is no import library to find and one source tree builds for both
# architectures unchanged. Unlike the BestSpeech engine this replaces, no
# 32-bit surrogate process or named pipe is needed: echotalk ships a native
# build for each architecture.
set(ECHO_ENGINE_SOURCES
    src/sapi_main.cpp
    src/com.cpp
    src/registry.cpp
    src/echo_core.cpp
    src/echo_log.cpp
    src/echo_settings.cpp
    src/ISpDataKeyImpl.cpp
    src/IEnumSpObjectTokensImpl.cpp
    src/ISpTTSEngineImpl.cpp
    src/voice_token.cpp
)

add_library(EchoGPPCSAPI SHARED
    ${ECHO_ENGINE_SOURCES}
    src/echogppc_sapi.def
)

target_include_directories(EchoGPPCSAPI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(EchoGPPCSAPI PRIVATE
    ole32
    oleaut32
    advapi32
    shell32
    shlwapi
)

target_compile_definitions(EchoGPPCSAPI PRIVATE
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# A console harness that drives the engine through real SAPI5, so the engine
# can be verified end to end rather than only unit-tested in isolation.
add_executable(EchoGPPCTest src/sapi_selftest.cpp)
target_include_directories(EchoGPPCTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(EchoGPPCTest PRIVATE ole32 oleaut32 sapi)
target_compile_definitions(EchoGPPCTest PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# The configuration utility. It shares the settings and logging code with the
# engine but deliberately not echo_core: it finds voices through SAPI, so this
# process never holds an emulator instance that could collide with the one the
# engine needs while speaking the test phrase.
add_executable(EchoGPPCConfig WIN32
    src/config_ui.cpp
    src/echo_settings.cpp
    src/echo_log.cpp
    src/config_ui.rc
)
target_include_directories(EchoGPPCConfig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(EchoGPPCConfig PRIVATE
    ole32
    oleaut32
    comctl32
    shell32
    user32
    advapi32
)
target_compile_definitions(EchoGPPCConfig PRIVATE
    _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    foreach(target EchoGPPCSAPI EchoGPPCTest EchoGPPCConfig)
        target_compile_options(${target} PRIVATE /W4 /MP /permissive-)
        # Static CRT so the installed engine has no redistributable dependency:
        # it gets loaded into arbitrary host processes and cannot rely on the
        # host having a matching runtime.
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

# Stage the emulator and the Textalker ROM pairs next to the freshly built
# engine so it can be registered and tested straight out of the build tree.
set(ECHO_RUNTIME_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ECHO_EMULATOR_DLL}"
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/textalker.ram.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/textalker.obj.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/textalker_v13.ram.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/bin/textalker_v13.obj.bin"
)

foreach(file ${ECHO_RUNTIME_FILES})
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "Required Echo GPPC runtime file is missing: ${file}")
    endif()
endforeach()

add_custom_command(TARGET EchoGPPCSAPI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ECHO_RUNTIME_FILES} "$<TARGET_FILE_DIR:EchoGPPCSAPI>"
    COMMENT "Staging the Echo GPPC emulator and Textalker ROMs beside the engine"
)

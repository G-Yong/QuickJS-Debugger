cmake_minimum_required(VERSION 3.10)
project(qjs_debugger LANGUAGES C CXX)

# MSVC < 17.5 (cl < 19.35) does not ship <stdatomic.h> for C.
# Inject a shim directory so that #include <stdatomic.h> in QuickJS
# picks up our compatibility header instead of failing.
if(MSVC AND (MSVC_VERSION LESS 1935))
    message(STATUS "MSVC ${MSVC_VERSION} lacks C11 <stdatomic.h> – using compat shim")
    include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/compat/msvc")
endif()

add_subdirectory(quickjs)

# Force DIRECT_DISPATCH=0 in QuickJS so that the per-opcode handler
# (JS_SetOPChangedHandler) fires on every instruction.
# With MSVC, _MSC_VER already triggers DIRECT_DISPATCH=0.
# For GCC/Clang/MinGW we define EMSCRIPTEN to achieve the same effect.
if(NOT MSVC)
    target_compile_definitions(qjs PRIVATE EMSCRIPTEN)
    message(STATUS "Non-MSVC compiler – defining EMSCRIPTEN to disable DIRECT_DISPATCH")
endif()

add_subdirectory(debugger)

# ── Bundle sched.lua as a C byte array ───────────────────────────────────────
# EMBED_TXTFILES mis-resolves the generated .S path under PlatformIO (doubles the
# build dir → "Source sched.lua.S not found"), so generate the array ourselves at
# configure time. Reconfigures automatically when sched.lua changes.
# NOTE: ${CMAKE_CURRENT_SOURCE_DIR} points at the project root during ESP-IDF's
# requirements scan, so address sched.lua explicitly from the project root.
set(SCHED_LUA_SRC "${CMAKE_SOURCE_DIR}/components/lua_runner/sched.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCHED_LUA_SRC}")
file(READ "${SCHED_LUA_SRC}" SCHED_LUA_HEX HEX)
string(LENGTH "${SCHED_LUA_HEX}" SCHED_LUA_HEXLEN)
math(EXPR SCHED_LUA_LEN "${SCHED_LUA_HEXLEN} / 2")
string(REGEX REPLACE "(..)" "0x\\1," SCHED_LUA_ARR "${SCHED_LUA_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sched_lua_embed.h"
"/* Generated from sched.lua at configure time — do not edit. */\n"
"static const char sched_lua_start[] = { ${SCHED_LUA_ARR} };\n"
"static const unsigned sched_lua_size = ${SCHED_LUA_LEN};\n")

set(LUA_LOCAL_DIR "${CMAKE_SOURCE_DIR}/components/lua")
set(HAVE_LOCAL_LUA FALSE)
if (EXISTS "${LUA_LOCAL_DIR}/lapi.c")
    set(HAVE_LOCAL_LUA TRUE)
endif()

if (HAVE_LOCAL_LUA)
    idf_component_register(
        SRCS
            "lua_runner.c"
        INCLUDE_DIRS
            "include"
        REQUIRES
            device_commands
            time_sync
            json
            lua
    )
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
else()
    set(LUA_DEP_PACKAGES "${CMAKE_SOURCE_DIR}/.pio/libdeps/*/lua")
    file(GLOB LUA_DEP_DIRS "${LUA_DEP_PACKAGES}")
    if (NOT LUA_DEP_DIRS)
        message(FATAL_ERROR "Lua package sources not found. Ensure lib_deps includes lua and run a clean PlatformIO dependency fetch/build.")
    endif()

    list(GET LUA_DEP_DIRS 0 LUA_SRC_BASE)
    set(LUA_SRC_DIR "${LUA_SRC_BASE}/src")
    set(LUA_INCLUDE_DIRS
        "${LUA_SRC_BASE}"
        "${LUA_SRC_DIR}"
    )

    idf_component_register(
        SRCS
            "lua_runner.c"
        INCLUDE_DIRS
            "include"
        REQUIRES
            device_commands
            time_sync
            json
    )

    target_include_directories(${COMPONENT_LIB} PRIVATE ${LUA_INCLUDE_DIRS} "${CMAKE_CURRENT_BINARY_DIR}")

    set(LUA_SOURCES
        "${LUA_SRC_DIR}/lapi.c"
        "${LUA_SRC_DIR}/lcode.c"
        "${LUA_SRC_DIR}/lctype.c"
        "${LUA_SRC_DIR}/ldebug.c"
        "${LUA_SRC_DIR}/ldo.c"
        "${LUA_SRC_DIR}/ldump.c"
        "${LUA_SRC_DIR}/lfunc.c"
        "${LUA_SRC_DIR}/lgc.c"
        "${LUA_SRC_DIR}/llex.c"
        "${LUA_SRC_DIR}/lmem.c"
        "${LUA_SRC_DIR}/lobject.c"
        "${LUA_SRC_DIR}/lopcodes.c"
        "${LUA_SRC_DIR}/lparser.c"
        "${LUA_SRC_DIR}/lstate.c"
        "${LUA_SRC_DIR}/lstring.c"
        "${LUA_SRC_DIR}/ltable.c"
        "${LUA_SRC_DIR}/ltm.c"
        "${LUA_SRC_DIR}/lundump.c"
        "${LUA_SRC_DIR}/lvm.c"
        "${LUA_SRC_DIR}/lzio.c"
        "${LUA_SRC_DIR}/lauxlib.c"
        "${LUA_SRC_DIR}/lbaselib.c"
        "${LUA_SRC_DIR}/ldblib.c"
        "${LUA_SRC_DIR}/liolib.c"
        "${LUA_SRC_DIR}/lmathlib.c"
        "${LUA_SRC_DIR}/loslib.c"
        "${LUA_SRC_DIR}/ltablib.c"
        "${LUA_SRC_DIR}/lstrlib.c"
        "${LUA_SRC_DIR}/lutf8lib.c"
        "${LUA_SRC_DIR}/loadlib.c"
        "${LUA_SRC_DIR}/lcorolib.c"
        "${LUA_SRC_DIR}/linit.c"
    )

    target_sources(${COMPONENT_LIB} PRIVATE ${LUA_SOURCES})
endif()

cmake_minimum_required(VERSION 3.16)
project(phone_emu C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# <root>/emu/CMakeLists.txt  ->  <root>
get_filename_component(PHONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

# ---- the project's own Lua VM (compiled unchanged, NOT using the force-include) -------------
file(GLOB LUA_SRC "${PHONE_ROOT}/src/lua/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(phone_lua STATIC ${LUA_SRC})
target_include_directories(phone_lua PUBLIC "${PHONE_ROOT}/include")
target_compile_definitions(phone_lua PRIVATE LUA_USE_POSIX)
# fopen("/fs/...") redirection for loadfile/dofile (no malloc tracking: Lua uses its own allocator)
target_compile_options(phone_lua PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/shim/emu_force.h" -w)
target_link_libraries(phone_lua PUBLIC m)

# ---- the project's own device code that defines the Lua API (compiled UNCHANGED) ------------
#   api.c wrap.c tft_gfx.c wifi/lua_wifi.c cursor_poll.c
# Everything below them (FreeRTOS, gpio, esp_lcd, wifi driver...) is replaced by emu/shim + emu/src.
set(DEVICE_SRC
  "${PHONE_ROOT}/src/api.c"
  "${PHONE_ROOT}/src/wrap.c"
  "${PHONE_ROOT}/src/tft_gfx.c"
  "${PHONE_ROOT}/src/wifi/lua_wifi.c")
# optional device-side extras (each is added only if present in the project)
foreach(extra cursor_poll.c)
  if(EXISTS "${PHONE_ROOT}/src/${extra}")
    list(APPEND DEVICE_SRC "${PHONE_ROOT}/src/${extra}")
  endif()
endforeach()
add_library(phone_device STATIC ${DEVICE_SRC})
target_include_directories(phone_device PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${PHONE_ROOT}/include")
target_compile_definitions(phone_device PRIVATE EMU_TRACK_ALLOC)
target_compile_options(phone_device PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/shim/emu_force.h" -w)
target_link_libraries(phone_device PUBLIC phone_lua)

# ---- emulator ---------------------------------------------------------------------------------
file(GLOB EMU_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB EMU_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/modules/*.cpp")   # <- drop new Lua APIs here
add_executable(phone_emu ${EMU_SRC} ${EMU_MODULES})
target_include_directories(phone_emu PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${PHONE_ROOT}/include"
  ${SDL2_INCLUDE_DIRS})
target_compile_definitions(phone_emu PRIVATE EMU_DEFAULT_ROOT="${PHONE_ROOT}")
target_compile_options(phone_emu PRIVATE -Wall -Wextra -Wno-unused-parameter)
# the module registrars are static objects: keep them even though nothing references them
target_link_libraries(phone_emu PRIVATE -Wl,--whole-archive phone_device -Wl,--no-whole-archive
  phone_lua ${SDL2_LIBRARIES} Threads::Threads)

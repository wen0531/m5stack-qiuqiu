cmake_minimum_required(VERSION 3.16)
project(stopwatch_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(FW   ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL ${FW}/components/lvgl)
set(UITK ${FW}/components/smooth_ui_toolkit/src)
set(MC    ${FW}/components/mooncake/src)

# The simulator compiles the firmware's UI sources unchanged; only the HAL and
# board headers are replaced by stubs (that is the whole point of the design).
file(GLOB_RECURSE LVGL_SRC   ${LVGL}/src/*.c)
file(GLOB_RECURSE UITK_SRC   ${UITK}/*.cpp)
file(GLOB_RECURSE APP_SRC    ${FW}/main/apps/*.cpp)
file(GLOB_RECURSE  ASSET_SRC ${FW}/main/assets/fonts/*.c ${FW}/main/assets/images/*.c ${FW}/main/assets/sfx/*.c)
file(GLOB_RECURSE MC_SRC     ${MC}/*.cpp)

add_executable(stopwatch_sim
    src/sim_main.cpp
    src/hal_sim.cpp
    src/sim_input.cpp
    src/png.cpp
    src/sim_display.cpp
    src/sim_log.cpp
    src/mem_guard.cpp
    src/sim_clock.cpp
    src/time_shim.cpp
    src/voice_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/button/Button_Class.cpp
    ${FW}/main/i18n/i18n.cpp
    ${APP_SRC}
    ${ASSET_SRC}
    ${UITK_SRC}
    ${MC_SRC}
    ${LVGL_SRC}
)

target_include_directories(stopwatch_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LVGL}
    ${FW}/main
    ${FW}/main/assets
    ${UITK}
    ${UITK}/lvgl
    ${UITK}/..
    ${MC}
)

target_compile_definitions(stopwatch_sim PRIVATE LV_CONF_INCLUDE_SIMPLE)
# fmt is only needed by C++ sources (the firmware uses fmt::format in a few places)
set_source_files_properties(${APP_SRC} ${UITK_SRC} ${MC_SRC} src/sim_main.cpp src/hal_sim.cpp
    PROPERTIES COMPILE_OPTIONS "-include;fmt/format.h")
target_compile_options(stopwatch_sim PRIVATE -w)
# route libc allocation through the device-budget guard
target_link_options(stopwatch_sim PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
    -Wl,--wrap=time -Wl,--wrap=localtime -Wl,--wrap=localtime_r -Wl,--wrap=gettimeofday)
target_link_libraries(stopwatch_sim PRIVATE z m pthread fmt)

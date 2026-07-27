cmake_minimum_required(VERSION 3.15)
project(rtl-dvbt-macos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --------------------------------------------------------------------------
# libusb
# --------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# --------------------------------------------------------------------------
# librtlsdr
#
# We compile librtlsdr's sources directly into our binary rather than linking
# against a prebuilt library. This is not a style preference - it is required.
# rtlsdr_demod_write_reg() and rtlsdr_demod_read_reg() are the entire basis of
# this driver, and they are absent from the public header while librtlsdr
# builds with -fvisibility=hidden. A Homebrew librtlsdr does not export them,
# so there is nothing to link against. Building the sources ourselves puts
# them in the same translation unit set and they resolve normally.
# --------------------------------------------------------------------------
set(LIBRTLSDR_DIR ${CMAKE_SOURCE_DIR}/third_party/librtlsdr)

if(NOT EXISTS ${LIBRTLSDR_DIR}/src/librtlsdr.c)
    message(FATAL_ERROR
        "librtlsdr sources not found. Run:\n"
        "  git submodule update --init --recursive")
endif()

set(LIBRTLSDR_SOURCES
    ${LIBRTLSDR_DIR}/src/librtlsdr.c
    ${LIBRTLSDR_DIR}/src/tuner_e4k.c
    ${LIBRTLSDR_DIR}/src/tuner_fc0012.c
    ${LIBRTLSDR_DIR}/src/tuner_fc0013.c
    ${LIBRTLSDR_DIR}/src/tuner_fc2580.c
    ${LIBRTLSDR_DIR}/src/tuner_r82xx.c
)

# --------------------------------------------------------------------------
# rtl_dvbt
# --------------------------------------------------------------------------
add_executable(rtl_dvbt
    src/rtl_dvbt.c
    src/rtl2832_dvbt.c
    ${LIBRTLSDR_SOURCES}
)

target_include_directories(rtl_dvbt PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LIBRTLSDR_DIR}/include
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_directories(rtl_dvbt PRIVATE ${LIBUSB_LIBRARY_DIRS})
target_link_libraries(rtl_dvbt PRIVATE ${LIBUSB_LIBRARIES})

# librtlsdr's own build defines this; its sources expect it.
target_compile_definitions(rtl_dvbt PRIVATE _GNU_SOURCE)

# librtlsdr is not warning-clean and it is not our code to fix. Keep our
# warnings loud and theirs quiet.
set_source_files_properties(${LIBRTLSDR_SOURCES} PROPERTIES
    COMPILE_FLAGS "-w")
target_compile_options(rtl_dvbt PRIVATE -Wall -Wextra)

install(TARGETS rtl_dvbt RUNTIME DESTINATION bin)

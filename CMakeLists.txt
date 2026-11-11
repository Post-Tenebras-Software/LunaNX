cmake_minimum_required(VERSION 3.20)
project(LunaNX VERSION 1.0.1 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale libavformat libswresample)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
find_package(ZLIB REQUIRED)

add_library(lunanx_mbedtls_platform STATIC source/mbedtls_switch.c)
target_include_directories(lunanx_mbedtls_platform PRIVATE external/mbedtls/include)
target_compile_definitions(lunanx_mbedtls_platform PRIVATE
    MBEDTLS_USER_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/config/mbedtls_switch.h")

add_library(lunanx_rtc STATIC IMPORTED)
set_target_properties(lunanx_rtc PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build-libdatachannel-switch/libdatachannel.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/libdatachannel/include"
    INTERFACE_COMPILE_DEFINITIONS "RTC_STATIC;RTC_ENABLE_MEDIA=1;RTC_ENABLE_WEBSOCKET=0")
add_library(lunanx_juice STATIC IMPORTED)
set_target_properties(lunanx_juice PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build-libdatachannel-switch/deps/libjuice/libjuice.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/libdatachannel/deps/libjuice/include"
    INTERFACE_COMPILE_DEFINITIONS JUICE_STATIC)
add_library(lunanx_usrsctp STATIC IMPORTED)
set_target_properties(lunanx_usrsctp PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/build-libdatachannel-switch/deps/usrsctp/usrsctplib/libusrsctp.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/libdatachannel/deps/usrsctp/usrsctplib")

add_executable(LunaNX
    source/main.cpp source/native_runtime.cpp source/native_auth.cpp source/native_session.cpp
    source/numeric_address.c source/h264_decoder.cpp source/video_view.cpp source/opus_audio.cpp
    source/controller_input.cpp source/luna_input.cpp source/luna_control.cpp source/luna_time_sync.cpp
    source/local_control_session.cpp source/luna_session.cpp source/launcher_ui.cpp source/launcher_view.cpp
    source/launcher_artwork.cpp source/launcher_image_cache.cpp source/launcher_shortcut.cpp
    source/launcher_capture.cpp source/launcher_preferences.cpp source/performance_overlay.cpp
    source/launcher_recent.cpp source/remembered_login.cpp source/login_key_switch.cpp
    source/native_profiles.cpp source/trailer_decoder.cpp source/launcher_trailer.cpp)
target_include_directories(LunaNX PRIVATE source external/libdatachannel/deps/json/include
    external/mbedtls/include config)
target_compile_features(LunaNX PRIVATE cxx_std_17)
target_compile_options(LunaNX PRIVATE -Wall -Wextra)
target_compile_definitions(LunaNX PRIVATE MBEDTLS_USER_CONFIG_FILE="mbedtls_switch.h"
    LUNANX_AUTH_VERSION="${PROJECT_VERSION}" LUNANX_BUILD_VERSION="${PROJECT_VERSION}"
    LUNANX_BUILD_CHANNEL="1.0.1")
target_link_options(LunaNX PRIVATE "-Wl,--wrap=getnameinfo" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/LunaNX.map")
target_link_libraries(LunaNX PRIVATE lunanx_rtc lunanx_juice lunanx_usrsctp
    "${CMAKE_CURRENT_SOURCE_DIR}/build-libdatachannel-switch/deps/libsrtp/libsrtp2.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/build-mbedtls-switch/library/libmbedtls.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/build-mbedtls-switch/library/libmbedx509.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/build-mbedtls-switch/library/libmbedcrypto.a"
    lunanx_mbedtls_platform PkgConfig::CURL PkgConfig::FFMPEG PkgConfig::FREETYPE ZLIB::ZLIB)
nx_generate_nacp(OUTPUT LunaNX.nacp NAME "LunaNX" AUTHOR "PostTenebrasSoftware" VERSION "${PROJECT_VERSION}")
include(dkp-asset-folder)
dkp_add_asset_target(lunanx_assets "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher")
dkp_track_assets(lunanx_assets FILES fonts/Inter-Regular.ttf fonts/Inter-SemiBold.ttf fonts/LICENSE.txt)
nx_create_nro(LunaNX NACP LunaNX.nacp ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/icon.jpg" ROMFS lunanx_assets)

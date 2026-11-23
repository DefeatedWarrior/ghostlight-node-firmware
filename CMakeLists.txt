# Minimum CMake version
cmake_minimum_required(VERSION 3.5)

# Registry components (mdns, littlefs, led_strip; see main/idf_component.yml)
# are fetched into managed_components/ and added by the component manager,
# so they must not be listed in EXTRA_COMPONENT_DIRS (that breaks a clean
# checkout, where the folder doesn't exist yet).

# Firmware version (shown in the web UI, ArtPollReply and the OTA image)
set(PROJECT_VER "1.2.0")

# Board: tools/build.sh <board> passes SDKCONFIG_DEFAULTS for the board it
# builds. Plain `idf.py build` (no SDKCONFIG_DEFAULTS given) builds the
# Ghostlight Node rev1. See docs/DEVELOPING.md.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/ghostlight-rev1/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# lwIP counts one multicast membership per group per network interface, 8 in
# all by default. Ethernet, Wi-Fi and the node's own AP each hold the
# all-systems and mDNS groups, and sACN joins every output universe, the sync
# universes its sources use and the discovery universe on each of them.
idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_IGMP_GROUP=24" APPEND)

project(ESP-NODE-2RDM)

# The build date and time in the app description are __DATE__ and __TIME__ in
# ESP-IDF's esp_app_desc.c, which an incremental build doesn't recompile: write
# them each time the app is linked (before the image is made from the ELF)
if(CONFIG_APP_COMPILE_TIME_DATE)
    idf_build_get_property(python PYTHON)
    idf_build_get_property(app_elf EXECUTABLE)
    add_custom_command(TARGET ${app_elf} POST_BUILD
        COMMAND ${python} ${CMAKE_SOURCE_DIR}/tools/stamp_build_date.py
                $<TARGET_FILE:${app_elf}> ${CMAKE_PROJECT_NAME}
        VERBATIM)
endif()

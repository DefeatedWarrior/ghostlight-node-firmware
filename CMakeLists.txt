# Minimum CMake version
cmake_minimum_required(VERSION 3.5)

# Registry components (mdns, littlefs, led_strip; see main/idf_component.yml)
# are fetched into managed_components/ and added by the component manager,
# so they must not be listed in EXTRA_COMPONENT_DIRS (that breaks a clean
# checkout, where the folder doesn't exist yet).

# Firmware version (shown in the web UI, ArtPollReply and the OTA image)
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP-NODE-2RDM)
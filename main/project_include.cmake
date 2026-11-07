# SPDX-License-Identifier: Apache-2.0
#
# esp_ot_br_server (third_party/esp-thread-br) always packs its stock web UI
# into a SPIFFS image for a "web_storage" partition. This firmware has no such
# partition -- the partition table is frozen since the first release and only
# the border router's REST API is used -- so that step would fail the build.
#
# project_include.cmake files are read before any component's CMakeLists.txt,
# so redefining the function here turns that one call into a no-op without
# patching the submodule. CMake keeps the previous definition available as
# _spiffs_create_partition_image, so any other SPIFFS image still works.
function(spiffs_create_partition_image partition)
    if("${partition}" STREQUAL "web_storage")
        message(STATUS "Skipping SPIFFS image '${partition}': no such partition in this firmware")
        return()
    endif()
    _spiffs_create_partition_image(${ARGV})
endfunction()

# SPDX-License-Identifier: Apache-2.0
# ESP32-C5 single-chip OpenThread Border Router
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PROJECT_VER is read automatically from version.txt in this directory.
# CI overwrites version.txt with the release tag before building.
project(esp32c5_otbr)

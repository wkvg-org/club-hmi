# Copyright (c) 2025-2026 Wolfenbütteler Kultur- und Veranstaltungsgesellschaft gemeinnützige UG (haftungsbeschränkt)
# SPDX-License-Identifier: Apache-2.0
#
# club-display: bar-dashboard application for the Waveshare ESP32-P4-NANO
# 10.1" MIPI-DSI panel. See PLAN.md for the full design.
#
# The out-of-tree board (waveshare_esp32p4_nano), devicetree bindings and
# drivers all live in ./module and are wired in as a Zephyr module below,
# *before* find_package(Zephyr) runs the module/board/dts discovery.

cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/module)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(club_display)

target_sources(app PRIVATE src/main.c)

add_subdirectory(src/modules/config)
add_subdirectory(src/modules/net)
add_subdirectory(src/modules/time)
add_subdirectory(src/modules/ui)
add_subdirectory(src/modules/shell)

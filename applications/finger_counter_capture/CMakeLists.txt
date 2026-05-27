# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

cmake_minimum_required(VERSION 3.20.0)

# Re-use the arducam_mega DT binding (arducam,mega.yaml) from person_detection.
# DTS_ROOT must be appended BEFORE find_package(Zephyr), otherwise Zephyr's DTS
# pass runs without our extra binding directory and the arducam node gets
# generated without the SPI device properties from spi-device.yaml.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../person_detection)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(finger_counter_capture)

target_sources(app PRIVATE
  src/main.c
  ../person_detection/src/drivers/arducam_mega.c
)

target_include_directories(app PRIVATE
  ../person_detection/src/drivers
)

# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

cmake_minimum_required(VERSION 3.20.0)

# Re-use the arducam_mega DT binding from person_detection. This must come
# before find_package(Zephyr) so the binding is picked up during DT pass.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../person_detection)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(finger_counter)

target_sources(app PRIVATE
  src/main.c
  ../person_detection/src/drivers/arducam_mega.c
)

target_include_directories(app PRIVATE
  ../person_detection/src/drivers
)

# Edge Impulse SDK glue (mirrors samples/edge_impulse/hello_ei).
zephyr_compile_definitions(
  EI_C_LINKAGE=1
  EIDSP_SIGNAL_C_FN_POINTER=1
)

zephyr_compile_options(
  -Wno-double-promotion
  -Wno-unused
  -Wno-sign-compare
  -Wno-maybe-uninitialized
)

string(CONFIGURE ${CONFIG_EDGE_IMPULSE_PATH} CONFIG_EDGE_IMPULSE_PATH)

if(NOT IS_ABSOLUTE ${CONFIG_EDGE_IMPULSE_PATH})
  set(CONFIG_EDGE_IMPULSE_PATH ${APPLICATION_SOURCE_DIR}/${CONFIG_EDGE_IMPULSE_PATH})
endif()

set(FETCH_CONTENT_NAME edge_impulse)

include(FetchContent)

FetchContent_Declare(
    ${FETCH_CONTENT_NAME}
    URL ${CONFIG_EDGE_IMPULSE_PATH}
)

FetchContent_MakeAvailable(${FETCH_CONTENT_NAME})

if(${CONFIG_NRF_AXON})
  zephyr_compile_definitions(
    NRF_AXON_MODEL_NAME=${CONFIG_NRF_AXON_MODEL_NAME}
  )

  zephyr_include_directories(${edge_impulse_SOURCE_DIR})
  zephyr_include_directories(${edge_impulse_SOURCE_DIR}/nordic-axon-model)
endif()

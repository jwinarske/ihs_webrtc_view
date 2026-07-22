# SPDX-FileCopyrightText: 2026 Joel Winarske
# SPDX-License-Identifier: MIT

# ihs_webrtc_view native presenter (.so).
#
# Speaks the flat C ABIs only: the producer side (third_party/lw_abi/
# lw_video_sink.h) and the ivi-homescreen platform-view ABI (third_party/ihs).
# MUST NOT include any C++ libwebrtc header.
#
# The heavy import (KMS plane / GL texture / SHM) is the registry's job: the
# presenter negotiates a surface and forwards dmabuf frames via ihs_pv_submit,
# so it links neither drm-cxx nor wayland. libihs_shared.so is resolved at run
# time (dlsym/dlopen), never linked -- only its headers are used at build time.
cmake_minimum_required(VERSION 3.16)
project(ihs_webrtc_view_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(Threads REQUIRED)

add_library(ihs_webrtc_view SHARED
  src/sink.cc           # Presenter: LwVideoSinkV1 consumer + ihs_pv producer
  src/tier_resolver.cc  # ihs bridge + platform-view factory + negotiation
)

target_include_directories(ihs_webrtc_view
  PUBLIC  include
  PRIVATE
    third_party/lw_abi  # C ABI only — no C++ libwebrtc headers
    third_party/ihs     # vendored ivi-homescreen platform-view headers
)

# libihs_shared.so is dlopen'd, not linked; only libdl + pthreads are needed.
target_link_libraries(ihs_webrtc_view PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

# Presenter check against a stand-in registry: the test binary exports
# ihs_get_api itself, so every call the presenter makes lands on a recording
# implementation. Needs -rdynamic so that dlsym(RTLD_DEFAULT, ...) finds it.
# No GPU, display or webrtc required.
enable_testing()
add_executable(presenter_test test/presenter_test.cc)
target_link_libraries(presenter_test PRIVATE ihs_webrtc_view Threads::Threads)
target_include_directories(presenter_test PRIVATE third_party/ihs third_party/lw_abi)
set_target_properties(presenter_test PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME presenter_test COMMAND presenter_test)

# Optional: drive the presenter with real hardware-decoded frames and report
# what reaches the platform-view interface. Needs a libwebrtc checkout and a
# built libwebrtc.so with lw_enable_v4l2_codec=true:
#   cmake -S . -B build -DLIBWEBRTC_DIR=... -DLIBWEBRTC_LIB=.../libwebrtc.so
if(LIBWEBRTC_DIR AND LIBWEBRTC_LIB)
  add_executable(presenter_live test/presenter_live.cc)
  target_link_libraries(presenter_live PRIVATE ihs_webrtc_view Threads::Threads
                        ${LIBWEBRTC_LIB})
  target_include_directories(presenter_live PRIVATE
    third_party/ihs third_party/lw_abi
    ${LIBWEBRTC_DIR}/include)
  # The library is built with desktop capture on, which adds virtual methods to
  # the public interfaces; a consumer must see the same define or its vtable
  # layout will not match.
  target_compile_definitions(presenter_live PRIVATE RTC_DESKTOP_DEVICE)
  set_target_properties(presenter_live PROPERTIES ENABLE_EXPORTS ON)
endif()

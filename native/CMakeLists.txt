# ihs_webrtc_view native presenter (.so).
#
# Speaks the flat C ABI only (third_party/lw_abi/lw_video_sink.h). MUST NOT
# include any C++ libwebrtc header. Links drm-cxx + wayland + ihs_shared.
cmake_minimum_required(VERSION 3.16)
project(ihs_webrtc_view_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(ihs_webrtc_view SHARED
  src/sink.cc             # LwVideoSinkV1 impl + SPSC handoff
  src/tier_resolver.cc    # attach-time resolver (re-entrant)
  src/tiers/plane.cc      # direct plane
  src/tiers/subsurface.cc # subsurface promotion
  src/tiers/gpu_import.cc # GPU-import floor
)

target_include_directories(ihs_webrtc_view
  PUBLIC  include
  PRIVATE third_party/lw_abi          # C ABI only — no C++ libwebrtc headers
)

# TODO: find_package/link drm-cxx, wayland, ihs_shared.
# target_link_libraries(ihs_webrtc_view PRIVATE drm-cxx wayland-client ihs_shared)

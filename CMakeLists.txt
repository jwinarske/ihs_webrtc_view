# SPDX-FileCopyrightText: 2026 Joel Winarske
# SPDX-License-Identifier: MIT

# Top-level entry for the native presenter cross-build. emb (and any plain
# CMake invocation) uses the repo root as the source directory; the presenter
# itself lives under native/. This just forwards to it.
cmake_minimum_required(VERSION 3.16)
project(ihs_webrtc_view_root LANGUAGES CXX)

enable_testing()
add_subdirectory(native)

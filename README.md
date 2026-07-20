<!--
SPDX-FileCopyrightText: 2026 Joel Winarske
SPDX-License-Identifier: MIT
-->

# ihs_webrtc_view

The FFI plugin: a **native presenter** plus a **Flutter package**, delivering
zero-copy WebRTC video into ivi-homescreen (and any Flutter embedder) as a
platform view.

## Two artifacts

- **`native/`** — the presenter `.so`. Implements `LwVideoSinkV1` (the data
  plane), the attach-time **tier resolver**, an SPSC handoff from the decoder
  delivery thread to the present thread, and the presentation tiers: direct
  **plane**, **subsurface** promotion, and **GPU import** floor. Links drm-cxx
  + wayland + ihs_shared.
- **`lib/`** — the Flutter package: the platform-view widget, sink-token
  plumbing, and the `track.bindSink(token)` glue. Depends on `rtc_dart`.

## Hard boundary

The native half **never includes C++ libwebrtc headers** — it speaks the flat
C ABI only (`lw_video_sink.h`, vendored under `native/third_party/lw_abi/`).
Presenter and `rtc_dart` dependency sets are disjoint by construction, which
keeps the headless (no-presenter) path and the service-bundle shape intact.

## Tier resolution

The plugin is backend-agnostic; only the achievable tier differs. The
attach-time resolver keys on backend kind, plane availability (DRM), lease
contents (leased-DRM), and dmabuf-feedback tranches + driver import extensions
(Wayland). It is **re-entrant** — lease revocation tears down and re-resolves.

| Backend | Best tier | Fallback floor |
|---|---|---|
| DRM (EGL/Vulkan/SW) | direct plane | GPU import (none for SW renderer) |
| wayland-leased-drm | direct plane *iff leased* | GPU import |
| Wayland EGL | subsurface promotion | EGL dmabuf import |
| Wayland Vulkan | subsurface promotion | `VK_EXT_image_drm_format_modifier` |
| Wayland software | subsurface promotion | none (debug readback only) |

## Binding handshake

platform-view create -> presenter registers sink -> token surfaced through the
ihs_shared registry / platform-view params -> Dart `track.bindSink(token)` ->
`lw_video_track_bind_sink` -> frames flow native-to-native. Unbind on view
dispose (quiescing).

## Layout

```
native/CMakeLists.txt
native/include/ihs_webrtc_view/            presenter public headers
native/src/sink.cc                          LwVideoSinkV1 impl + SPSC
native/src/tier_resolver.cc                 attach-time resolver (re-entrant)
native/src/tiers/plane.cc                   direct plane: matching alloc, hole punch, OUT_FENCE
native/src/tiers/subsurface.cc              subsurface + dmabuf promotion
native/src/tiers/gpu_import.cc              floor: VK_EXT_image_drm_format_modifier / EGL dmabuf
native/third_party/lw_abi/lw_video_sink.h   vendored C ABI (C-only boundary)
lib/ihs_webrtc_view.dart                    platform-view widget + bindSink glue
example/                                     receive + data-channel demo
```

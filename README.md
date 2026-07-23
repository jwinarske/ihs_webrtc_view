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
  plane), a handoff from the decoder delivery thread to a present thread that
  paces on the frames' timestamps, and the buffer accounting that decides how
  many decoder buffers it may hold. Speaks `ihs_shared` for the surface.
- **`lib/`** — the Flutter package: the platform-view widget, sink-token
  plumbing, and the `track.bindSink(token)` glue. Depends on `rtc_dart`.

## Hard boundary

The native half **never includes C++ libwebrtc headers** — it speaks the flat
C ABI only (`lw_video_sink.h`, vendored under `native/third_party/lw_abi/`).
Presenter and `rtc_dart` dependency sets are disjoint by construction, which
keeps the headless (no-presenter) path and the service-bundle shape intact.

## Surface negotiation

The presenter states what it can accept and the registry grants one of them;
it does not resolve a presentation path itself. At view creation it asks for a
DRM plane or the software floor, linear NV12, below Flutter, preferring
explicit sync. Requesting the floor guarantees a grant, so there is always a
path. `submit` is uniform across kinds, so nothing is rewired per kind.

The grant's sync mode is what matters downstream, because it decides how the
presenter releases buffers:

| Grant | How buffers are retired | Held at 30fps |
|---|---|---|
| explicit | as each release fence signals | 0–1 |
| implicit | a depth window, since there is nothing to wait on | `min(pool/3, 8)` |

The window follows the pool size the producer advertises in the frame
descriptor: a VAAPI pool of 28 surfaces and a V4L2 pool of 9 cannot share one
fixed depth without either wasting the first or starving the second.

A richer resolver — keying on lease contents, dmabuf-feedback tranches and
driver import extensions, with subsurface promotion and a GPU-import floor —
is a design, not code. There is no `native/src/tiers/`.

## Binding handshake

platform-view create -> presenter registers sink -> token surfaced through the
ihs_shared registry / platform-view params -> Dart `track.bindSink(token)` ->
`lw_video_track_bind_sink` -> frames flow native-to-native. Unbind on view
dispose (quiescing).

## Tests

```sh
cmake -S native -B build && cmake --build build
ctest --test-dir build
```

`presenter_test` runs anywhere — no GPU, no display, no webrtc. The test binary
exports `ihs_get_api` itself, so the presenter's `dlsym` resolves to a
recording stand-in and every call it makes is observed. It covers what the
presenter is responsible for: frames reaching submit intact, every taken frame
released exactly once, retirement driven by release fences under an explicit
grant, the depth window following the advertised pool size, and a resolution
change retiring the old pool before the new one is presented — which matters
because the kernel hands back the same fd numbers, every one of them.

`presenter_live` drives real decoded frames and needs a GPU and a libwebrtc
build, so it is built only when `LIBWEBRTC_DIR` and `LIBWEBRTC_LIB` are given.

## Layout

```
native/CMakeLists.txt
native/include/ihs_webrtc_view/            presenter public headers
native/src/sink.cc                          LwVideoSinkV1 impl, present thread
native/src/tier_resolver.cc                 ihs_shared bridge, view factory,
                                            surface negotiation, C entry points
native/test/presenter_test.cc               GPU-free presenter checks
native/test/presenter_live.cc               real frames; needs a GPU
native/third_party/lw_abi/lw_video_sink.h   vendored C ABI (C-only boundary)
lib/ihs_webrtc_view.dart                    platform-view widget + bindSink glue
example/                                     receive + data-channel demo
```

There is no CI in this repository yet; the checks above are run by hand.

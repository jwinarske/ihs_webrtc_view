<!-- SPDX-FileCopyrightText: 2026 Joel Winarske -->
<!-- SPDX-License-Identifier: MIT -->

# ihs_webrtc_view example

Shows a decoded WebRTC H.264 video track in a `WebRtcView` on ivi-homescreen,
zero-copy end to end: peer → libwebrtc → V4L2 hardware decode → dma-buf →
native presenter → `ihs_pv_submit` → the platform-view registry composites it
(a DRM overlay plane or a dma-buf-imported texture, whichever the backend
grants).

The control plane is native: `native/webrtc_session.c` (built as
`libwebrtc_session.so`) does the connect/answer/ICE on its own thread and
exposes a polled track-handle getter, so Dart never services a native-thread
callback — `lib/main.dart` polls `webrtc_session_track()` and mounts the view
when the track appears. Frames never touch Dart.

## Peer

Any WebRTC peer that offers H.264 over the example's framed-TCP signaling. The
repo's development peer is a small aiortc sender (offer/answer/candidate over a
length-framed TCP socket). Point the app at it with:

    flutter run --dart-define=SIGNALING_HOST=<peer-ip> --dart-define=SIGNALING_PORT=9300

## Running on a Raspberry Pi against ivi-homescreen

Validated on a Pi 4 (bcm2835-codec, `/dev/video10`) with the
`jw/drm-pv-direct-scanout` homescreen driving a connected display. Outline:

1. Cross-build the AOT bundle for arm64:

       emb bundle --app-path . --arch arm64 --build

2. Cross-build the native presenter (`libihs_webrtc_view.so`) and, on the Pi,
   the session lib against the target libwebrtc:

       emb cross .. --target rpi4-trixie --build          # presenter .so
       gcc -shared -fPIC native/webrtc_session.c -lwebrtc -o libwebrtc_session.so

3. Assemble a bundle from the homescreen (embedder + engine + `libihs_shared`)
   plus the AOT `libapp.so`/`data`, and drop the native libs beside them:
   `libwebrtc.so`, `libihs_webrtc_view.so`, `libwebrtc_session.so`.

4. Run with hardware decode enabled:

       cd <bundle> && LW_V4L2=1 LD_LIBRARY_PATH=$PWD/lib ./homescreen --bundle $PWD

`LW_V4L2=1` selects the in-tree V4L2 hardware decoder in libwebrtc; without it
the software decoder is used.

> A single `emb`-integrated native-assets flow (auto-building the session lib
> and staging `libwebrtc.so`) is future work; the assembly above is manual for
> now.

<!-- SPDX-FileCopyrightText: 2026 Joel Winarske -->
<!-- SPDX-License-Identifier: MIT -->

# ihs_webrtc_view example

A decoded WebRTC H.264 track in a `WebRtcView` on ivi-homescreen, zero-copy end
to end: peer → libwebrtc → hardware decode → dma-buf → native presenter →
`ihs_pv_submit` → the platform-view registry imports and composites it.

The control plane is native: `native/webrtc_session.c` (built as
`libwebrtc_session.so`) does the connect/answer/ICE on its own thread and
exposes a polled track-handle getter, so Dart never services a native-thread
callback — `lib/main.dart` polls `webrtc_session_track()` and mounts the view
when the track appears. Frames never touch Dart.

## What it needs from the shell

A homescreen whose platform-view registry can import planar YUV. The registry
samples a dma-buf through `GL_TEXTURE_EXTERNAL_OES` only on the EGL backends,
and only since toyota-connected/ivi-homescreen#335; before that a YUV frame
rendered as a red luma ramp, and on a Vulkan backend it still does, because
that importer needs a `VkSamplerYcbcrConversion`.

So: **build the shell for `drm-kms-egl` or `wayland-egl`**, not Vulkan.

## Peer

`peer/h264_peer.c` — a sender built on the same flat C ABI as everything else,
so it needs no second WebRTC stack. It listens on a TCP port, offers H.264 to
the first client, and pushes a moving test pattern:

```sh
gcc -I<libwebrtc>/include peer/h264_peer.c -lwebrtc -lpthread -o h264_peer
./h264_peer 9300
```

The pattern is a diagonal luma ramp with neutral chroma, so it renders
**greyscale**. Colour means the chroma plane is being sampled wrong — that is
what the red image looked like before #335, and it is a useful thing to be able
to tell at a glance.

The signalling is a framed TCP socket: `OFFER <len>\n<sdp>`,
`ANSWER <len>\n<sdp>`, `CAND <mline> <len>\n<candidate>`. Any peer speaking
that and offering H.264 will do.

`SIGNALING_HOST`/`SIGNALING_PORT` are compile-time
(`String.fromEnvironment`), so they are baked when the assets are built:

```sh
flutter build bundle --dart-define=SIGNALING_HOST=<peer-ip> \
                     --dart-define=SIGNALING_PORT=9300
```

## Running on a Raspberry Pi 4

Verified on a Pi 4 (`bcm2835-codec`, `/dev/video10`) driving a connected
display over `drm-kms-egl`.

1. Cross-build the shell and deploy an arm64 bundle. From the ivi-homescreen
   checkout — note the package dir is positional and `--target` finds the board
   file under `.emb/`:

   ```sh
   emb cross . --target rpi4-trixie --backend drm-kms-egl --build \
       --app /path/to/ihs_webrtc_view/example -m debug \
       --deploy joel@raspberrypi.local --deploy-dir ihs-webrtc
   ```

2. Cross-build the presenter and the session lib for arm64 and copy them, plus
   the target `libwebrtc.so`, into the bundle's `lib/`.

3. Build the peer **on the Pi**, not on the host. `libwebrtc.so` references
   X11 symbols (`XRRQueryVersion`, `XComposite*`, `XDamage*`) that a Raspberry
   Pi OS Lite sysroot does not carry. Undefined symbols are fine in a shared
   library and fatal when linking an executable, so the peer cross-links only
   against a sysroot with X11 development packages. Building it natively
   sidesteps that.

4. Run the peer, then the shell:

   ```sh
   cd ~/ihs-webrtc
   LD_LIBRARY_PATH=$PWD/lib ./h264_peer 9300 &
   LW_V4L2=1 LD_LIBRARY_PATH=$PWD/lib \
       ./homescreen -b $PWD --backend drm-kms-egl -w 640 --height 480
   ```

`LW_V4L2=1` is required, not optional. It selects the in-tree V4L2 hardware
decoder; without it libwebrtc uses its builtin software decoder, which produces
no dma-buf, so the native sink is never called and the view stays black. A
black view with the track mounted almost always means this.

`-w`/`--height` size the view within the display.

## Reading a failure

| symptom | cause |
|---|---|
| `session start failed (-5)` | `connect()` failed — no peer listening on the port |
| view mounts, stays black | no native frames: `LW_V4L2=1` missing, or the sink never bound |
| red diagonal ramp | planar YUV sampled as RGBA — shell predates #335, or is a Vulkan backend |
| `EglDmabufImporter ... 0x3003` | `EGL_BAD_ALLOC` on import; the plane fds were closed under the decoder |

## Not automated

The bundle assembly in step 2 is manual. An `emb`-integrated native-assets flow
that builds the session lib and stages `libwebrtc.so` alongside the app would
remove it.

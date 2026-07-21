// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// A reference to a decoded WebRTC video track, as a raw libwebrtc handle.
///
/// Interim shim until rtc_dart exposes an idiomatic `VideoTrack`: the app gets
/// a `lw_video_track*` from rtc_dart on OnTrack and wraps its address here to
/// hand to [WebRtcView]. No frame data is involved — this is a control-plane
/// handle only.
library;

import 'dart:ffi' as ffi;

import 'package:rtc_dart/rtc_dart_ffi.dart' show lw_video_track;

class WebRtcVideoTrack {
  /// Wraps the address of a libwebrtc `lw_video_track*` (from
  /// `lw_receiver_video_track`). Must be non-zero and outlive the [WebRtcView].
  const WebRtcVideoTrack.fromAddress(this.address)
      : assert(address != 0, 'track handle must be non-null');

  final int address;

  ffi.Pointer<lw_video_track> get pointer =>
      ffi.Pointer<lw_video_track>.fromAddress(address);
}

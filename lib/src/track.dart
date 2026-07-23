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

import 'package:ffi/ffi.dart';
import 'package:rtc_dart/rtc_dart_ffi.dart'
    show LwBindings, lw_video_track, openLibwebrtc;

class WebRtcVideoTrack {
  /// Resolves the track another plugin published under [id].
  ///
  /// Where flutter_webrtc (or anything else) owns the peer connection, it
  /// surfaces each track's id to its own Dart API; that id resolves here, so
  /// only the string crosses between the two and neither needs to know the
  /// other's pointers.
  ///
  /// Returns null when no live track carries that id. The handle is owned by
  /// this object -- unlike [WebRtcVideoTrack.fromAddress], which borrows one --
  /// so [dispose] must be called, and before the plugin that owns the track
  /// tears its factory down.
  static WebRtcVideoTrack? byId(String id, {LwBindings? lw}) {
    if (id.isEmpty) {
      return null;
    }
    final bindings = lw ?? openLibwebrtc();
    final name = id.toNativeUtf8();
    try {
      final found = bindings.lw_video_track_find(name.cast());
      if (found == ffi.nullptr) {
        return null;
      }
      return WebRtcVideoTrack._owned(found.address, bindings);
    } finally {
      malloc.free(name);
    }
  }

  WebRtcVideoTrack._owned(this.address, LwBindings bindings)
      : _owner = bindings;

  /// Non-null only when this object created the handle and must release it.
  final LwBindings? _owner;

  /// Releases the handle, if this object owns one. Idempotent; a no-op for a
  /// borrowed handle from [WebRtcVideoTrack.fromAddress].
  void dispose() {
    final owner = _owner;
    if (owner != null && !_disposed) {
      _disposed = true;
      owner.lw_release(pointer.cast());
    }
  }

  bool _disposed = false;

  /// Wraps the address of a libwebrtc `lw_video_track*` (from
  /// `lw_receiver_video_track`). Must be non-zero and outlive the [WebRtcView].
  WebRtcVideoTrack.fromAddress(this.address)
      : _owner = null,
        assert(address != 0, 'track handle must be non-null');

  final int address;

  ffi.Pointer<lw_video_track> get pointer =>
      ffi.Pointer<lw_video_track>.fromAddress(address);
}

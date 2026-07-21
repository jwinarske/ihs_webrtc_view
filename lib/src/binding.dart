// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// The control-plane binding: connect a platform view's native presenter sink
/// to a decoded WebRTC video track so frames flow native-to-native.
///
/// Flow (all handles are process-global pointers; no frame data touches Dart):
///   1. The view's presenter (libihs_webrtc_view.so) exposes an LwVideoSinkV1.
///   2. Register it with libwebrtc's sink registry -> an unguessable token.
///   3. Bind the token to the track -> the decoder delivers dmabuf frames to
///      the presenter, which submits them to the ivi-homescreen registry.
/// Reverse on detach: unbind the track, then unregister the token.
library;

import 'dart:ffi' as ffi;

import 'package:rtc_dart/rtc_dart_ffi.dart'
    show LwBindings, lw_video_track, openLibwebrtc;

import 'ffi/presenter_ffi.dart';

/// One live binding between a platform view and a video track. Create with
/// [attach]; release exactly once with [detach].
class WebRtcViewBinding {
  WebRtcViewBinding._(this._lw, this._track, this._token);

  final LwBindings _lw;
  final ffi.Pointer<lw_video_track> _track;
  final int _token;
  bool _detached = false;

  /// Binds the sink of the created view [viewId] to [track] (a libwebrtc
  /// `lw_video_track*` obtained from the receiver on OnTrack). Returns null if
  /// the view's presenter is not ready or registration/binding fails.
  ///
  /// [presenter]/[lw] default to the process-wide instances; inject for tests.
  static WebRtcViewBinding? attach(
    int viewId,
    ffi.Pointer<lw_video_track> track, {
    PresenterFfi? presenter,
    LwBindings? lw,
  }) {
    final pres = presenter ?? PresenterFfi.instance();
    final bindings = lw ?? openLibwebrtc();

    final viewSink = pres.sinkForView(viewId);
    if (viewSink == null) {
      return null; // view not created yet
    }
    final token = bindings.lw_video_sink_register(viewSink.sink, viewSink.user);
    if (token == 0) {
      return null; // registry rejected the sink
    }
    if (bindings.lw_video_track_bind_sink(track, token) != 0) {
      bindings.lw_video_sink_unregister(token);
      return null; // bind failed (unknown track/token)
    }
    return WebRtcViewBinding._(bindings, track, token);
  }

  /// Unbinds the track (quiesces the sink, emits on_eos) and unregisters the
  /// token. Safe to call once; further calls are no-ops.
  void detach() {
    if (_detached) {
      return;
    }
    _detached = true;
    _lw.lw_video_track_unbind_sink(_track);
    _lw.lw_video_sink_unregister(_token);
  }
}

// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// The Flutter platform-view widget. Publishes an ihs_webrtc_view platform view
/// and, once the embedder assigns it an id, binds the view's native presenter
/// sink to the supplied track so decoded frames flow native-to-native. The
/// widget itself renders only a hit-test surface; pixels are composited by the
/// ivi-homescreen registry (DRM plane / dmabuf import / software floor).
library;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'binding.dart';
import 'track.dart';

/// The platform-view type string the native factory is registered for
/// (ihs_webrtc_view_register). Must match on both sides.
const String kWebRtcViewType = 'ihs_webrtc_view';

/// Displays a decoded WebRTC [track] as a composited platform view.
class WebRtcView extends StatefulWidget {
  /// Binds [track], a handle the caller already holds and continues to own.
  const WebRtcView({
    super.key,
    required WebRtcVideoTrack this.track,
    this.hitTestBehavior = PlatformViewHitTestBehavior.opaque,
    this.gestureRecognizers = const <Factory<OneSequenceGestureRecognizer>>{},
  }) : trackId = null;

  /// Resolves [trackId] and binds what it finds.
  ///
  /// For a track another plugin owns: flutter_webrtc surfaces the id of each
  /// track it receives, and that id resolves here. The handle is resolved when
  /// the view is created and released when it is disposed, so nothing outlives
  /// the view.
  const WebRtcView.byTrackId({
    super.key,
    required String this.trackId,
    this.hitTestBehavior = PlatformViewHitTestBehavior.opaque,
    this.gestureRecognizers = const <Factory<OneSequenceGestureRecognizer>>{},
  }) : track = null;

  /// The decoded video track to bind once the view is created, when given
  /// directly. Null when [trackId] is used instead.
  final WebRtcVideoTrack? track;

  /// The id to resolve at view creation. Null when [track] is given.
  final String? trackId;
  final PlatformViewHitTestBehavior hitTestBehavior;
  final Set<Factory<OneSequenceGestureRecognizer>> gestureRecognizers;

  @override
  State<WebRtcView> createState() => _WebRtcViewState();
}

class _WebRtcViewState extends State<WebRtcView>
    with SingleTickerProviderStateMixin {
  // The registry updates the view's imported texture per ihs_pv_submit, but a
  // composited (hybrid) platform view is only re-drawn when Flutter produces a
  // frame. A retained-mode UI goes idle after mount, freezing the video on its
  // first frame. An always-running ticker keeps a frame scheduled every vsync
  // so the view re-composites with the newest decoded frame.
  late final AnimationController _repaint = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 1),
  )..repeat();

  @override
  void dispose() {
    _repaint.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return PlatformViewLink(
      viewType: kWebRtcViewType,
      surfaceFactory: (context, controller) {
        // Build the surface once and hand it to AnimatedBuilder as `child`, so
        // the ticker still schedules a frame each vsync (re-compositing the view
        // with the newest decoded frame) without reconstructing the widget.
        final surface = PlatformViewSurface(
          controller: controller,
          hitTestBehavior: widget.hitTestBehavior,
          gestureRecognizers: widget.gestureRecognizers,
        );
        return AnimatedBuilder(
          animation: _repaint,
          child: surface,
          builder: (_, child) => child!,
        );
      },
      onCreatePlatformView: (params) {
        final controller = _WebRtcViewController(
          viewId: params.id,
          track: widget.track,
          trackId: widget.trackId,
          onCreated: params.onPlatformViewCreated,
        );
        controller._create();
        return controller;
      },
    );
  }
}

/// Drives the flutter/platform_views channel and owns the track binding for one
/// view. The embedder routes `create` to the native factory when a factory is
/// registered for the view type.
class _WebRtcViewController extends PlatformViewController {
  _WebRtcViewController({
    required int viewId,
    required WebRtcVideoTrack? track,
    required String? trackId,
    required PlatformViewCreatedCallback onCreated,
  })  : _viewId = viewId,
        _given = track,
        _trackId = trackId,
        _onCreated = onCreated;

  static const MethodChannel _channel =
      MethodChannel('flutter/platform_views', StandardMethodCodec());

  final int _viewId;

  /// Handed in by the caller, who keeps owning it. Null when [_trackId] is
  /// used instead.
  final WebRtcVideoTrack? _given;

  /// Resolved at create time; owned here and released on dispose.
  final String? _trackId;
  WebRtcVideoTrack? _resolved;
  final PlatformViewCreatedCallback _onCreated;

  WebRtcViewBinding? _binding;
  bool _created = false;
  bool _disposed = false;

  @override
  int get viewId => _viewId;

  Future<void> _create() async {
    if (_created || _disposed) {
      return;
    }
    await _channel.invokeMethod<dynamic>('create', <String, dynamic>{
      'id': _viewId,
      'viewType': kWebRtcViewType,
      'direction': 0, // AxisDirection.down / LTR default
      'hybrid': true,
    });
    if (_disposed) {
      // Disposed mid-create; tear the just-created view back down.
      await _channel.invokeMethod<dynamic>('dispose', <String, dynamic>{
        'id': _viewId,
        'hybrid': true,
      });
      return;
    }
    _created = true;
    // Bind the native presenter sink to the track now that the view exists.
    // A track named by id is resolved now, not at construction: the far side
    // may not have announced it when the widget was built.
    final id = _trackId;
    if (id != null) {
      _resolved = WebRtcVideoTrack.byId(id);
      if (_resolved == null) {
        // Nothing carries that id. The view stays up and blank rather than
        // throwing from a platform-view callback.
        _onCreated(_viewId);
        return;
      }
    }
    final track = _given ?? _resolved!;
    _binding = WebRtcViewBinding.attach(_viewId, track.pointer);
    _onCreated(_viewId);
  }

  @override
  Future<void> clearFocus() async {
    if (!_created) {
      return;
    }
    await _channel.invokeMethod<void>('clearFocus', _viewId);
  }

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {
    // Gesture forwarding to the native view is not wired yet; a video surface
    // needs none for playback. Left as a no-op until touch routing lands.
  }

  @override
  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _binding?.detach(); // unbind track + unregister sink before the view goes
    _binding = null;
    // Only what this controller resolved; a handed-in track stays the
    // caller's.
    _resolved?.dispose();
    _resolved = null;
    if (_created) {
      await _channel.invokeMethod<void>('dispose', <String, dynamic>{
        'id': _viewId,
        'hybrid': true,
      });
    }
  }
}

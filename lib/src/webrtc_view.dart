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
  const WebRtcView({
    super.key,
    required this.track,
    this.hitTestBehavior = PlatformViewHitTestBehavior.opaque,
    this.gestureRecognizers = const <Factory<OneSequenceGestureRecognizer>>{},
  });

  /// The decoded video track to bind once the view is created.
  final WebRtcVideoTrack track;
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
      surfaceFactory: (context, controller) => AnimatedBuilder(
        animation: _repaint,
        builder: (_, __) => PlatformViewSurface(
          controller: controller,
          hitTestBehavior: widget.hitTestBehavior,
          gestureRecognizers: widget.gestureRecognizers,
        ),
      ),
      onCreatePlatformView: (params) {
        final controller = _WebRtcViewController(
          viewId: params.id,
          track: widget.track,
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
    required WebRtcVideoTrack track,
    required PlatformViewCreatedCallback onCreated,
  })  : _viewId = viewId,
        _track = track,
        _onCreated = onCreated;

  static const MethodChannel _channel =
      MethodChannel('flutter/platform_views', StandardMethodCodec());

  final int _viewId;
  final WebRtcVideoTrack _track;
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
    _binding = WebRtcViewBinding.attach(_viewId, _track.pointer);
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
    if (_created) {
      await _channel.invokeMethod<void>('dispose', <String, dynamic>{
        'id': _viewId,
        'hybrid': true,
      });
    }
  }
}

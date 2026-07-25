// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// The Flutter platform-view widget. Publishes an ihs_webrtc_view platform view
/// and, once the embedder assigns it an id, binds the view's native presenter
/// sink to the supplied track so decoded frames flow native-to-native. The
/// widget itself renders only a hit-test surface; pixels are composited by the
/// ivi-homescreen registry (DRM plane / dmabuf import / software floor).
library;

import 'dart:convert';

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
    this.onInput,
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
    this.onInput,
  }) : track = null;

  /// The decoded video track to bind once the view is created, when given
  /// directly. Null when [trackId] is used instead.
  final WebRtcVideoTrack? track;

  /// The id to resolve at view creation. Null when [track] is given.
  final String? trackId;
  final PlatformViewHitTestBehavior hitTestBehavior;
  final Set<Factory<OneSequenceGestureRecognizer>> gestureRecognizers;

  /// Optional sink for pointer input to forward to the far peer. When set, each
  /// pointer event on the view is serialized to a small JSON message and handed
  /// here for the app to send over its own transport (e.g. a WebRTC data
  /// channel). The message is UTF-8 `{"action":<int>,"pointers":[{"id":<int>,
  /// "x":<double>,"y":<double>}],"ts":<int-ms>}`, where `action` is
  /// 0=down/1=up/2=move/3=cancel and coordinates are view-local logical pixels.
  /// The view has no data channel of its own; forwarding is the app's to wire.
  /// This is independent of the native on_touch routing the embedder does.
  final void Function(Uint8List message)? onInput;

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
          onInput: widget.onInput,
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
    required void Function(Uint8List message)? onInput,
  })  : _viewId = viewId,
        _given = track,
        _trackId = trackId,
        _onCreated = onCreated,
        _onInput = onInput;

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
  final void Function(Uint8List message)? _onInput;

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

  // Android MotionEvent actions the embedder's PlatformViewTouch decodes. The
  // flutter/platform_views 'touch' contract is the Android motion-event shape;
  // the embedder reads the action and pointer coordinates from it and hands them
  // to the view's native on_touch callback.
  static const int _actionDown = 0;
  static const int _actionUp = 1;
  static const int _actionMove = 2;
  static const int _actionCancel = 3;

  static int? _androidAction(PointerEvent event) {
    if (event is PointerDownEvent) {
      return _actionDown;
    }
    if (event is PointerMoveEvent) {
      return _actionMove;
    }
    if (event is PointerUpEvent) {
      return _actionUp;
    }
    if (event is PointerCancelEvent) {
      return _actionCancel;
    }
    return null; // hover/scroll/etc. — nothing the native view consumes
  }

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {
    if (!_created || _disposed) {
      return;
    }
    final action = _androidAction(event);
    if (action == null) {
      return;
    }
    // One contact, coordinates in view-local logical pixels. The coords row is
    // the Android PointerCoords layout: x at index 7, y at index 8 (the rest are
    // orientation/pressure/size/tool/touch axes the video view does not use).
    final x = event.localPosition.dx;
    final y = event.localPosition.dy;
    final params = <Object?>[
      _viewId, // 0: view id
      0, // 1: downTime (unused downstream)
      event.timeStamp.inMilliseconds, // 2: eventTime
      action, // 3: action
      1, // 4: pointer count
      <List<int>>[
        <int>[0, 1], // 5: pointerProperties: [pointerId, TOOL_TYPE_FINGER]
      ],
      <List<double>>[
        <double>[0, 1, 1, 0, 0, 0, 0, x, y], // 6: pointerCoords (x@7, y@8)
      ],
      0, // 7: metaState
      0, // 8: buttonState
      1.0, // 9: xPrecision
      1.0, // 10: yPrecision
      0, // 11: deviceId
      0, // 12: edgeFlags
      0, // 13: source
      0, // 14: flags
      0, // 15: motionEventId
    ];
    await _channel.invokeMethod<void>('touch', params);

    // Forward the same event to the app's input sink, if any, for the app to
    // send to the far peer over its own transport. Independent of the native
    // routing above.
    final onInput = _onInput;
    if (onInput != null) {
      final message = jsonEncode(<String, Object?>{
        'action': action,
        'pointers': <Object?>[
          <String, Object?>{'id': event.pointer, 'x': x, 'y': y},
        ],
        'ts': event.timeStamp.inMilliseconds,
      });
      onInput(Uint8List.fromList(utf8.encode(message)));
    }
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

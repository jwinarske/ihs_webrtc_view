// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// Example: show a decoded WebRTC video track in a WebRtcView on ivi-homescreen.
//
// Control plane stays native (libwebrtc_session.so does the connect/answer/ICE
// on its own thread); Dart polls webrtc_session_track() for the decoded track
// handle and, once it appears, mounts a WebRtcView bound to it. Frames never
// touch Dart — they flow decoder -> presenter -> platform-view registry (a DRM
// overlay plane on this target).
import 'dart:async';
import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';
import 'package:ihs_webrtc_view/ihs_webrtc_view.dart';

/// FFI to the native receive session (libwebrtc_session.so).
class _Session {
  _Session() : _lib = ffi.DynamicLibrary.open('libwebrtc_session.so') {
    _start = _lib.lookupFunction<
        ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Int32),
        int Function(ffi.Pointer<Utf8>, int)>('webrtc_session_start');
    _track = _lib.lookupFunction<ffi.IntPtr Function(), int Function()>(
        'webrtc_session_track');
    _trackId = _lib.lookupFunction<ffi.Pointer<Utf8> Function(),
        ffi.Pointer<Utf8> Function()>('webrtc_session_track_id');
    _iceState = _lib.lookupFunction<ffi.Int32 Function(), int Function()>(
        'webrtc_session_ice_state');
  }

  final ffi.DynamicLibrary _lib;
  late final int Function(ffi.Pointer<Utf8>, int) _start;
  late final int Function() _track;
  late final ffi.Pointer<Utf8> Function() _trackId;
  late final int Function() _iceState;

  int start(String host, int port) {
    final h = host.toNativeUtf8();
    try {
      return _start(h, port);
    } finally {
      malloc.free(h);
    }
  }

  int trackAddress() => _track();

  /// The id webrtc gave the remote track, or null before it arrives. This is
  /// what a view resolves when the track belongs to another plugin.
  String? trackIdOrNull() {
    final p = _trackId();
    if (p == ffi.nullptr) {
      return null;
    }
    final s = p.toDartString();
    calloc.free(p.cast());
    return s;
  }

  int iceState() => _iceState();
}

// The x86 aiortc peer's framed-TCP signaling endpoint. Override via
// --dart-define=SIGNALING_HOST=... / SIGNALING_PORT=...
const String kSignalingHost =
    String.fromEnvironment('SIGNALING_HOST', defaultValue: '192.168.45.151');
const int kSignalingPort =
    int.fromEnvironment('SIGNALING_PORT', defaultValue: 9300);

void main() {
  // Install the native platform-view factory before any WebRtcView is built.
  final ok = initIhsWebRtcView();
  // ignore: avoid_print
  print('[app] initIhsWebRtcView=$ok');
  runApp(const ExampleApp());
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      debugShowCheckedModeBanner: false,
      home: VideoPage(),
    );
  }
}

class VideoPage extends StatefulWidget {
  const VideoPage({super.key});

  @override
  State<VideoPage> createState() => _VideoPageState();
}

class _VideoPageState extends State<VideoPage> {
  final _Session _session = _Session();
  Timer? _poll;
  String? _trackIdValue;
  String _status = 'starting session…';

  @override
  void initState() {
    super.initState();
    final rc = _session.start(kSignalingHost, kSignalingPort);
    if (rc != 0) {
      setState(() => _status = 'session start failed ($rc)');
      return;
    }
    setState(() => _status = 'connecting to $kSignalingHost:$kSignalingPort…');
    // Poll for the decoded track; once it appears, mount the view and stop.
    _poll = Timer.periodic(const Duration(milliseconds: 200), (_) {
      final trackId = _session.trackIdOrNull();
      if (trackId != null) {
        _poll?.cancel();
        // ignore: avoid_print
        print('[app] track ready id="$trackId"; mounting WebRtcView by id');
        setState(() {
          _trackIdValue = trackId;
          _status = 'track bound';
        });
      }
    });
  }

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final trackId = _trackIdValue;
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        fit: StackFit.expand,
        children: [
          if (trackId != null)
            // Resolved by id: the same route a consumer takes when another
            // plugin owns the peer connection and only surfaces the id.
            WebRtcView.byTrackId(trackId: trackId)
          else
            Center(
              child: Text(
                _status,
                style: const TextStyle(color: Colors.white70, fontSize: 20),
              ),
            ),
        ],
      ),
    );
  }
}

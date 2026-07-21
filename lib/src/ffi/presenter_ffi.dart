// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// FFI to the native presenter (libihs_webrtc_view.so): install the
/// platform-view factory, and fetch a created view's LwVideoSinkV1 so the
/// control plane can bind it to a decoded track.
///
/// Only the two flat C entry points are used; the presenter resolves
/// libihs_shared.so itself at run time. See native/src/tier_resolver.cc.
library;

import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';
import 'package:rtc_dart/rtc_dart_ffi.dart' show LwVideoSinkV1;

typedef _RegisterNative = ffi.Int32 Function(ffi.Pointer<Utf8>);
typedef _RegisterDart = int Function(ffi.Pointer<Utf8>);

typedef _SinkForViewNative = ffi.Pointer<LwVideoSinkV1> Function(
    ffi.Int32, ffi.Pointer<ffi.Pointer<ffi.Void>>);
typedef _SinkForViewDart = ffi.Pointer<LwVideoSinkV1> Function(
    int, ffi.Pointer<ffi.Pointer<ffi.Void>>);

/// The presenter's `.so` name. The Flutter plugin build bundles it; the dynamic
/// linker resolves it by SONAME. Override for tests.
const String kPresenterLibName = 'libihs_webrtc_view.so';

/// A resolved view sink: the LwVideoSinkV1 pointer plus its `user` cookie, both
/// passed verbatim to lw_video_sink_register.
class ViewSink {
  const ViewSink(this.sink, this.user);
  final ffi.Pointer<LwVideoSinkV1> sink;
  final ffi.Pointer<ffi.Void> user;
}

/// Thin binding to libihs_webrtc_view.so. Construct once (cached by [instance]).
class PresenterFfi {
  PresenterFfi(ffi.DynamicLibrary lib)
      : _register = lib.lookupFunction<_RegisterNative, _RegisterDart>(
            'ihs_webrtc_view_register'),
        _sinkForView = lib.lookupFunction<_SinkForViewNative, _SinkForViewDart>(
            'ihs_webrtc_view_sink_for_view');

  final _RegisterDart _register;
  final _SinkForViewDart _sinkForView;

  static PresenterFfi? _instance;

  /// The process-wide presenter binding, opening [kPresenterLibName] on first
  /// use (or [path] when given).
  static PresenterFfi instance([String? path]) => _instance ??=
      PresenterFfi(ffi.DynamicLibrary.open(path ?? kPresenterLibName));

  /// Installs the platform-view factory for [viewType]. Idempotent per type.
  /// Returns true on success (ihs_shared reachable and factory installed).
  bool registerFactory(String viewType) {
    final ptr = viewType.toNativeUtf8();
    try {
      return _register(ptr) == 0;
    } finally {
      malloc.free(ptr);
    }
  }

  /// Returns the sink for a created view, or null if the view does not exist
  /// yet. The returned pointers are valid until the view is disposed.
  ViewSink? sinkForView(int viewId) {
    final outUser = malloc<ffi.Pointer<ffi.Void>>();
    try {
      final sink = _sinkForView(viewId, outUser);
      if (sink == ffi.nullptr) {
        return null;
      }
      return ViewSink(sink, outUser.value);
    } finally {
      malloc.free(outUser);
    }
  }
}

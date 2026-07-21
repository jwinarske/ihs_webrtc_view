// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// ihs_webrtc_view — zero-copy WebRTC video as a Flutter platform view for
/// ivi-homescreen.
///
/// The Dart half brokers the binding only; frames never touch Dart. Call
/// [initIhsWebRtcView] once at startup to install the native platform-view
/// factory, then place a [WebRtcView] with a decoded [WebRtcVideoTrack]. On
/// view attach the native presenter surfaces its LwVideoSinkV1; this package
/// registers it with libwebrtc and binds it to the track so the decoder
/// delivers dmabuf frames straight to the ivi-homescreen registry. Unbind is
/// automatic on dispose.
library ihs_webrtc_view;

import 'src/ffi/presenter_ffi.dart';
import 'src/webrtc_view.dart' show kWebRtcViewType;

export 'src/binding.dart' show WebRtcViewBinding;
export 'src/track.dart' show WebRtcVideoTrack;
export 'src/webrtc_view.dart' show WebRtcView, kWebRtcViewType;

/// Installs the native platform-view factory for [kWebRtcViewType]. Call once
/// at startup, before building a [WebRtcView]. Returns true on success (the
/// presenter `.so` loaded and ihs_shared was reachable). Idempotent.
bool initIhsWebRtcView() {
  return PresenterFfi.instance().registerFactory(kWebRtcViewType);
}

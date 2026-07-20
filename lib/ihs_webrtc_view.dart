// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

/// ihs_webrtc_view — Flutter platform-view widget + track->sink-token glue.
///
/// The Dart half brokers the binding only; frames never touch Dart. On view
/// attach the native presenter registers a sink and surfaces its token; this
/// widget calls `track.bindSink(token)` (rtc_dart) so frames flow
/// native-to-native. Unbind on dispose.
///
/// TODO: implement the platform-view widget + token plumbing over rtc_dart
/// once the presenter and rtc_dart idiomatic layer land.
library ihs_webrtc_view;

// export 'src/webrtc_view.dart' show WebRtcView;

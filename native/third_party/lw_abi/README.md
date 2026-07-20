# vendored lw_abi

`lw_video_sink.h` vendored verbatim from `jwinarske/libwebrtc`
`include/c/lw_video_sink.h`, pinned by `LW_ABI_VERSION`. This is the ONLY
libwebrtc header the native presenter may include — the C ABI boundary. CI
asserts byte-identity to the pinned upstream revision.

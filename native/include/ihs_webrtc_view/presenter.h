// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// Presenter: bridges one decoded video track (LwVideoSinkV1, dmabuf frames) to
// one ivi-homescreen platform view (ihs_pv_submit). It is the LwVideoSinkV1
// consumer and the ihs producer at once.
//
// The heavy import (KMS plane / GL texture / SHM blit) is the registry's job:
// the presenter negotiates a surface path once, then hands each decoded dmabuf
// frame to ihs_pv_submit. It performs NO EGL/KMS/Wayland work itself.
//
// Threading: on_frame arrives on the decoder delivery thread and only does an
// SPSC latest-wins handoff (non-blocking, allocation-free). A dedicated present
// thread drains the slot and calls ihs_pv_submit, which may block. Producer
// releases (re-QBUF) are issued from the present thread once the registry is
// done with a buffer (bounded by an in-flight ring, honoring release fences).
//
// Speaks the flat C ABIs only (lw_video_sink.h + ihs/*). MUST NOT include any
// C++ libwebrtc header.

#ifndef IHS_WEBRTC_VIEW_PRESENTER_H_
#define IHS_WEBRTC_VIEW_PRESENTER_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "ihs/platform_view.h"
#include "lw_video_sink.h"

namespace ihs_webrtc_view {

// The ihs entry points the presenter needs, captured once at construction so
// the hot path never re-resolves them. IhsPlatformViewApi is a global (C ABI)
// type from ihs/platform_view.h.
struct IhsBridge {
  const ::IhsPlatformViewApi* pv = nullptr;  // platform-view sub-table
};

class Presenter {
 public:
  // `view` and `grant` come from a successful ihs_pv_negotiate on the platform
  // thread; `bridge.pv` is the resolved platform-view API. The presenter does
  // not own the view (the registry does) but uses it for every submit.
  Presenter(const IhsBridge& bridge, ::IhsPlatformView* view,
            const ::IhsPvGrant& grant);
  ~Presenter();

  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  // The sink callback table to register with the libwebrtc sink registry and
  // bind to the track. Stable for the presenter's lifetime; `user` is `this`.
  const LwVideoSinkV1* sink() const { return &sink_; }
  void* sink_user() { return this; }

  // Pause/resume presentation when the view leaves/enters the composited scene.
  void set_suspended(bool suspended);

 private:
  // One frame taken from the decoder, awaiting or undergoing presentation.
  struct Held {
    LwDmabufDescriptor desc;
    LwFrameRelease release;
    void* release_ctx;
    int release_fence_fd;  // from ihs_pv_submit; -1 when none
  };

  // ---- LwVideoSinkV1 trampolines (decoder delivery thread) ----
  static int OnFrame(const LwDmabufDescriptor* desc, LwFrameRelease release,
                     void* release_ctx, void* user);
  static void OnFormat(const LwDmabufDescriptor* fmt_only, void* user);
  static void OnEos(void* user);

  void PresentLoop();         // present thread body
  void Submit(Held&& frame);  // build IhsFrame + ihs_pv_submit
  void Retire(Held& frame);   // wait release fence (bounded) + producer release
  void RetireSignalled();     // retire whatever the compositor has released
  void DrainInflight();       // retire everything still held
  void ReleaseHeld(Held& h);  // drop a queued frame (re-QBUF its buffer)

  IhsBridge bridge_;
  ::IhsPlatformView* view_;
  ::IhsPvGrant grant_;
  LwVideoSinkV1 sink_;

  std::mutex m_;
  std::condition_variable cv_;
  // Ready-to-present frames in arrival order, drained by the present thread on
  // a playout clock (see PresentLoop). A fixed-capacity ring: on_frame runs on
  // the decoder thread, which the sink contract requires to be allocation-free,
  // and an overrun drops the oldest so latency stays capped. A couple of frames
  // of lookahead smooth the decoder's bursty delivery.
  static constexpr size_t kReadyCap = 6;
  Held ready_[kReadyCap]{};
  size_t ready_head_ = 0;   // index of the oldest live entry
  size_t ready_count_ = 0;  // live entries in the ring
  // Ring accessors; the caller holds m_.
  bool ready_empty() const { return ready_count_ == 0; }
  Held& ready_at(size_t i) { return ready_[(ready_head_ + i) % kReadyCap]; }
  void ready_pop() {
    ready_head_ = (ready_head_ + 1) % kReadyCap;
    --ready_count_;
  }
  bool stop_ = false;
  bool suspended_ = false;
  uint32_t generation_ = 0;  // pool generation of the current format

  // Playout clock: maps the frames' presentation timestamps (the even capture
  // cadence) to wall-clock due times, so jittery decode delivery is presented
  // on a steady beat. Anchored on the first frame (plus a small buffer) and
  // re-anchored on a stream discontinuity / format change.
  bool clock_set_ = false;
  int64_t base_pts_us_ = 0;   // presentation timestamp (us) of the anchor frame
  int64_t base_wall_us_ = 0;  // wall-clock (monotonic us) the anchor is due

  std::deque<Held>
      inflight_;  // submitted, not yet retired (present thread only)
  // How many of those to keep, derived from the pool the producer advertises
  // and revisited whenever the pool is reallocated.
  size_t inflight_cap_ = 3;

  // Ring identity for the registry's import cache. It imports each buffer once
  // and reuses the VkImage/texture, keyed on IhsFrame::buffer_id, so the id has
  // to name the buffer rather than the frame. Plane fds are unique and stable
  // within a pool generation, so first sight of an fd assigns the next index;
  // a new generation clears the map, since the fds are handed straight back to
  // the new pool and would otherwise alias the old one's imports.
  std::map<int, uint32_t> buffer_ids_;
  uint32_t next_buffer_id_ = 0;
  uint32_t BufferIdFor(int plane_fd);
  std::thread present_thread_;
};

}  // namespace ihs_webrtc_view

#endif  // IHS_WEBRTC_VIEW_PRESENTER_H_

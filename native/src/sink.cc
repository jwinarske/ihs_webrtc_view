// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// sink.cc — Presenter: the LwVideoSinkV1 consumer + ihs_pv producer.
//
// Speaks the flat C ABIs only (lw_video_sink.h + ihs/*); MUST NOT include any
// C++ libwebrtc header. The registry performs the KMS/EGL/SHM import; this file
// only negotiates a slot's worth of buffering and forwards dmabuf frames.

#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <utility>

#include "ihs_webrtc_view/presenter.h"

namespace ihs_webrtc_view {
namespace {

// Upper bound on presenter-held buffers, whatever the pool size says. Holding
// submitted-but-not-retired frames keeps a buffer from being re-queued to the
// decoder until the implicit-sync scanout path (scene overlay) has finished
// with it — margin against the decoder overwriting a buffer the KMS plane is
// still scanning (tearing).
constexpr size_t kMaxInflight = 8;

// What to hold when the producer does not say how big its pool is. Small
// enough to be safe against the smallest pool likely to turn up, since the
// cost of guessing high is a starved decoder and the cost of guessing low is
// only less tolerance for bursty delivery.
constexpr size_t kDefaultInflight = 3;

// Fraction of the producer's pool the presenter will hold. The decoder needs
// the rest to decode into: reference frames, reorder depth and the buffer
// currently being written. A third leaves two thirds with the producer.
constexpr size_t kPoolShareDivisor = 3;

// How many buffers to hold given the pool the producer advertises.
//
// Pools differ by an order of magnitude — a VAAPI surface pool runs to dozens,
// a V4L2 CAPTURE pool is often reorder depth plus pipeline plus one, so eight
// to ten — so a fixed depth cannot suit both: eight is comfortable against
// twenty-eight surfaces and nearly the whole of a nine-buffer pool.
size_t InflightCapForPool(uint32_t pool_size) {
  if (pool_size == 0) {
    return kDefaultInflight;  // unknown: assume small
  }
  const size_t share = pool_size / kPoolShareDivisor;
  if (share < 2) {
    return 2;  // one on screen, one arriving
  }
  return share < kMaxInflight ? share : kMaxInflight;
}

// Bounded wait for a release fence before force-retiring a buffer (~2 vsync at
// 60 Hz + margin), matching the sink contract's release watchdog.
constexpr int kReleaseFenceTimeoutMs = 40;

// Lead the anchor by this much so early-arriving frames can be held rather than
// rushed — absorbs the decoder's bursty delivery. ~1.5 frames at 30 fps.
constexpr int64_t kPlayoutBufferUs = 50000;
// A pts gap beyond this (vs the playout clock) is a discontinuity (freeze,
// seek, format change) — re-anchor rather than stall or dump the whole queue.
constexpr int64_t kResyncThresholdUs = 300000;

int64_t NowUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void WaitAndCloseFence(int fd) {
  if (fd < 0) {
    return;
  }
  pollfd pfd{fd, POLLIN, 0};
  poll(&pfd, 1, kReleaseFenceTimeoutMs);  // best-effort; retire regardless
  ::close(fd);
}

}  // namespace

Presenter::Presenter(const IhsBridge& bridge, ::IhsPlatformView* view,
                     const ::IhsPvGrant& grant)
    : bridge_(bridge), view_(view), grant_(grant) {
  std::memset(&sink_, 0, sizeof(sink_));
  sink_.size = sizeof(sink_);
  sink_.on_frame = &Presenter::OnFrame;
  sink_.on_format = &Presenter::OnFormat;
  sink_.on_eos = &Presenter::OnEos;
  present_thread_ = std::thread(&Presenter::PresentLoop, this);
}

Presenter::~Presenter() {
  {
    std::lock_guard<std::mutex> lock(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (present_thread_.joinable()) {
    present_thread_.join();
  }
}

void Presenter::set_suspended(bool suspended) {
  std::lock_guard<std::mutex> lock(m_);
  suspended_ = suspended;
}

// ---- LwVideoSinkV1 (decoder delivery thread): non-blocking SPSC handoff ----

int Presenter::OnFrame(const LwDmabufDescriptor* desc, LwFrameRelease release,
                       void* release_ctx, void* user) {
  auto* self = static_cast<Presenter*>(user);
  // Validate the attacker/bug-reachable plane count before trusting the desc.
  if (!desc || desc->num_planes == 0 || desc->num_planes > LW_MAX_PLANES) {
    return 0;  // decline; producer drops
  }

  Held dropped{};
  bool have_dropped = false;
  {
    std::lock_guard<std::mutex> lock(self->m_);
    if (self->suspended_ || self->stop_) {
      return 0;  // decline; producer drops (keeps the decoder unblocked)
    }
    // Enqueue for the paced present thread (fixed ring, allocation-free).
    // Overrun (the consumer fell behind) drops the oldest so latency stays
    // bounded — the newest motion wins.
    if (self->ready_count_ == Presenter::kReadyCap) {
      dropped = self->ready_at(0);
      have_dropped = true;
      self->ready_pop();
    }
    self->ready_[(self->ready_head_ + self->ready_count_) %
                 Presenter::kReadyCap] = Held{*desc, release, release_ctx, -1};
    ++self->ready_count_;
  }
  self->cv_.notify_one();
  if (have_dropped && dropped.release) {
    dropped.release(dropped.release_ctx);  // retire the dropped frame now
  }
  return 1;  // took the new frame
}

void Presenter::OnFormat(const LwDmabufDescriptor* /*fmt_only*/,
                         void* /*user*/) {
  // No cached imports to invalidate here (the registry imports per submit); the
  // per-frame pool_generation check in PresentLoop drains stale in-flight
  // buffers before the first frame of the new generation is submitted.
}

void Presenter::OnEos(void* user) {
  auto* self = static_cast<Presenter*>(user);
  // Drop any queued-but-unpresented frames; the present thread drains in-flight
  // on wake. Move them out under the lock, then release outside it — the
  // producer's release() must not run while m_ is held. Re-anchor the clock so
  // a post-EOS resume restarts cleanly rather than as one giant late interval.
  Held pending[Presenter::kReadyCap];
  size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(self->m_);
    while (!self->ready_empty()) {
      pending[n++] = self->ready_at(0);
      self->ready_pop();
    }
    self->clock_set_ = false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (pending[i].release) {
      pending[i].release(pending[i].release_ctx);
    }
  }
  self->cv_.notify_one();
}

void Presenter::ReleaseHeld(Held& h) {
  if (h.release) {
    h.release(h.release_ctx);
    h.release = nullptr;
  }
}

// ---- present thread ----

void Presenter::PresentLoop() {
  for (;;) {
    Held frame{};
    {
      std::unique_lock<std::mutex> lock(m_);
      // Wait until a frame is ready (or shutdown). Once ready, hold it until
      // its playout due time; a spurious/early wake or a newly-arrived frame
      // just re-evaluates the head.
      for (;;) {
        cv_.wait(lock, [this] { return !ready_empty() || stop_; });
        if (stop_) {
          // Move queued frames out, then release outside the lock (release()
          // must not run while m_ is held).
          Held pending[kReadyCap];
          size_t n = 0;
          while (!ready_empty()) {
            pending[n++] = ready_at(0);
            ready_pop();
          }
          lock.unlock();
          for (size_t i = 0; i < n; ++i) {
            ReleaseHeld(pending[i]);
          }
          DrainInflight();
          return;
        }

        const int64_t pts = ready_at(0).desc.rtp_timestamp_us;  // microseconds
        const int64_t now = NowUs();
        if (!clock_set_) {
          base_pts_us_ = pts;
          base_wall_us_ = now + kPlayoutBufferUs;
          clock_set_ = true;
        }
        int64_t due = base_wall_us_ + (pts - base_pts_us_);

        // Discontinuity (freeze/seek/format wrap): the head sits far off the
        // clock in either direction. Re-anchor on it instead of stalling for a
        // huge future gap or dumping a long-past backlog frame-by-frame.
        if (due - now > kResyncThresholdUs || now - due > kResyncThresholdUs) {
          base_pts_us_ = pts;
          base_wall_us_ = now + kPlayoutBufferUs;
          due = base_wall_us_;
        }

        if (now < due) {
          // Not yet time. Sleep until due, but wake on a new frame/stop.
          cv_.wait_for(lock, std::chrono::microseconds(due - now));
          continue;  // re-evaluate the head (it may have changed)
        }

        // Due. If a newer frame is already due too, this one is stale — drop it
        // and catch up rather than flashing both in one refresh interval.
        if (ready_count_ > 1) {
          const int64_t next_pts = ready_at(1).desc.rtp_timestamp_us;
          const int64_t next_due = base_wall_us_ + (next_pts - base_pts_us_);
          if (now >= next_due) {
            Held stale = ready_at(0);
            ready_pop();
            lock.unlock();
            ReleaseHeld(stale);
            break;  // fall through to retire pass, then loop
          }
        }

        frame = ready_at(0);
        ready_pop();
        break;
      }
    }

    if (frame.release != nullptr) {
      // A pool reallocation reuses fd numbers; retire every prior-generation
      // buffer before presenting the new generation.
      if (frame.desc.pool_generation != generation_) {
        DrainInflight();
        generation_ = frame.desc.pool_generation;
      }
      // Track what the producer advertises rather than sampling it once: a
      // reallocated pool may be a different size, and a producer that never
      // changes generation would otherwise never be read at all.
      inflight_cap_ = InflightCapForPool(frame.desc.pool_size);
      Submit(std::move(frame));
    }

    // Return whatever the compositor has finished with. Under an explicit
    // grant this is the normal path and keeps only the frames still in use;
    // the depth window below is the backstop for implicit sync, and for a
    // fence that never signals.
    RetireSignalled();

    while (inflight_.size() > inflight_cap_) {
      Retire(inflight_.front());
      inflight_.pop_front();
    }
  }
}

void Presenter::Submit(Held&& frame) {
  ::IhsFrame f{};
  f.struct_size = sizeof(f);
  f.format.fourcc = frame.desc.fourcc;
  f.format.modifier = frame.desc.modifier;
  f.color_space = IHS_COLOR_SPACE_DEFAULT;  // registry picks BT.601/709 by size
  f.color_range = IHS_COLOR_RANGE_DEFAULT;
  f.width = frame.desc.width;
  f.height = frame.desc.height;
  f.plane_count = frame.desc.num_planes;
  for (uint32_t p = 0; p < frame.desc.num_planes && p < 4; ++p) {
    f.plane_fd[p] = frame.desc.planes[p].fd;
    f.plane_offset[p] = frame.desc.planes[p].offset;
    f.plane_stride[p] = frame.desc.planes[p].pitch;
  }
  f.hdr = nullptr;  // SDR

  int release_fence = -1;
  const int rc =
      (bridge_.pv && bridge_.pv->submit)
          ? bridge_.pv->submit(view_, &f, frame.desc.acquire_fence_fd,
                               &release_fence)
          : IHS_PV_ERR_NO_BACKEND;
  if (rc != IHS_PV_OK) {
    // The registry did not take the frame; retire it immediately.
    if (frame.release) {
      frame.release(frame.release_ctx);
    }
    return;
  }
  frame.release_fence_fd = release_fence;
  inflight_.push_back(frame);
}

void Presenter::RetireSignalled() {
  // Only an explicit grant carries release fences; with implicit sync there is
  // nothing to test, so the depth window governs as before.
  if (grant_.sync == IHS_PV_SYNC_IMPLICIT) {
    return;
  }
  // inflight_ is in submit order, so stop at the first frame the compositor is
  // still using rather than retiring out of order.
  while (!inflight_.empty()) {
    Held& front = inflight_.front();
    if (front.release_fence_fd >= 0) {
      pollfd pfd{front.release_fence_fd, POLLIN, 0};
      if (poll(&pfd, 1, 0) <= 0) {
        break;  // not signalled yet
      }
    }
    // An explicit grant with no fence means the buffer was already free on
    // return, so there is nothing to wait for.
    Retire(inflight_.front());
    inflight_.pop_front();
  }
}

void Presenter::Retire(Held& frame) {
  WaitAndCloseFence(frame.release_fence_fd);
  frame.release_fence_fd = -1;
  if (frame.release) {
    frame.release(frame.release_ctx);  // -> producer re-QBUF
    frame.release = nullptr;
  }
}

void Presenter::DrainInflight() {
  while (!inflight_.empty()) {
    Retire(inflight_.front());
    inflight_.pop_front();
  }
}

}  // namespace ihs_webrtc_view

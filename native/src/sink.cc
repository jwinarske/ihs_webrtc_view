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

// Presenter-held buffers cap. The decoder CAPTURE pool has headroom (>= 16); we
// keep at most this many submitted-but-not-retired so a buffer is not re-queued
// to the decoder until the implicit-sync scanout path (scene overlay) has long
// finished with it — a wide margin against the decoder overwriting a buffer the
// KMS plane is still scanning (tearing). Must stay below the CAPTURE count so
// the decoder never starves for a free buffer to decode into.
constexpr size_t kMaxInflight = 8;

// Bounded wait for a release fence before force-retiring a buffer (~2 vsync at
// 60 Hz + margin), matching the sink contract's release watchdog.
constexpr int kReleaseFenceTimeoutMs = 40;

// Playout pacing (see PresentLoop). RTP video is a 90 kHz clock.
constexpr int64_t kRtpHz = 90000;
// Lead the anchor by this much so early-arriving frames can be held rather than
// rushed — absorbs the decoder's bursty delivery. ~1.5 frames at 30 fps.
constexpr int64_t kPlayoutBufferUs = 50000;
// Ready-queue depth cap; an overrun drops the oldest to keep latency bounded.
constexpr size_t kMaxReady = 6;
// An RTP gap beyond this (vs the playout clock) is a discontinuity (freeze,
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
    // Enqueue for the paced present thread. Overrun (the consumer fell behind)
    // drops the oldest so latency stays bounded — the newest motion wins.
    self->ready_.push_back(Held{*desc, release, release_ctx, -1});
    if (self->ready_.size() > kMaxReady) {
      dropped = self->ready_.front();
      have_dropped = true;
      self->ready_.pop_front();
    }
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
  {
    std::lock_guard<std::mutex> lock(self->m_);
    // Drop any queued-but-unpresented frames; the present thread drains
    // in-flight on wake. Re-anchor the clock so a post-EOS resume restarts
    // cleanly rather than treating the gap as one giant late interval.
    for (auto& h : self->ready_) {
      if (h.release) {
        h.release(h.release_ctx);
      }
    }
    self->ready_.clear();
    self->clock_set_ = false;
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
        cv_.wait(lock, [this] { return !ready_.empty() || stop_; });
        if (stop_) {
          for (auto& h : ready_) {
            ReleaseHeld(h);
          }
          ready_.clear();
          lock.unlock();
          DrainInflight();
          return;
        }

        const int64_t rtp = ready_.front().desc.rtp_timestamp_us;
        const int64_t now = NowUs();
        if (!clock_set_) {
          base_rtp_ = rtp;
          base_wall_us_ = now + kPlayoutBufferUs;
          clock_set_ = true;
        }
        int64_t due = base_wall_us_ + (rtp - base_rtp_) * 1000000 / kRtpHz;

        // Discontinuity (freeze/seek/format wrap): the head sits far off the
        // clock in either direction. Re-anchor on it instead of stalling for a
        // huge future gap or dumping a long-past backlog frame-by-frame.
        if (due - now > kResyncThresholdUs || now - due > kResyncThresholdUs) {
          base_rtp_ = rtp;
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
        if (ready_.size() > 1) {
          const int64_t next_rtp = ready_[1].desc.rtp_timestamp_us;
          const int64_t next_due =
              base_wall_us_ + (next_rtp - base_rtp_) * 1000000 / kRtpHz;
          if (now >= next_due) {
            Held stale = ready_.front();
            ready_.pop_front();
            lock.unlock();
            ReleaseHeld(stale);
            break;  // fall through to retire pass, then loop
          }
        }

        frame = ready_.front();
        ready_.pop_front();
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
      Submit(std::move(frame));
    }

    while (inflight_.size() > kMaxInflight) {
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

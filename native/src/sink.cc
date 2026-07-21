// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// sink.cc — Presenter: the LwVideoSinkV1 consumer + ihs_pv producer.
//
// Speaks the flat C ABIs only (lw_video_sink.h + ihs/*); MUST NOT include any
// C++ libwebrtc header. The registry performs the KMS/EGL/SHM import; this file
// only negotiates a slot's worth of buffering and forwards dmabuf frames.

#include <poll.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "ihs_webrtc_view/presenter.h"

namespace ihs_webrtc_view {
namespace {

// Presenter-held buffers cap. The decoder CAPTURE pool has headroom (>= 6); we
// keep at most this many submitted-but-not-retired so the decoder never
// starves.
constexpr size_t kMaxInflight = 3;

// Bounded wait for a release fence before force-retiring a buffer (~2 vsync at
// 60 Hz + margin), matching the sink contract's release watchdog.
constexpr int kReleaseFenceTimeoutMs = 40;

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
    if (self->has_slot_) {
      // Latest-wins: the previous slotted-but-unpresented frame is dropped.
      dropped = self->slot_;
      have_dropped = true;
    }
    self->slot_ = Held{*desc, release, release_ctx, -1};
    self->has_slot_ = true;
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
    // Drop any pending slot; the present thread drains in-flight on wake.
    if (self->has_slot_ && self->slot_.release) {
      self->slot_.release(self->slot_.release_ctx);
    }
    self->has_slot_ = false;
  }
  self->cv_.notify_one();
}

// ---- present thread ----

void Presenter::PresentLoop() {
  for (;;) {
    Held frame{};
    {
      std::unique_lock<std::mutex> lock(m_);
      cv_.wait(lock, [this] { return has_slot_ || stop_; });
      if (stop_) {
        // Retire a frame still parked in the slot so its buffer is re-queued.
        if (has_slot_) {
          Held s = slot_;
          has_slot_ = false;
          lock.unlock();
          if (s.release) {
            s.release(s.release_ctx);
          }
        }
        break;
      }
      frame = slot_;
      has_slot_ = false;
    }

    // A pool reallocation reuses fd numbers; retire every prior-generation
    // buffer before presenting the new generation.
    if (frame.desc.pool_generation != generation_) {
      DrainInflight();
      generation_ = frame.desc.pool_generation;
    }

    Submit(std::move(frame));

    while (inflight_.size() > kMaxInflight) {
      Retire(inflight_.front());
      inflight_.pop_front();
    }
  }
  DrainInflight();
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

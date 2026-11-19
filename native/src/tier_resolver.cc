// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT

// tier_resolver.cc — ihs_shared bridge + platform-view factory + surface
// negotiation, and the flat C entry points the FFI control plane calls.
//
// Resolves libihs_shared.so's ihs_get_api at load, installs a platform-view
// factory for the webrtc view type, and on each view negotiates a surface path
// (dmabuf import / DRM plane / software floor). Each view owns one Presenter
// (see sink.cc). The registry performs the actual import; this file selects the
// path and owns view lifetime.
//
// Speaks the flat C ABIs only (ihs/* + lw_video_sink.h); MUST NOT include any
// C++ libwebrtc header.

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "ihs/ihs.h"
#include "ihs/platform_view.h"
#include "ihs_webrtc_view/presenter.h"

namespace ihs_webrtc_view {
namespace {

uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

// DRM_FORMAT_MOD_BROADCOM_SAND128 with no column height in it: the height is
// in each plane's pitch, which is how the decoder describes a frame whose luma
// and chroma are in separate buffers. Spelled out because this file includes
// no DRM header.
constexpr uint64_t kModBroadcomSand128 = (0x07ULL << 56) | 4;

// The layouts the decoder can hand over, and so the ones a frame may be
// submitted in (Presenter::Submit passes each frame's own fourcc and modifier
// on). Linear NV12 first: most decoders produce it, a vc4 overlay plane scans
// it out directly and the software floor reads it, so it is the one to be
// granted where the backend takes it. A Pi 5's HEVC decoder has no linear
// output: its frames are SAND128 columns, NV12 for 8-bit and P030 for 10-bit.
// Asking for linear alone told the registry something the frames then
// contradicted.
const IhsFormatModifier kFormats[] = {
    {Fourcc('N', 'V', '1', '2'), 0, 0},
    {Fourcc('N', 'V', '1', '2'), 0, kModBroadcomSand128},
    {Fourcc('P', '0', '3', '0'), 0, kModBroadcomSand128},
};
constexpr size_t kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

// ---- ihs_shared resolution (once) ----

struct Ihs {
  const IhsApi* api = nullptr;
  const IhsPlatformViewApi* pv = nullptr;
};

const Ihs& ResolveIhs() {
  static Ihs ihs = [] {
    Ihs out;
    using GetApiFn = const IhsApi* (*)(uint32_t);
    // libihs_shared.so is already loaded by the host; prefer the global scope,
    // fall back to loading it by name.
    auto get_api =
        reinterpret_cast<GetApiFn>(dlsym(RTLD_DEFAULT, "ihs_get_api"));
    if (!get_api) {
      if (void* h = dlopen("libihs_shared.so", RTLD_NOW | RTLD_GLOBAL)) {
        get_api = reinterpret_cast<GetApiFn>(dlsym(h, "ihs_get_api"));
      }
    }
    if (get_api) {
      out.api = get_api(IHS_SHARED_ABI_VERSION);
      if (out.api) {
        out.pv = out.api->platform_view;
      }
    }
    return out;
  }();
  return ihs;
}

// ---- per-view registry (view id -> Presenter) ----

std::mutex g_mutex;
std::map<int32_t, std::unique_ptr<Presenter>>& Views() {
  static auto* v = new std::map<int32_t, std::unique_ptr<Presenter>>();
  return *v;
}

// The negotiated presenter for a view, or nullptr.
Presenter* FindPresenter(int32_t view_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = Views().find(view_id);
  return it == Views().end() ? nullptr : it->second.get();
}

// ---- platform-view callbacks (platform thread) ----

struct ViewState {
  int32_t id;
  IhsPlatformView* view;
  Presenter* presenter;  // owned by Views()
};

void OnDispose(void* user_data) {
  auto* st = static_cast<ViewState*>(user_data);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Views().erase(st->id);  // destroys the Presenter (drains + joins)
  }
  delete st;
}

void OnSetSuspended(void* user_data, uint8_t suspended) {
  auto* st = static_cast<ViewState*>(user_data);
  if (st->presenter) {
    st->presenter->set_suspended(suspended != 0);
  }
}

// Pointer/touch + gesture-arena arbitration from the registry (platform
// thread), forwarded to the per-view presenter, which queues them for its
// present thread.
void OnTouch(void* user_data, int32_t action, int32_t point_count,
             size_t pointer_data_size, const double* pointer_data) {
  auto* st = static_cast<ViewState*>(user_data);
  if (st->presenter) {
    st->presenter->OnTouch(action, point_count, pointer_data_size,
                           pointer_data);
  }
}

void OnAcceptGesture(void* user_data) {
  auto* st = static_cast<ViewState*>(user_data);
  if (st->presenter) {
    st->presenter->OnAcceptGesture();
  }
}

void OnRejectGesture(void* user_data) {
  auto* st = static_cast<ViewState*>(user_data);
  if (st->presenter) {
    st->presenter->OnRejectGesture();
  }
}

void OnRenegotiate(void* user_data) {
  auto* st = static_cast<ViewState*>(user_data);
  const Ihs& ihs = ResolveIhs();
  if (!ihs.pv || !st->view) {
    return;
  }
  // Re-request the same surface path; on failure the registry drops us to the
  // software floor. submit is uniform across kinds, so no per-kind rewiring.
  IhsPvRequirements req{};
  req.struct_size = sizeof(req);
  // Every kind this presenter can drive, best first in intent; the registry
  // grants the best it can offer. TEXTURE_DMABUF_IMPORT matters as much as the
  // plane: a Vulkan backend offers only that, and asking for the plane alone
  // would drop such a backend to the software floor -- a copy per frame, which
  // is the one outcome the whole path exists to avoid.
  req.kinds = IHS_PV_KIND_DRM_PLANE | IHS_PV_KIND_TEXTURE_DMABUF_IMPORT |
              IHS_PV_KIND_SOFTWARE_SHM;
  req.formats = kFormats;
  req.format_count = kFormatCount;
  req.sync =
      IHS_PV_SYNC_EXPLICIT_PREFERRED;  // get a release fence -> no tearing
  req.z_order = IHS_PV_Z_BELOW_FLUTTER;
  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  ihs.pv->negotiate(st->view, &req, &grant);
}

// The factory the registry invokes when Flutter creates a view of our type.
int Factory(const IhsPvCreateInfo* info, void* /*factory_user*/,
            IhsPlatformView* view, IhsPvCallbacks* out_callbacks,
            void** out_user_data) {
  const Ihs& ihs = ResolveIhs();
  if (!ihs.pv || !info || !view) {
    return IHS_PV_ERR_INVALID;
  }

  // Negotiate a surface path. Requesting the software floor guarantees a grant.
  IhsPvRequirements req{};
  req.struct_size = sizeof(req);
  // Every kind this presenter can drive, best first in intent; the registry
  // grants the best it can offer. TEXTURE_DMABUF_IMPORT matters as much as the
  // plane: a Vulkan backend offers only that, and asking for the plane alone
  // would drop such a backend to the software floor -- a copy per frame, which
  // is the one outcome the whole path exists to avoid.
  req.kinds = IHS_PV_KIND_DRM_PLANE | IHS_PV_KIND_TEXTURE_DMABUF_IMPORT |
              IHS_PV_KIND_SOFTWARE_SHM;
  req.formats = kFormats;
  req.format_count = kFormatCount;
  req.needs_alpha = 0;
  req.sync =
      IHS_PV_SYNC_EXPLICIT_PREFERRED;  // get a release fence -> no tearing
  req.z_order = IHS_PV_Z_BELOW_FLUTTER;

  IhsPvGrant grant{};
  grant.struct_size = sizeof(grant);
  const int nrc = ihs.pv->negotiate(view, &req, &grant);
  if (nrc != IHS_PV_OK) {
    return IHS_PV_ERR_UNSUPPORTED;
  }

  IhsBridge bridge;
  bridge.pv = ihs.pv;
  auto presenter =
      std::unique_ptr<Presenter>(new Presenter(bridge, view, grant));
  auto* state = new ViewState{info->id, view, presenter.get()};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Views()[info->id] = std::move(presenter);
  }

  std::memset(out_callbacks, 0, sizeof(*out_callbacks));
  out_callbacks->struct_size = sizeof(*out_callbacks);
  out_callbacks->on_touch = &OnTouch;
  out_callbacks->accept_gesture = &OnAcceptGesture;
  out_callbacks->reject_gesture = &OnRejectGesture;
  out_callbacks->set_suspended = &OnSetSuspended;
  out_callbacks->renegotiate = &OnRenegotiate;
  out_callbacks->dispose = &OnDispose;
  *out_user_data = state;
  return IHS_PV_OK;
}

}  // namespace
}  // namespace ihs_webrtc_view

// ---- public flat C ABI (called by the Dart/FFI control plane) ----

extern "C" {

// Installs the platform-view factory for `view_type`. Call once at plugin load.
// Returns 0 on success, negative on failure (ihs_shared unavailable).
__attribute__((visibility("default"))) int ihs_webrtc_view_register(
    const char* view_type) {
  const auto& ihs = ihs_webrtc_view::ResolveIhs();
  if (!ihs.pv || !view_type) {
    return -1;
  }
  return ihs.pv->register_factory(view_type, &ihs_webrtc_view::Factory,
                                  nullptr) == IHS_PV_OK
             ? 0
             : -1;
}

// Returns the LwVideoSinkV1 for a created view so the control plane can
// register it (lw_video_sink_register) and bind it to the decoded track
// (lw_video_track_bind_sink). *out_user receives the sink's user cookie.
// Returns NULL if the view does not exist yet. The pointer is valid until the
// view is disposed.
__attribute__((visibility("default"))) const LwVideoSinkV1*
ihs_webrtc_view_sink_for_view(int32_t view_id, void** out_user) {
  auto* p = ihs_webrtc_view::FindPresenter(view_id);
  if (!p) {
    return nullptr;
  }
  if (out_user) {
    *out_user = p->sink_user();
  }
  return p->sink();
}

// Total input samples the view's present thread has consumed since creation.
// An observability/bring-up hook ("is touch reaching the view?") and what the
// unit test checks. Returns 0 if the view does not exist.
__attribute__((visibility("default"))) uint64_t
ihs_webrtc_view_touch_count(int32_t view_id) {
  auto* p = ihs_webrtc_view::FindPresenter(view_id);
  return p ? p->touches_consumed() : 0;
}

}  // extern "C"

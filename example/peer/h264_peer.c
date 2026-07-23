// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: MIT
//
// h264_peer — the sending half the example connects to.
//
// Listens on a TCP port, and for the first client offers an H.264 video track
// fed by a generated pattern, then relays ICE. It speaks the same framed
// protocol as native/webrtc_session.c:
//
//   -> OFFER <len>\n <sdp>            offered to the client
//   <- ANSWER <len>\n <sdp>           the client's answer
//   <> CAND <mline> <len>\n <cand>    both directions
//
// Built on the same flat C ABI as everything else, so it needs no Python and
// no second WebRTC stack.
//
//   gcc -I<libwebrtc>/include peer/h264_peer.c -lwebrtc -o h264_peer
//   ./h264_peer [port]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "c/lw_c_api.h"

#define WIDTH 640
#define HEIGHT 480

static int g_sock = -1;
static pthread_mutex_t g_send_m = PTHREAD_MUTEX_INITIALIZER;
static lw_pc_t* g_pc = NULL;
static volatile int g_remote_set = 0;
static volatile int g_running = 1;

static void send_framed(const char* hdr, const char* payload, int len) {
  pthread_mutex_lock(&g_send_m);
  (void)!write(g_sock, hdr, strlen(hdr));
  if (len > 0) (void)!write(g_sock, payload, len);
  pthread_mutex_unlock(&g_send_m);
}

// Candidates offered before the far side has a remote description are dropped,
// so hold them until the answer lands.
static char* g_pending[64];
static int g_pending_mline[64];
static int g_pending_n = 0;

static void on_ice_candidate(char* cand, char* mid, int mline, void* user) {
  (void)user;
  (void)mid;
  if (cand != NULL) {
    if (g_remote_set) {
      char hdr[64];
      int n = (int)strlen(cand);
      snprintf(hdr, sizeof hdr, "CAND %d %d\n", mline, n);
      send_framed(hdr, cand, n);
    } else if (g_pending_n < 64) {
      g_pending[g_pending_n] = strdup(cand);
      g_pending_mline[g_pending_n++] = mline;
    }
  }
  lw_string_free(cand);
  lw_string_free(mid);
}

static void flush_pending(void) {
  for (int i = 0; i < g_pending_n; ++i) {
    char hdr[64];
    int n = (int)strlen(g_pending[i]);
    snprintf(hdr, sizeof hdr, "CAND %d %d\n", g_pending_mline[i], n);
    send_framed(hdr, g_pending[i], n);
    free(g_pending[i]);
  }
  g_pending_n = 0;
}

static void on_connection_state(int state, void* user) {
  (void)user;
  fprintf(stderr, "[peer] connection_state=%d\n", state);
}

static void on_fail(char* err, void* user) {
  (void)user;
  fprintf(stderr, "[peer] sdp error: %s\n", err ? err : "?");
  lw_string_free(err);
}

static void on_offer(char* sdp, char* type, void* user) {
  (void)user;
  (void)type;
  if (sdp != NULL) {
    lw_pc_set_local_description(g_pc, sdp, "offer", NULL, on_fail, NULL);
    int n = (int)strlen(sdp);
    char hdr[64];
    snprintf(hdr, sizeof hdr, "OFFER %d\n", n);
    send_framed(hdr, sdp, n);
    fprintf(stderr, "[peer] offer sent (%d bytes)\n", n);
  }
  lw_string_free(sdp);
  lw_string_free(type);
}

static void on_remote_answer_set(void* user) {
  (void)user;
  g_remote_set = 1;
  flush_pending();
  fprintf(stderr, "[peer] answer applied\n");
}

static int read_line(int fd, char* out, int max) {
  int i = 0;
  while (i < max - 1) {
    char c;
    if (read(fd, &c, 1) <= 0) return -1;
    if (c == '\n') break;
    out[i++] = c;
  }
  out[i] = 0;
  return i;
}

static int read_n(int fd, char* buf, int n) {
  int got = 0;
  while (got < n) {
    int r = read(fd, buf + got, n - got);
    if (r <= 0) return -1;
    got += r;
  }
  return got;
}

static void* signal_loop(void* arg) {
  (void)arg;
  char header[128];
  char* payload = malloc(1 << 16);
  while (g_running) {
    int hn = read_line(g_sock, header, sizeof header);
    if (hn <= 0) break;
    char kind[16];
    int a = 0, b = 0;
    if (sscanf(header, "%15s", kind) != 1) continue;
    if (strcmp(kind, "ANSWER") == 0) {
      sscanf(header, "%*s %d", &b);
      if (read_n(g_sock, payload, b) == b) {
        payload[b] = 0;
        fprintf(stderr, "[peer] answer received (%d bytes)\n", b);
        lw_pc_set_remote_description(g_pc, payload, "answer",
                                     on_remote_answer_set, on_fail, NULL);
      }
    } else if (strcmp(kind, "CAND") == 0) {
      sscanf(header, "%*s %d %d", &a, &b);
      if (read_n(g_sock, payload, b) == b) {
        payload[b] = 0;
        lw_pc_add_ice_candidate(g_pc, "", a, payload);
      }
    }
  }
  free(payload);
  return NULL;
}

int main(int argc, char** argv) {
  const int port = argc > 1 ? atoi(argv[1]) : 9300;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr*)&addr, sizeof addr) != 0 ||
      listen(srv, 1) != 0) {
    perror("[peer] bind/listen");
    return 1;
  }
  fprintf(stderr, "[peer] listening on %d\n", port);
  g_sock = accept(srv, NULL, NULL);
  if (g_sock < 0) {
    perror("[peer] accept");
    return 1;
  }
  fprintf(stderr, "[peer] client connected\n");

  if (!lw_initialize()) {
    fprintf(stderr, "[peer] lw_initialize failed\n");
    return 1;
  }
  lw_factory_t* factory = lw_factory_create();
  if (factory == NULL || !lw_factory_initialize(factory)) {
    fprintf(stderr, "[peer] factory failed\n");
    return 1;
  }
  g_pc = lw_pc_create(factory);

  LwPcObserver obs;
  memset(&obs, 0, sizeof obs);
  obs.size = sizeof obs;
  obs.on_ice_candidate = on_ice_candidate;
  obs.on_connection_state = on_connection_state;
  lw_pc_set_observer(g_pc, &obs, NULL);

  lw_video_source_t* source =
      lw_factory_create_video_source(factory, "peer-source");
  lw_video_track_t* track =
      lw_factory_create_video_track(factory, source, "peer-video");
  const char* stream_ids[] = {"peer-stream"};
  lw_sender_t* sender = lw_pc_add_track(g_pc, track, stream_ids, 1);
  if (source == NULL || track == NULL || sender == NULL) {
    fprintf(stderr, "[peer] local video failed\n");
    return 1;
  }

  pthread_t th;
  pthread_create(&th, NULL, signal_loop, NULL);

  lw_pc_create_offer(g_pc, on_offer, on_fail, NULL);

  // A moving pattern, so a still frame is obviously distinguishable from a
  // live one on screen.
  const size_t size = (size_t)WIDTH * HEIGHT * 3 / 2;
  uint8_t* i420 = malloc(size);
  for (int n = 0; g_running; ++n) {
    for (int y = 0; y < HEIGHT; ++y) {
      for (int x = 0; x < WIDTH; ++x) {
        i420[(size_t)y * WIDTH + x] = (uint8_t)(x + y + n * 4);
      }
    }
    memset(i420 + (size_t)WIDTH * HEIGHT, 128, (size_t)WIDTH * HEIGHT / 2);
    lw_video_source_push_i420(source, WIDTH, HEIGHT, i420, size);
    usleep(33000);
  }
  free(i420);
  return 0;
}

/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * speed_tracker.c - EMA-smoothed speed tracker
 */

#include <x/base/speed_tracker.h>
#include <x/base/time.h>

#include <stdio.h>

void xSpeedTrackerUpdate(xSpeedTracker *t, uint64_t transferred) {
  uint64_t now_ms = xMonoMs();
  uint64_t dt     = now_ms - t->prev_ms;

  if (t->prev_ms > 0 && dt > 0) {
    double instant = (double)(transferred - t->prev_bytes) / (dt / 1000.0);
    if (t->smooth_bps <= 0.0)
      t->smooth_bps = instant;
    else
      t->smooth_bps = t->alpha * instant + (1.0 - t->alpha) * t->smooth_bps;
  }
  t->prev_bytes = transferred;
  t->prev_ms    = now_ms;
}

char *xSpeedTrackerFormat(const xSpeedTracker *t, char *buf, size_t bufsz) {
  if (t->smooth_bps > 0.0) {
    if (t->smooth_bps >= 1024.0 * 1024.0)
      snprintf(buf, bufsz, "  %.2f MB/s",
               t->smooth_bps / (1024.0 * 1024.0));
    else
      snprintf(buf, bufsz, "  %.1f KB/s",
               t->smooth_bps / 1024.0);
  } else {
    buf[0] = '\0';
  }
  return buf;
}

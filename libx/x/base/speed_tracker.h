/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * speed_tracker.h - EMA-smoothed speed tracker
 */

#ifndef XBASE_SPEED_TRACKER_H
#define XBASE_SPEED_TRACKER_H

#include <x/base/base.h>

#include <stddef.h>
#include <stdint.h>

/* ── EMA-smoothed speed tracker ────────────────────────── */

/**
 * @brief Tracks transfer speed with exponential moving average smoothing.
 *
 * Usage:
 *   1. Declare a xSpeedTracker and initialise with XSPEED_TRACKER_INIT().
 *   2. Call xSpeedTrackerUpdate() in every progress callback.
 *   3. Call xSpeedTrackerFormat() to get a human-readable string.
 */
XDEF_STRUCT(xSpeedTracker) {
  uint64_t prev_bytes;
  uint64_t prev_ms;
  double   smooth_bps; /* bytes per second, smoothed */
  double   alpha;      /* EMA weight for new sample (0,1] */
};

#define XSPEED_TRACKER_INIT(a) { 0, 0, 0.0, (a) }

/**
 * @brief Feed a new transferred-bytes sample into the tracker.
 *
 * Internally reads the monotonic clock to compute elapsed time.
 */
XCAPI(void) xSpeedTrackerUpdate(xSpeedTracker *t, uint64_t transferred);

/**
 * @brief Format the current smoothed speed into buf.
 *
 * Writes something like "  1.23 MB/s" or "  456.7 KB/s".
 * If no speed data is available yet, buf[0] is set to '\0'.
 *
 * @return buf (for convenience).
 */
XCAPI(char *) xSpeedTrackerFormat(const xSpeedTracker *t,
                                  char *buf, size_t bufsz);

#endif /* XBASE_SPEED_TRACKER_H */

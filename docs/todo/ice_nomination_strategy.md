# xp2p — TODO

> Optimize ICE nomination strategy to reduce connection establishment latency.

## ICE Nomination Strategy Optimization

### Background

During real-world testing of the `xfer` file transfer tool (sender behind restricted NAT, receiver on a public-IP VPS), we observed that the ICE agent takes longer than necessary to establish a connection. The root cause is the current nomination strategy: it waits for **all** candidate pairs to be dispatched before nominating, even if a high-priority pair has already succeeded much earlier.

### Current Behavior

The current `try_nominate` logic in `ice_agent.c` requires two conditions:

```c
if (any_succeeded && a->check_index >= a->pair_count) {
    // nominate the highest-priority succeeded pair
}
```

1. At least one pair has succeeded (`any_succeeded`)
2. **All pairs have been dispatched** (`check_index >= pair_count`)

With 8 candidate pairs and a 50 ms pacing interval, this means:

- Even if pair[2] succeeds at T=150 ms, nomination is delayed until T=400 ms (when all 8 pairs are dispatched)
- The extra 250 ms is pure waste — we're waiting for lower-priority pairs to be sent out, not for better results

#### Example from real logs

```text
T=0ms    send_check: pair[0] 192.168.1.11 -> 10.5.8.12        (host→host, will fail)
T=50ms   send_check: pair[1] 192.168.255.10 -> 10.5.8.12      (host→host, will fail)
T=100ms  send_check: pair[2] 192.168.1.11 -> 43.161.217.33    (host→srflx)
T=120ms  ✅ check response: pair[2] SUCCESS                     ← could nominate here!
T=150ms  send_check: pair[3] 120.229.22.97 -> 10.5.8.12       (srflx→host)
T=200ms  send_check: pair[4] 192.168.255.10 -> 43.161.217.33  (host→srflx)
T=220ms  ✅ check response: pair[4] SUCCESS
T=250ms  send_check: pair[5] 120.229.22.97 -> 43.161.217.33   (srflx→srflx)
T=270ms  ✅ check response: pair[5] SUCCESS
T=300ms  send_check: pair[6] 120.229.22.97 -> 10.5.8.12       (srflx→host)
T=350ms  send_check: pair[7] 120.229.22.97 -> 43.161.217.33   (srflx→srflx)
T=370ms  ✅ check response: pair[7] SUCCESS
T=370ms  nominated pair: pair[2]                                ← finally nominates!
```

Pair[2] succeeded at T=120 ms but nomination happened at T=370 ms — a **250 ms unnecessary delay**.

### Comparison with libwebrtc (Chromium)

| Aspect | moo (current) | libwebrtc (Chromium) |
| --- | --- | --- |
| **When to nominate** | After all pairs dispatched | First success → immediately usable |
| **Nomination model** | One-shot, immutable | Dynamic, can switch to better pair later |
| **USE-CANDIDATE flag** | All checks carry it (aggressive) | Only on selected pair |
| **Pacing impact on latency** | High (N pairs × pacing = delay) | Low (first success starts DTLS) |
| **Final pair quality** | Guaranteed global optimum | Converges to optimum over time |
| **Implementation complexity** | Simple | Complex (path switching, DTLS migration) |

#### libwebrtc's "Continuous Nomination"

libwebrtc does not strictly follow either RFC 8445 Regular or Aggressive nomination. Instead it uses a custom strategy:

1. **First succeeded pair is immediately selected** as `selected_connection`, DTLS/data starts flowing
2. If a **higher-priority pair** succeeds later, it **dynamically switches** to the new pair
3. A stabilization window prevents excessive switching

This gives the fastest possible time-to-first-byte while still converging to the optimal path.

### Proposed Optimization

#### Approach A: Early Nomination (Recommended)

Change the nomination condition from "all pairs dispatched" to "no higher-priority pair is still pending":

```text
When pair[i] succeeds:
  If all pairs with priority > pair[i].priority have reached
  a terminal state (Succeeded or Failed):
    → Nominate pair[i] immediately
  Else:
    → Wait (a better pair might still succeed)
```

**Benefits:**

- Pair[2] in the example above would be nominated at T=120 ms (after pair[0] and pair[1] fail), not T=370 ms
- No need for path switching — we still pick the global best among completed pairs
- Minimal code change in `try_nominate`

**Risks:**

- If pair[0] and pair[1] are still InProgress (not yet timed out), we'd still wait for them. But host→host pairs to unreachable private IPs typically fail quickly (ICMP unreachable), so this is rarely an issue in practice.

#### Approach B: libwebrtc-style Dynamic Switching

1. First succeeded pair → immediately nominate and start DTLS
2. If a better pair succeeds later → switch the nominated pair and migrate the DTLS path

**Benefits:**

- Absolute fastest connection establishment
- Matches browser WebRTC behavior

**Risks:**

- Requires DTLS layer to support path migration (re-binding to a different socket/address)
- Significantly more complex — need to handle in-flight packets during switch
- Overkill for the current use case

#### Approach C: Reduce Pacing Interval

Simply reduce `XICE_CHECK_PACING_MS` from 50 ms to a smaller value (e.g., 20 ms).

**Benefits:**

- Trivial change
- Reduces the "all dispatched" wait time proportionally

**Risks:**

- RFC 8445 recommends ≥ 50 ms pacing to avoid network congestion
- Doesn't solve the fundamental problem — just masks it

### Recommendation

**Approach A** is the sweet spot: minimal complexity, significant latency improvement, and no RFC compliance concerns. It can be implemented by modifying the `try_nominate` function to check whether all **higher-priority** pairs (not all pairs) have been dispatched and resolved.

Approach B can be revisited later if sub-100ms connection establishment becomes a requirement.

### Priority

P2 — The current strategy works correctly but adds unnecessary latency (100–300 ms depending on pair count) to every ICE connection. For interactive use cases like file transfer, this is noticeable. The fix is small and low-risk.

### Affected Code

- `libx/xp2p/ice_agent.c` — `try_nominate()`, `check_pacing_cb()`, `on_check_response()`

### References

- RFC 8445 §8.1.1 — Nominating Pairs (Regular and Aggressive)
- Chromium source: `p2p/base/p2p_transport_channel.cc` — `MaybeSwitchSelectedConnection()`
- Oleg Obolensky, "WebRTC ICE Nomination: How Browsers Really Do It" (webrtcHacks, 2020)

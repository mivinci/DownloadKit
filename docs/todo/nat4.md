# xp2p — TODO

> Analysis and feasibility study for NAT4 (Symmetric NAT) traversal via birthday attack port prediction.

## Symmetric NAT Traversal — Birthday Attack

### Background

RFC 3489 classifies NAT4 as **Symmetric NAT**: each `(src_ip, src_port, dst_ip, dst_port)` tuple maps to a **different** external port. This means the srflx candidate obtained via STUN (XOR-MAPPED-ADDRESS) has a port that **differs** from the port the NAT assigns when the peer sends to a different destination. Standard ICE srflx candidates are therefore ineffective under Symmetric NAT.

The current ICE agent falls back to **TURN relay** for Symmetric NAT scenarios, which always works but adds relay-hop latency. A birthday attack approach could potentially establish a direct path before resorting to TURN.

### Birthday Attack Principle

When both peers are behind Symmetric NATs:

1. Peer A opens `N` local UDP sockets and sends from each to B's STUN-reflected address
2. Peer B opens `M` local UDP sockets and sends from each to A's STUN-reflected address
3. A's NAT creates `N` distinct external port mappings; B's NAT creates `M` distinct mappings
4. If any of A's external ports matches the port B is targeting (or vice versa), the packet traverses the NAT → connection established

This exploits the **birthday paradox**: in a port space of `P ≈ 64512` (excluding well-known ports), opening `n` ports per side yields:

$$P(\text{collision}) \approx 1 - e^{-n^2 / P}$$

| Ports per side (n) | Collision probability |
| --- | --- |
| 128 | ~22% |
| 256 | ~63% |
| 512 | ~98% |
| 1024 | ~99.99% |

### Practical Constraints

#### NAT Port Allocation Is Not Always Random

Many Symmetric NATs use **sequential port allocation** rather than random. In this case:

- The birthday attack's random-collision assumption breaks down
- A **port prediction** strategy works better: send two STUN requests, observe the port delta `Δ`, predict the next port as `last_port + Δ`
- The current `send_stun_binding_for_host` sends only one STUN request per host candidate, so port deltas cannot be observed

#### Resource Overhead

Each side needs 256–512 bound UDP sockets sending simultaneously:

- `XICE_MAX_CANDIDATES` is currently 32 — far too small
- `XICE_MAX_PAIRS` would explode to `N × M`
- Each socket must be registered with the event loop, increasing memory and fd usage

#### NAT Mapping TTL

NAT mappings typically expire in 30–120 seconds. All probes must complete within this window. With the current `check_pacing_cb` at ~50 ms per pair, 256 pairs take 12.8 s (acceptable), but 512 pairs take 25.6 s (tight).

#### CGNAT Makes It Harder

Modern mobile networks use Carrier-Grade NAT (CGNAT) with larger port spaces and more complex allocation policies, reducing birthday attack success rates.

### Approach Comparison

| Approach | Applicable scenario | Success rate | Complexity |
| --- | --- | --- | --- |
| Standard ICE (srflx) | NAT1/2/3 | High | Low (already implemented) |
| TURN relay | All NAT types | 100% | Low (already implemented) |
| Birthday attack | Both sides Symmetric NAT | ~60–98% | High |
| Port prediction (sequential NAT) | Sequential-allocation Symmetric NAT | ~70–90% | Medium |

### Implementation Plan (If Pursued)

1. **Port delta detection** — During gathering, send two STUN Binding Requests from each host candidate to observe the NAT's port allocation delta
2. **Expand candidate limits** — Increase `XICE_MAX_CANDIDATES` and `XICE_MAX_PAIRS` (or use dynamic allocation) to accommodate the extra sockets
3. **Multi-port gathering** — Bind multiple local UDP sockets per interface and collect srflx candidates for each
4. **Parallel check dispatch** — Reduce pacing interval or send checks in parallel batches to fit within NAT mapping TTL
5. **Short timeout with TURN fallback** — Set a ~5 s timeout for the birthday attack phase; on failure, immediately fall back to TURN relay

### Priority

P3 — TURN relay already provides 100% connectivity for Symmetric NAT at the cost of modest relay-hop latency (typically tens of milliseconds with a well-placed TURN server). The birthday attack adds significant implementation complexity and non-deterministic success. Revisit if profiling shows TURN relay latency is a real bottleneck for the target use case, or if TURN server costs become a concern.

### References

- Guha, S., Takeda, Y., & Francis, P. (2005). "NUTSS: A SIP-based Approach to UDP and TCP Network Connectivity"
- Ford, B., Srisuresh, P., & Kegel, D. (2005). "Peer-to-Peer Communication Across Network Address Translators"
- RFC 8445 — Interactive Connectivity Establishment (ICE)
- RFC 3489 — STUN (Classic NAT Type Classification)

# DMUDP - DMOD UDP

## Overview

DMUDP builds and parses UDP segments (RFC 768) - source/destination port,
length, and a pseudo-header checksum - and sends/receives them. Sending
calls straight into [dmip](../../dmip)'s family-agnostic-by-destination-
address `dmip_send()`; receiving registers itself with dmip as the handler
for UDP's IP protocol number (`dmip_register_protocol()`) rather than
pulling packets from dmip directly - dmip dispatches by protocol number
(see [dmip.md](../../dmip/docs/dmip.md#protocol-dispatch)). Either way,
dmudp itself never talks to dmroute, dmarp, or dmnetif directly for
anything - dmip (and, below it, dmnetbridge) already does all of that
(route lookup, ARP resolution, frame I/O, fragmentation). Building UDP as
a thin layer over an IP layer that can already put a packet on the wire is
the entire reason dmudp exists as its own module rather than a few more
functions bolted onto dmip.

```
┌──────────────────────────────────────────────┐
│                  DMUDP                        │
│   build/parse/checksum a UDP segment,         │
│   per-port binding registry, dmudp_send()/    │
│   _bind()/_bind_any()/_unbind()               │
├──────────────────────────────────────────────┤
│         DMIP                    │   DMICMP    │
│   dmip_send(), dmip_checksum(), │   Port      │
│   protocol registration         │   Unreachable│
├──────────────────────────────────────────────┤
│      DMNETBRIDGE / DMROUTE / DMNETIF / DMARP  │
└──────────────────────────────────────────────┘
```

## One function per direction, not one per family

There is `dmudp_send()` - no `dmudp_v4_send()`/`_v6_send()`. A caller
sending a datagram only ever has one destination address, and that
address already carries the family that would otherwise decide which of
two same-shaped functions to call - making the caller repeat that choice
via the function name too is pure redundancy, not a real API surface. This
is the same reasoning [dmip.md](../../dmip/docs/dmip.md#family-agnostic-dmip_send)
gives for `dmip_send()` one layer down.

Receiving works the other way: a bound port's handler is simply called
with whichever family the datagram actually arrived as (its `src` address
carries that), rather than a caller having to ask "is anything here yet"
per family.

## A per-port registry, not a receive queue

Earlier drafts of this module had no port concept at all: a single
`dmudp_receive()` handed back every datagram regardless of destination
port, backed by a receive queue (dmlist + semaphore) that the dmip-driven
handler fed and a blocking caller drained. That shape is gone.

`dmudp_bind(port, handler)` reserves a specific UDP port and registers a
`dmudp_datagram_handler_t` for it; `dmudp_bind_any(handler, &out_port)`
does the same but picks the first free port in the ephemeral range
(`DMUDP_PORT_EPHEMERAL_FIRST`..`DMUDP_PORT_EPHEMERAL_LAST`, RFC 6335's
dynamic/private range) and reports which one it chose;
`dmudp_unbind(port)` releases either kind of registration. The registry
itself is a small `dmlist` of `{ port, handler }` entries guarded by a
`dmosi_mutex_t` - the same shape
[dmicmp](../../dmicmp/docs/dmicmp.md#sending-our-own-echo-request-needs-a-different-shape)'s
own echo-listener table uses, just keyed by UDP port instead of ICMP echo
identifier.

## No extra thread needed to deliver a datagram

`dmip_register_protocol()`'s callback already runs on whatever thread is
pumping the interface a packet arrived on (see `dmip_protocol_handler_t`
in `dmip.h`) - the same thread `dmnetbridge_handle_netif_rx()` uses.
`dmudp_handle_ip_packet()` parses and checksum-validates the segment on
that same thread, looks up the bound handler for its destination port, and
- if one is registered - calls it **right there, inline**, the same
reasoning [dmicmp.md](../../dmicmp/docs/dmicmp.md#no-extra-thread-needed-to-answer-a-ping)
gives for answering an Echo Request without a queue or worker thread. A
handler that wants to reply can just call `dmudp_send()` back from inside
itself, synchronously, in the same call. A handler that needs the payload
to outlive the call must copy it out itself - it is only valid for the
call's duration, the same borrowing rule `dmip_protocol_handler_t` itself
documents.

## An unbound port gets a Port Unreachable, not silence

A datagram addressed to a port with no `dmudp_bind()`/`_bind_any()`
registration is, for IPv4, reported back to its sender via
`dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_port, ...)` -
exactly the case [dmicmp.h](../../dmicmp/include/dmicmp.h) already
anticipated and documents dmudp as the motivating use for. This is always
on - there is no flag to suppress it, the same way `dmicmp`'s own
default-protocol-handler reply has none.

For IPv6 there is no equivalent: sending an ICMPv6 error needs
`dmip_v6_send()` (blocked on a missing NDP module, the same gap described
below), so an unbound-port IPv6 datagram is logged (`DMOD_LOG_WARN`) and
dropped instead - mirroring exactly how `dmicmp_handle_unclaimed_protocol()`
handles its own unclaimed-IPv6-protocol case.

## Malformed or checksum-failing traffic is always dropped silently

This is a separate rule from the port-unreachable case above and takes
priority over it: a segment shorter than `DMUDP_HEADER_LEN`, one whose
declared `length` field doesn't match the segment actually received, or
one that fails checksum verification, is dropped with no reply at all -
regardless of family, regardless of whether the destination port is bound
or not. Only a *well-formed, checksum-valid* datagram addressed to an
unbound port earns a Port Unreachable; nothing here dignifies possibly-
spoofed or corrupted traffic with a reply.

## Why the source address has to be known up front

A UDP checksum is computed over a pseudo-header that includes the
*source* IP address (RFC 768) - but picking a source address is normally
something the IP layer does internally, after a route lookup, right
before it actually sends. `dmudp_send()` can't wait for that: the segment
(including its checksum) has to be fully built *before* it's handed to
`dmip_send()` as a payload.

The fix is `dmip_v4_get_source_address()` - a function `dmip_v4_send()`
already uses internally when its caller leaves the source unset, exposed
publicly for exactly this reason. For an IPv4 destination, `dmudp_send()`
calls it first to learn the source address, builds the segment and its
checksum against that, then calls `dmip_send()` with an explicit
`header.header.v4.src` so the two never disagree.

## One buffer, no extra copy

`dmudp_send()` builds a single buffer shaped `[pseudo-header][UDP
header][payload]` and runs `dmip_checksum()` over the whole thing - the
pseudo-header prefix is never transmitted, only summed. It then hands
`dmip_send()` a pointer into the middle of that same buffer (past the
pseudo-header) rather than allocating and copying a second time.
`dmudp_v4_checksum_valid()`/`_v6_checksum_valid()` do the equivalent for a
received segment: build `[pseudo-header][segment]` once and run
`dmip_checksum()` over it.

## IPv4: checksum 0 means "none"; IPv6: checksum is mandatory

RFC 768 lets an IPv4 UDP sender skip the checksum entirely by writing 0 in
the checksum field. `dmudp_send()` never does this itself (it always
computes a real checksum, remapping a computed value of exactly 0 to
0xFFFF per the RFC), but the receive path respects it on the way in for an
IPv4 datagram - a wire checksum of 0 skips verification rather than being
treated as a mismatch. This exception lives in `dmudp_handle_ip_packet()`,
not in `dmudp_v4_checksum_valid()` itself, which has no awareness of it at
all and would report a wire-0 checksum as invalid if asked to check one.

RFC 8200 removed that allowance for IPv6: the checksum is mandatory, so
for an IPv6 datagram the receive path always calls
`dmudp_v6_checksum_valid()` unconditionally, no special-casing for a wire
value of 0.

## No IPv6 send yet

`dmudp_send()` returns `-ENOSYS` for an IPv6 destination: sending needs
`dmip_send()`'s IPv4 path (`dmip_v4_send()`), which resolves a destination
MAC via ARP - IPv6 needs the NDP equivalent instead, and there is no NDP
module in this tree yet (see
[dmip.md](../../dmip/docs/dmip.md#send--receive)). Receiving has no such
gap - the receive path handles an inbound IPv6 datagram exactly like an
IPv4 one (aside from the mandatory-checksum and no-ICMP-error differences
noted above). Once `dmip_send()` gains an IPv6 path, `dmudp_send()`'s
IPv6 case follows the exact shape of its existing IPv4 one - no new public
function needed.

## Byte buffers, not packed structs

Same reasoning as `dmip.c`/`dmicmp.c`: segments are built/parsed as raw
`uint8_t` buffers indexed by hand, not packed C structs - dmod's minimal
module runtime gives no struct-packing guarantee.

## Dependencies

- `dmip` - `dmip_send()` for transmit, `dmip_register_protocol()`/
  `_unregister_protocol()` to receive, plus `dmip_checksum()` for the
  pseudo-header checksum and `dmip_v4_get_source_address()`/
  `_v4_next_identification()`
- `dmicmp` - `dmicmp_v4_send_dest_unreachable()`, to report a datagram
  addressed to an unbound port back to its sender as a Port Unreachable
- `dmroute` - header-only: `dmip_addr_t`'s real definition
  (`dmroute_addr_t`)
- `dmnetif` - header-only: `dmnetif_iface_t`, passed through to a bound
  `dmudp_datagram_handler_t`. dmudp.c never calls a `dmnetif_*` function
  directly - `dmnetbridge` (via dmip) already does all frame I/O
- `dmlist` - backs the port-binding registry
- `dmosi` - mutex guarding that registry

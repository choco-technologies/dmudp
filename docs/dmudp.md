# DMUDP - DMOD UDP

## Overview

DMUDP builds and parses UDP segments (RFC 768) - source/destination port,
length, and a pseudo-header checksum - and sends/receives them. Sending
calls straight into [dmip](../../dmip)'s family-agnostic `dmip_send()`;
receiving registers itself with dmip as the handler for UDP's IP protocol
number (`dmip_register_protocol()`) rather than pulling packets from dmip
directly - dmip no longer hands packets to whoever asks, it dispatches by
protocol number (see [dmip.md](../../dmip/docs/dmip.md#protocol-dispatch)).
Either way, dmudp itself never talks to dmroute, dmarp, or dmnetif
directly for anything - dmip (and, below it, dmnetbridge) already does
all of that (route lookup, ARP resolution, frame I/O, fragmentation).
Building UDP as a thin layer over an IP layer that can already put a
packet on the wire is the entire reason dmudp exists as its own module
rather than a few more functions bolted onto dmip.

```
┌──────────────────────────────────────────────┐
│                  DMUDP                        │
│   build/parse/checksum a UDP segment,         │
│   own receive queue fed by a registered       │
│   protocol handler, dmudp_send()/_receive()   │
├──────────────────────────────────────────────┤
│                  DMIP                         │
│   dmip_send(), dmip_checksum(),               │
│   protocol registration                       │
├──────────────────────────────────────────────┤
│      DMNETBRIDGE / DMROUTE / DMNETIF / DMARP  │
└──────────────────────────────────────────────┘
```

## One function per direction, not one per family

There is `dmudp_send()` and `dmudp_receive()` - no `dmudp_v4_send()`/
`_v6_send()`, no `dmudp_v4_receive()`/`_v6_receive()`. A caller sending a
datagram only ever has one destination address, and that address already
carries the family that would otherwise decide which of two
same-shaped functions to call - making the caller repeat that choice via
the function name too is pure redundancy, not a real API surface. This is
the same reasoning [dmip.md](../../dmip/docs/dmip.md#family-agnostic-dmip_send)
gives for `dmip_send()` one layer down - dmudp just carries it one step
further and drops the per-family functions altogether rather than
keeping them *and* a wrapper on top.

Receiving is, if anything, an even clearer case: which family a datagram
turns out to be *is* the thing waiting is trying to discover, so it was
never something a caller could have picked by calling a specific function
in the first place - `dmudp_receive()` reports it back via `out_family`.

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

Both `dmudp_send()` and `dmudp_v4_checksum_valid()`/`_v6_checksum_valid()`
build a single buffer shaped `[pseudo-header][UDP header][payload]` and
run `dmip_checksum()` over the whole thing - the pseudo-header prefix is
never transmitted, only summed. `dmudp_send()` goes one step further:
since the segment it needs to send is already sitting right after the
pseudo-header in that same buffer, it hands `dmip_send()` a pointer into
the middle of it rather than allocating and copying a second time.

## IPv4: checksum 0 means "none"; IPv6: checksum is mandatory

RFC 768 lets an IPv4 UDP sender skip the checksum entirely by writing 0 in
the checksum field. `dmudp_send()` never does this itself (it always
computes a real checksum, remapping a computed value of exactly 0 to
0xFFFF per the RFC), but `dmudp_receive()` respects it on the way in for
an IPv4 datagram - a wire checksum of 0 skips verification rather than
being treated as a mismatch.

RFC 8200 removed that allowance for IPv6: the checksum is mandatory, so
for an IPv6 datagram `dmudp_receive()` always calls
`dmudp_v6_checksum_valid()`, no special-casing for a wire value of 0
(which would essentially never happen for a real non-empty checksum sum
anyway, but there's no reason to carve out an exception for it that RFC
8200 itself doesn't allow).

## No IPv6 send yet

`dmudp_send()` returns `-ENOSYS` for an IPv6 destination: sending needs
`dmip_send()`'s IPv4 path (`dmip_v4_send()`), which resolves a destination
MAC via ARP - IPv6 needs the NDP equivalent instead, and there is no NDP
module in this tree yet (see
[dmip.md](../../dmip/docs/dmip.md#send--receive)). Receiving has no such
gap - `dmudp_receive()` handles an inbound IPv6 datagram exactly like an
IPv4 one, verifying its checksum with `dmudp_v6_checksum_valid()` instead.
Once `dmip_send()` gains an IPv6 path, `dmudp_send()`'s `dmip_family_v6`
case follows the exact shape of its existing IPv4 one - no new public
function needed.

## No socket layer, but not stateless anymore

dmudp has no socket/bind concept - there is no `dmudp_socket_create()`/
`bind()`/`close()`, no per-port registry, and `dmudp_receive()` hands back
every datagram regardless of destination port (a caller that wants a
socket-like abstraction - bind to a port, dispatch by destination port -
can build it on top of `out_dst_port` itself).

It does now own a small receive queue, though - a change from how this
module used to work (every call a one-shot send or a one-shot
wait-and-parse, no globals). That's a direct consequence of dmip no
longer keeping a general-purpose receive queue of its own (see
[dmip.md](../../dmip/docs/dmip.md#protocol-dispatch)): once dmip
dispatches by protocol instead of queuing for whoever asks next, whatever
wants a pull/wait-style `_receive()` call has to hold onto arrived data
itself between "dmip handed it a packet" and "a caller actually asked for
it". `dmudp_handle_ip_packet()` (registered via `dmip_register_protocol(DMIP_PROTO_UDP, ...)`
in `dmod_init()`) is the same parsing/checksum logic this module always
had, just triggered by that registration instead of by pulling from dmip -
see `src/dmudp.c`.

## Byte buffers, not packed structs

Same reasoning as `dmip.c`/`dmarp.c`: segments are built/parsed as raw
`uint8_t` buffers indexed by hand, not packed C structs - dmod's minimal
module runtime gives no struct-packing guarantee.

## Dependencies

- `dmip` - `dmip_send()` for transmit, `dmip_register_protocol()`/
  `_unregister_protocol()` to receive, plus `dmip_checksum()` for the
  pseudo-header checksum and `dmip_v4_get_source_address()`/
  `_v4_next_identification()`
- `dmroute` - `dmip_addr_t`'s real definition (`dmroute_addr_t`)
- `dmnetif` - header-only: `dmnetif_iface_t`, passed through to
  `dmudp_handle_ip_packet()` and out through `dmudp_receive()`'s optional
  `out_iface` parameter. dmudp.c never calls a `dmnetif_*` function
  directly - `dmnetbridge` (via dmip) already does all frame I/O
- `dmlist` - backs dmudp's own receive queue (see "No socket layer, but
  not stateless anymore" above)
- `dmosi` - mutex guarding that queue, plus a semaphore `dmudp_receive()`
  waits on and `dmudp_handle_ip_packet()` posts

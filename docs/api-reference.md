# dmudp API Reference

See [dmudp.md](dmudp.md) for the architecture and rationale behind this
module's design.

## Constants

| Constant          | Value | Description                                    |
|--------------------|-------|--------------------------------------------------|
| `DMUDP_HEADER_LEN` | 8     | UDP header length in bytes (source port, destination port, length, checksum) |

## Checksum

| Function | Description |
|----------|--------------|
| `dmudp_v4_checksum_valid(src_ip, dst_ip, segment, segment_len)` | Verify a received IPv4 UDP segment's checksum against the RFC 768 pseudo-header |
| `dmudp_v6_checksum_valid(src_ip, dst_ip, segment, segment_len)` | Verify a received IPv6 UDP segment's checksum against the RFC 8200 pseudo-header |

## Send / receive

No per-family split (`dmudp_v4_send()`/`_v6_send()`,
`dmudp_v4_receive()`/`_v6_receive()`) - see
[dmudp.md](dmudp.md#one-function-per-direction-not-one-per-family) for why.

| Function | Description |
|----------|--------------|
| `dmudp_send(dst_ip, dst_port, src_port, payload, payload_len, arp_timeout_ms)` | Build, checksum and send a UDP datagram to `dst_ip`. IPv4: calls `dmip_v4_get_source_address()` then `dmip_send()`. IPv6: `-ENOSYS` (no IPv6 send path yet - see [dmudp.md](dmudp.md#no-ipv6-send-yet)) |
| `dmudp_receive(timeout_ms, out_family, out_src_ip, out_src_port, out_dst_port, out_payload, out_payload_len, out_iface)` | Wait up to `timeout_ms` for an inbound UDP datagram of either family, reporting which family arrived via `*out_family` and which interface via the optional `out_iface`. `0` on success, `-EAGAIN` on timeout, `-EINVAL` for a NULL required output parameter - a malformed packet or checksum mismatch is dropped before it ever reaches this call (see [dmudp.md](dmudp.md#no-socket-layer-but-not-stateless-anymore)), not reported here |

All addresses use `dmip_addr_t` from [dmip](../../dmip); `dmudp_send()`'s
error codes are passed straight through from `dmip_send()`/
`dmip_v4_send()` except `-EINVAL` for arguments dmudp validates itself -
see `include/dmudp.h` for the full breakdown on each function.

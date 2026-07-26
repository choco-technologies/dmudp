# dmudp API Reference

See [dmudp.md](dmudp.md) for the design rationale behind this API.

## Types

| Type | Description |
|------|-------------|
| `dmudp_header_t` | Parsed UDP header fields: `src_port`, `dst_port`, `length`, `checksum` (parse output only) |
| `dmudp_datagram_handler_t` | Callback registered via `dmudp_bind()`/`_bind_any()`, called inline for every datagram addressed to the bound port |

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DMUDP_HEADER_LEN` | 8 | Length of the UDP header (identical layout for v4 and v6) |
| `DMUDP_MAX_PAYLOAD_LEN` | 65527 | Largest payload `dmudp_send()` can carry (the UDP length field is 16 bits, header included) |
| `DMUDP_PORT_EPHEMERAL_FIRST` | 49152 | First port `dmudp_bind_any()` will consider (RFC 6335 dynamic/private range) |
| `DMUDP_PORT_EPHEMERAL_LAST` | 65535 | Last port `dmudp_bind_any()` will consider |

## Functions

### Header build/parse

| Function | Description |
|----------|-------------|
| `dmudp_build_header()` | Build the 8-byte UDP header into a buffer (checksum field left 0) |
| `dmudp_parse_header()` | Parse the 8-byte UDP header from a buffer |

### Checksum

| Function | Description |
|----------|-------------|
| `dmudp_v4_checksum_valid()` | Verify a UDP-over-IPv4 segment's checksum (IPv4 pseudo-header required, RFC 768) - has no awareness of the "wire checksum 0 means none" allowance, that decision belongs to the receive path |
| `dmudp_v6_checksum_valid()` | Verify a UDP-over-IPv6 segment's checksum (IPv6 pseudo-header required, RFC 8200 8.1 - always mandatory, no exception) |

### Port binding

| Function | Description |
|----------|-------------|
| `dmudp_bind()` | Reserve a specific UDP port and register a handler for it |
| `dmudp_bind_any()` | Reserve the first free port in the ephemeral range and register a handler for it |
| `dmudp_unbind()` | Undo `dmudp_bind()`/`_bind_any()` - safe no-op if the port was never bound |

### Sending

| Function | Description |
|----------|-------------|
| `dmudp_send()` | Build, checksum and send a UDP datagram - one function for both families, `dst`'s own family selects the path |

## What's not here yet

- No `dmudp_send()` for an IPv6 destination (`-ENOSYS`) - blocked on
  `dmip_v6_send()` not existing yet (no NDP module). Receiving an IPv6
  datagram works fully, including mandatory checksum verification.
- No ICMPv6 Port Unreachable - an IPv6 datagram addressed to an unbound
  port is logged (`DMOD_LOG_WARN`) and dropped instead, the same gap
  `dmicmp` documents for its own unclaimed-IPv6-protocol case.

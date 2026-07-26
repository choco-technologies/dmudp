# dmudp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmudp is a [DMOD](https://github.com/choco-technologies/dmod) library module
that builds/parses UDP segments (RFC 768) and sends/receives them by calling
straight into [dmip](https://github.com/choco-technologies/dmip)'s own
family-agnostic send/receive. `dmudp_send()` computes the pseudo-header
checksum, builds the segment, and hands it to `dmip_send()`; `dmudp_receive()`
waits on dmudp's own receive queue (fed by a handler registered with dmip for
UDP's IP protocol number), verifies the checksum, and hands back just the UDP
payload plus which family it arrived as.

## Features

- **RFC 768 UDP segments** - builds and parses the 8-byte header
  (source/destination port, length, checksum) over raw byte buffers, no
  packed structs (dmod's minimal module runtime gives no struct-packing
  guarantee).
- **One function per direction, not one per family** - a single
  `dmudp_send()`/`dmudp_receive()` pair handles both IPv4 and IPv6; see
  [docs/dmudp.md](docs/dmudp.md#one-function-per-direction-not-one-per-family)
  for why a per-family split (`_v4_send()`/`_v6_send()`, ...) would be pure
  redundancy.
- **Correct checksum semantics per RFC** - IPv4 treats a wire checksum of `0`
  as "none computed" (RFC 768); IPv6 treats the checksum as mandatory
  (RFC 8200) and never special-cases `0`.
- **Own receive queue** - `dmudp_receive()` blocks (with a timeout) on a
  queue backed by [dmlist](https://github.com/choco-technologies/dmlist) and
  guarded by a [dmosi](https://github.com/choco-technologies/dmosi)
  mutex/semaphore, fed by a handler registered via
  `dmip_register_protocol()` rather than polling dmip directly.
- **Honest about what isn't implemented yet** - `dmudp_send()` returns
  `-ENOSYS` for an IPv6 destination (no NDP module in this tree yet to
  resolve a destination MAC) instead of silently failing; receiving has no
  such gap and handles either family from the start.

## Quick Start

### Installation

Using `dmf-get` from a DMOD release package:

```bash
dmf-get install dmudp
```

Or link it into another module's `CMakeLists.txt`:

```cmake
dmod_link_modules(${DMOD_MODULE_NAME} dmudp)
```

### Sending a datagram

```c
#include "dmudp.h"

dmip_addr_t peer = { .family = dmip_family_v4, .addr.v4 = { 192, 168, 1, 20 } };

int ret = dmudp_send(&peer, /* dst_port */ 5353, /* src_port */ 12345,
                      "hello", 5, DMARP_DEFAULT_TIMEOUT_MS);
if (ret != 0)
{
    /* -EINVAL: bad argument/family
     * -ENOSYS: dst_ip->family is dmip_family_v6 (no IPv6 send path yet)
     * -ENETUNREACH/-ENODEV: no usable route to dst_ip
     * -EHOSTUNREACH: ARP resolution failed
     * -EMSGSIZE/-ENOMEM/-EIO: see dmip_send()/dmip_v4_send()
     */
}
```

### Receiving a datagram

```c
#include "dmudp.h"

dmip_family_t   src_family;
dmip_addr_t     src_ip;
uint16_t        src_port, dst_port;
uint8_t*        payload;
size_t          payload_len;
dmnetif_iface_t iface;

/* wait up to 1000 ms for a datagram of either family; 0 = check once, don't wait */
int ret = dmudp_receive(1000, &src_family, &src_ip, &src_port, &dst_port,
                         &payload, &payload_len, &iface);
if (ret == 0)
{
    /* use payload ... */
    Dmod_Free(payload); /* caller owns the buffer */
}
else if (ret == -EAGAIN)
{
    /* nothing arrived within the timeout */
}
```

`dmudp_receive()` has no socket/bind concept - it hands back every datagram
regardless of destination port. A caller that wants to filter or dispatch by
`dst_port` builds that on top of `out_dst_port` itself.

### Verifying a checksum by hand

Normally `dmudp_receive()` already verifies the checksum for you, but the
same functions are public for callers parsing a UDP segment themselves:

```c
#include "dmudp.h"

bool ok = dmudp_v4_checksum_valid(&src_ip, &dst_ip, segment, segment_len);
/* dmudp_v6_checksum_valid(...) for an IPv6 segment - mandatory per RFC 8200,
 * unlike IPv4 where a wire checksum of 0 means "none computed" */
```

## API Overview

See [docs/api-reference.md](docs/api-reference.md) for the full reference
(parameters, exact return codes). Every function is plain DMOD Built-in API
(`dmod_dmudp_api`).

| Function | Description |
|----------|--------------|
| `dmudp_send(dst_ip, dst_port, src_port, payload, payload_len, arp_timeout_ms)` | Build, checksum and send a UDP datagram. Returns `0` on success; `-ENOSYS` for an IPv6 destination. |
| `dmudp_receive(timeout_ms, out_family, out_src_ip, out_src_port, out_dst_port, out_payload, out_payload_len, out_iface)` | Wait up to `timeout_ms` for an inbound datagram of either family. Returns `0` on success, `-EAGAIN` on timeout. `out_payload` is heap-allocated - free with `Dmod_Free()`. `out_iface` is optional (pass `NULL` if not needed). |
| `dmudp_v4_checksum_valid(src_ip, dst_ip, segment, segment_len)` | Verify a received IPv4 UDP segment's checksum against the RFC 768 pseudo-header. |
| `dmudp_v6_checksum_valid(src_ip, dst_ip, segment, segment_len)` | Verify a received IPv6 UDP segment's checksum against the RFC 8200 pseudo-header (mandatory, no "0 means none" exception). |
| `DMUDP_HEADER_LEN` | Constant, `8` - length of a UDP header in bytes. |

## Building

dmudp is a standalone DMOD module - its `CMakeLists.txt` fetches `dmod`
itself via CMake's `FetchContent` (no parent repo needed):

```bash
mkdir -p build
cd build
cmake .. -DDMOD_MODE=DMOD_MODULE
cmake --build .
```

This produces `build/dmf/dmudp.dmf` (and `build/dmf/test_dmudp.dmf`, the test
module - see [Running the tests](#running-the-tests) below).

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

## Running the tests

Tests are a regular DMOD test module (`test_dmudp`, see
[tests/dmudp_test.c](tests/dmudp_test.c)) built alongside the library and run
with `dmod_loader`. After [building](#building):

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf

# fetch this module's own runtime dependencies (dmip, dmnetbridge, dmarp, dmnetif, dmroute)
dmf-get install -d ${DMOD_DMF_DIR}/test_dmudp-local.dmd -y

# run the tests
dmod_loader build/dmf/test_dmudp.dmf
```

The test binary's exit code equals the number of failed test steps (`0` means
everything passed), which is what [.github/workflows/ci.yml](.github/workflows/ci.yml)
checks in CI. The test suite exercises checksum round-trips for both
families, argument validation, and the full send/receive path against a pair
of `/null`-backed `dmnetif` fixtures (no real driver needed - see the file
comment in [tests/dmudp_test.c](tests/dmudp_test.c) for exactly how far a
send can be exercised without one).

## Dependencies

Pulled in automatically via `dmod_link_modules()` in
[CMakeLists.txt](CMakeLists.txt) - nothing to install by hand beyond `dmod`
itself:

| Module | Used for |
|--------|----------|
| [dmip](https://github.com/choco-technologies/dmip) | `dmip_send()`, `dmip_checksum()`, `dmip_register_protocol()`, `dmip_addr_t` |
| [dmnetif](https://github.com/choco-technologies/dmnetif) | `dmnetif_iface_t` - `dmudp_receive()`'s optional `out_iface` parameter |
| [dmroute](https://github.com/choco-technologies/dmroute) | `dmip_addr_t`'s real definition (`dmroute_addr_t`) |
| [dmlist](https://github.com/choco-technologies/dmlist) | Backs dmudp's own receive queue |
| [dmosi](https://github.com/choco-technologies/dmosi) | Mutex guarding that queue, plus the semaphore `dmudp_receive()` waits on |

## Documentation

See the `docs/` directory:

- **[dmudp.md](docs/dmudp.md)** - Overview and rationale
- **[api-reference.md](docs/api-reference.md)** - Full API reference

View documentation using `dmf-man`:

```bash
dmf-man dmudp                # Main documentation
dmf-man dmudp api-reference  # API reference
```

## Project Structure

```
dmudp/
├── docs/              # Documentation (markdown format)
├── include/
│   └── dmudp.h        # Public API
├── src/
│   └── dmudp.c        # Implementation
├── tests/
│   ├── CMakeLists.txt
│   └── dmudp_test.c
├── scripts/
│   └── sync-claude.sh
├── CMakeLists.txt
├── Makefile           # Make-based equivalent build
├── manifest.dmm       # dmf-get download manifest
├── dmudp.dmr          # DMOD resource file (install/package mapping)
└── LICENSE
```

## Related Projects

- [DMOD](https://github.com/choco-technologies/dmod) - the loader/runtime and build system this module is built on
- [dmip](https://github.com/choco-technologies/dmip) - the IP layer dmudp sends/receives through
- [dmarp](https://github.com/choco-technologies/dmarp) - ARP resolution used by dmip's IPv4 send path

## Author

Patryk Kubiak

## License

This project is licensed under the MIT License - dmudp is its own top-level
repository with its own [LICENSE](LICENSE) file (previously it lived nested
under the `dmnet` repository and shared that repo's license - see the git
history for the move). See [LICENSE](LICENSE) for the full text.

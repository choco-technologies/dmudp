# dmudp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmudp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmudp/actions/workflows/ci.yml)

dmudp DMOD library module.

## Description

dmudp builds and parses UDP segments (RFC 768) on top of
[dmip](https://github.com/choco-technologies/dmip): send a datagram with
`dmudp_send()`, and receive datagrams addressed to a chosen port by
registering a handler for it with `dmudp_bind()` (a specific port) or
`dmudp_bind_any()` (the first free ephemeral port). A bound handler is
invoked inline, on whatever thread is pumping the interface the datagram
arrived on, so it can reply synchronously without needing its own thread
or queue. A datagram addressed to a port with no registered handler is
reported back to its sender as an ICMP Port Unreachable (IPv4) via
[dmicmp](https://github.com/choco-technologies/dmicmp). See
[docs/dmudp.md](docs/dmudp.md) for the full design rationale.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmudp-local.dmd -y
dmod_loader build/dmf/test_dmudp.dmf
```

## Usage

This library module provides functions that can be used by other modules:

```c
#include "dmudp.h"

static void on_datagram(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port,
                         dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len)
{
    /* Runs inline on the netif rx thread - reply synchronously if needed: */
    dmudp_send(src, dst_port, src_port, payload, payload_len, 1000u /* arp_timeout_ms */);
}

uint16_t port = 0;
dmudp_bind_any(on_datagram, &port);
```

## API

| Function | Description |
|----------|-------------|
| `dmudp_build_header()` / `_parse_header()` | Build/parse the 8-byte UDP header |
| `dmudp_v4_checksum_valid()` / `_v6_checksum_valid()` | Verify a received segment's pseudo-header checksum |
| `dmudp_bind()` | Reserve a specific UDP port and register a handler for it |
| `dmudp_bind_any()` | Reserve the first free ephemeral port and register a handler for it |
| `dmudp_unbind()` | Release a port reserved by `_bind()`/`_bind_any()` |
| `dmudp_send()` | Build, checksum and send a UDP datagram |

See [include/dmudp.h](include/dmudp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmudp`.
## Project Structure

```
dmudp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmudp.h
├── src/
│   └── dmudp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmudp_test.c
├── CMakeLists.txt
├── Makefile
├── dmudp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT

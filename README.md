# dmudp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

dmudp DMOD library module - builds/parses UDP segments (RFC 768) and
sends/receives them by calling straight into [dmip](../dmip)'s own
family-agnostic send/receive. `dmudp_send()` computes the pseudo-header
checksum, builds the segment, and hands it to `dmip_send()` (IPv6
destinations get `-ENOSYS` for now - no NDP module to resolve a
destination MAC through yet); `dmudp_receive()` polls `dmip_receive()`,
verifies the checksum, and hands back just the UDP payload plus which
family it was. One function per direction, not one per family - see
[docs/dmudp.md](docs/dmudp.md) for why.

## Building

This module lives under `lib/dmudp` inside the `dmnet` repository and is
built as part of the parent's CMake configure (the top-level
`CMakeLists.txt` calls `add_subdirectory(lib)`, whose own `CMakeLists.txt`
calls `add_subdirectory(dmudp)` after dmip, the module it depends on) - it
is not built standalone.

```bash
mkdir -p build
cd build
cmake ..
cmake --build . --target dmudp
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

## Usage

```c
#include "dmudp.h"

dmip_addr_t peer = { .family = dmip_family_v4, .addr.v4 = { 192, 168, 1, 20 } };
dmudp_send(&peer, 5353, 12345, "hello", 5, DMARP_DEFAULT_TIMEOUT_MS);

dmip_family_t from_family;
dmip_addr_t from_ip;
uint16_t from_port, to_port;
uint8_t* payload;
size_t payload_len;
if (dmudp_receive(iface, &from_family, &from_ip, &from_port, &to_port, &payload, &payload_len) == 0)
{
    /* use payload ... */
    Dmod_Free(payload);
}
```

## Documentation

See the `docs/` directory:

- **[dmudp.md](docs/dmudp.md)** - Overview and rationale
- **[api-reference.md](docs/api-reference.md)** - Full API reference

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
└── dmudp.dmr
```

LICENSE is shared with the rest of the `dmnet` repository (`../../LICENSE`) -
see `dmudp.dmr` for how it's picked up during packaging.

## Author

Patryk Kubiak

## License

MIT

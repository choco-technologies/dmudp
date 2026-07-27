/**
 * @file dmudp.c
 * @brief DMOD UDP - Implementation
 *
 * Four loosely-coupled pieces live here, in the same order dmicmp.c uses
 * for its own analogous pieces:
 *
 *  - Header build/parse and checksum (both UDP-over-IPv4 and UDP-over-IPv6
 *    need a pseudo-header, unlike ICMPv4 - RFC 768 / RFC 8200 8.1 - so
 *    unlike dmicmp.c's asymmetric v4/v6 checksum shape, dmudp's two
 *    checksum functions are symmetric: both take src_ip/dst_ip).
 *
 *  - Port binding: dmudp_bind()/_bind_any()/_unbind() manage a small
 *    dmlist-backed table of { port, handler } entries guarded by
 *    g_bind_mutex - the same shape dmicmp.c's own echo listener table
 *    uses, just keyed by UDP port instead of ICMP echo identifier.
 *
 *  - Sending: dmudp_send()/_send_on_iface() share udp_send_common(), which
 *    builds a single [pseudo-header][UDP header][payload] buffer, checksums
 *    the whole thing, and hands dmip_send()/dmip_v4_send_on_iface() a
 *    pointer into the middle of it (no second copy) - only the IPv4 path
 *    exists, same as dmicmp/dmip's own IPv6 send gap (no dmip_v6_send()
 *    yet). dmudp_send_on_iface() bypasses routing (see dmudp.h) for
 *    traffic like a DHCP client's pre-lease broadcast.
 *
 *  - Receiving: dmudp registers dmudp_handle_ip_packet() with dmip for
 *    DMIP_PROTO_UDP (see dmod_init()). A well-formed, checksum-valid
 *    datagram addressed to a bound port is delivered to that port's
 *    handler synchronously, inline, right there on whatever thread is
 *    pumping the interface - no queue, no worker thread, mirroring how
 *    dmicmp answers an Echo Request inline (see dmicmp.h). A datagram
 *    addressed to an unbound port gets an ICMPv4 Port Unreachable via
 *    dmicmp_v4_send_dest_unreachable() (IPv6: logged and dropped, no
 *    ICMPv6 send capability yet). Anything malformed or failing checksum
 *    is dropped silently, regardless of family - no ICMP reply for
 *    possibly-spoofed or corrupt traffic.
 *
 * See docs/dmudp.md.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmudp.h"
#include "dmicmp.h"
#include "dmlist.h"
#include "dmosi.h"
#include "dmnetif.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Default ARP-resolution timeout for a reply dmudp sends on its own
 *        initiative (the automatic ICMP Port Unreachable) - mirrors
 *        dmicmp's own DMICMP_DEFAULT_ARP_TIMEOUT_MS, duplicated here
 *        rather than linking dmarp just for one constant (dmudp only
 *        depends on dmip/dmicmp - see CMakeLists.txt)
 */
#define DMUDP_DEFAULT_ARP_TIMEOUT_MS 1000u

#define DMUDP_V4_PSEUDO_HEADER_LEN 12u
#define DMUDP_V6_PSEUDO_HEADER_LEN 40u

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* ============================================================================
 *                      Common header
 * ========================================================================== */

/**
 * @brief Implementation of dmudp_build_header() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, int, _build_header, ( uint8_t* buffer, size_t buffer_len, const dmudp_header_t* header ))
{
    if (buffer == NULL || header == NULL || buffer_len < DMUDP_HEADER_LEN)
        return -EINVAL;

    write_u16_be(&buffer[0], header->src_port);
    write_u16_be(&buffer[2], header->dst_port);
    write_u16_be(&buffer[4], header->length);
    write_u16_be(&buffer[6], 0); /* checksum - filled in by the caller once the payload is known, see dmudp_send() */
    return 0;
}

/**
 * @brief Implementation of dmudp_parse_header() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, int, _parse_header, ( const uint8_t* buffer, size_t length, dmudp_header_t* header ))
{
    if (buffer == NULL || header == NULL || length < DMUDP_HEADER_LEN)
        return -EINVAL;

    header->src_port = read_u16_be(&buffer[0]);
    header->dst_port = read_u16_be(&buffer[2]);
    header->length   = read_u16_be(&buffer[4]);
    header->checksum = read_u16_be(&buffer[6]);
    return 0;
}

/* ============================================================================
 *                      Checksum
 * ========================================================================== */

/**
 * @brief Write the 12-byte IPv4 UDP pseudo-header (RFC 768) into `buf`
 */
static void write_v4_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint16_t segment_len)
{
    memcpy(&buf[0], src_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&buf[4], dst_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    buf[8] = 0;
    buf[9] = DMIP_PROTO_UDP;
    write_u16_be(&buf[10], segment_len);
}

/**
 * @brief Write the 40-byte IPv6 pseudo-header (RFC 8200 8.1) into `buf`
 */
static void write_v6_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint32_t segment_len)
{
    memcpy(&buf[0], src_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&buf[16], dst_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    buf[32] = (uint8_t)(segment_len >> 24);
    buf[33] = (uint8_t)(segment_len >> 16);
    buf[34] = (uint8_t)(segment_len >> 8);
    buf[35] = (uint8_t)(segment_len & 0xFFu);
    buf[36] = 0;
    buf[37] = 0;
    buf[38] = 0;
    buf[39] = DMIP_PROTO_UDP;
}

/**
 * @brief Implementation of dmudp_v4_checksum_valid() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, bool, _v4_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || length < DMUDP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v4 || dst_ip->family != dmip_family_v4)
        return false;

    size_t total = DMUDP_V4_PSEUDO_HEADER_LEN + length;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return false;

    write_v4_pseudo_header(buf, src_ip, dst_ip, (uint16_t)length);
    memcpy(buf + DMUDP_V4_PSEUDO_HEADER_LEN, segment, length);

    bool valid = dmip_checksum(buf, total) == 0;
    Dmod_Free(buf);
    return valid;
}

/**
 * @brief Implementation of dmudp_v6_checksum_valid() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || length < DMUDP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v6 || dst_ip->family != dmip_family_v6)
        return false;

    size_t total = DMUDP_V6_PSEUDO_HEADER_LEN + length;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return false;

    write_v6_pseudo_header(buf, src_ip, dst_ip, (uint32_t)length);
    memcpy(buf + DMUDP_V6_PSEUDO_HEADER_LEN, segment, length);

    bool valid = dmip_checksum(buf, total) == 0;
    Dmod_Free(buf);
    return valid;
}

/* ============================================================================
 *                      Port binding
 *
 * A dmlist of struct dmudp_port_binding*, guarded by g_bind_mutex - the
 * same shape dmicmp.c's own echo listener table uses (see dmicmp.c's
 * "Echo listener registry" section), just keyed by UDP port instead of
 * ICMP echo identifier.
 * ========================================================================== */

struct dmudp_port_binding
{
    uint16_t                 port;
    dmudp_datagram_handler_t handler;
};

static dmlist_context_t* g_bindings = NULL;
static dmosi_mutex_t     g_bind_mutex = NULL;
static uint16_t          g_next_ephemeral_port = DMUDP_PORT_EPHEMERAL_FIRST;

/**
 * @brief dmlist_compare_func_t matching a struct dmudp_port_binding against
 *        a `const uint16_t*` port needle
 */
static int compare_port(const void* data, const void* user_data)
{
    const struct dmudp_port_binding* entry = (const struct dmudp_port_binding*)data;
    uint16_t port = *(const uint16_t*)user_data;
    return (entry->port == port) ? 0 : -1;
}

static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

/**
 * @brief Implementation of dmudp_bind() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, int, _bind, ( uint16_t port, dmudp_datagram_handler_t handler ))
{
    if (handler == NULL || port == 0)
        return -EINVAL;

    dmosi_mutex_lock(g_bind_mutex);

    int result;
    if (dmlist_find(g_bindings, &port, compare_port) != NULL)
    {
        result = -EEXIST;
    }
    else
    {
        struct dmudp_port_binding* entry = Dmod_Malloc(sizeof(*entry));
        if (entry == NULL)
        {
            result = -ENOMEM;
        }
        else
        {
            entry->port = port;
            entry->handler = handler;
            if (dmlist_push_back(g_bindings, entry))
            {
                result = 0;
            }
            else
            {
                Dmod_Free(entry);
                result = -ENOMEM;
            }
        }
    }

    dmosi_mutex_unlock(g_bind_mutex);
    return result;
}

/**
 * @brief Implementation of dmudp_bind_any() - see dmudp.h
 *
 * Scan and insert happen under one uninterrupted hold of g_bind_mutex -
 * two concurrent calls must never be able to both pick the same port. The
 * scan starts at a rotating cursor (g_next_ephemeral_port) rather than
 * always restarting at DMUDP_PORT_EPHEMERAL_FIRST, the same reason a real
 * OS's ephemeral port allocator avoids instant reuse and an O(range)
 * rescan-from-start on every call.
 */
dmod_dmudp_api_declaration(1.0, int, _bind_any, ( dmudp_datagram_handler_t handler, uint16_t* out_port ))
{
    if (handler == NULL || out_port == NULL)
        return -EINVAL;

    dmosi_mutex_lock(g_bind_mutex);

    int result = -EADDRNOTAVAIL;
    uint32_t range = (uint32_t)DMUDP_PORT_EPHEMERAL_LAST - (uint32_t)DMUDP_PORT_EPHEMERAL_FIRST + 1u;
    for (uint32_t i = 0; i < range; i++)
    {
        uint16_t candidate = g_next_ephemeral_port;
        g_next_ephemeral_port = (candidate == DMUDP_PORT_EPHEMERAL_LAST) ? DMUDP_PORT_EPHEMERAL_FIRST : (uint16_t)(candidate + 1u);

        if (dmlist_find(g_bindings, &candidate, compare_port) != NULL)
            continue;

        struct dmudp_port_binding* entry = Dmod_Malloc(sizeof(*entry));
        if (entry == NULL)
        {
            result = -ENOMEM;
            break;
        }

        entry->port = candidate;
        entry->handler = handler;
        if (!dmlist_push_back(g_bindings, entry))
        {
            Dmod_Free(entry);
            result = -ENOMEM;
            break;
        }

        *out_port = candidate;
        result = 0;
        break;
    }

    dmosi_mutex_unlock(g_bind_mutex);
    return result;
}

/**
 * @brief Implementation of dmudp_unbind() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, void, _unbind, ( uint16_t port ))
{
    dmosi_mutex_lock(g_bind_mutex);

    struct dmudp_port_binding* entry = (struct dmudp_port_binding*)dmlist_find(g_bindings, &port, compare_port);
    if (entry != NULL)
    {
        dmlist_remove(g_bindings, entry, compare_pointer);
        Dmod_Free(entry);
    }

    dmosi_mutex_unlock(g_bind_mutex);
}

/* ============================================================================
 *                      Sending
 * ========================================================================== */

/**
 * @brief Shared implementation behind dmudp_send()/_send_on_iface()
 *
 * Builds one [pseudo-header][UDP header][payload] buffer, checksums the
 * whole thing, and hands dmip_send()/dmip_v4_send_on_iface() a pointer into
 * the middle of it (past the pseudo-header prefix, which is never
 * transmitted) rather than allocating and copying a second time. Unlike
 * dmicmp's send path (which leaves header.src unset and lets dmip_v4_send()
 * pick it), the source address is needed here *before* the segment can be
 * built - the pseudo-header checksum covers it.
 *
 * `iface == NULL` is dmudp_send()'s routed behavior: the source address
 * comes from dmip_v4_get_source_address(), and its error (e.g.
 * -ENETUNREACH for a destination with no route yet) is returned as-is,
 * before any buffer is even allocated. `iface != NULL` is
 * dmudp_send_on_iface()'s routing bypass: the source address comes
 * straight from dmnetif_get_ip_address() on the given interface - which
 * never fails the send (an iface with no IP configured yet reports
 * 0.0.0.0, a legitimate v4 source value for the checksum, same as DHCP's
 * own initial broadcast) - and the packet goes out via
 * dmip_v4_send_on_iface(), which treats `dst` as on-link and skips the
 * dmroute lookup entirely.
 */
static int udp_send_common(dmnetif_iface_t iface, const dmip_addr_t* dst, uint16_t src_port, uint16_t dst_port,
                            const void* payload, size_t payload_len, uint32_t arp_timeout_ms)
{
    if (dst == NULL || dst_port == 0 || (payload == NULL && payload_len > 0) || payload_len > DMUDP_MAX_PAYLOAD_LEN)
        return -EINVAL;
    if (dst->family == dmip_family_v6)
        return -ENOSYS; /* no dmip_v6_send() yet - see dmudp.h/dmip.h */
    if (dst->family != dmip_family_v4)
        return -EINVAL;

    dmip_addr_t src_ip = { 0 };
    if (iface != NULL)
    {
        dmnetif_get_ip_address(iface, &src_ip);
        src_ip.family = dmip_family_v4;
    }
    else
    {
        int result = dmip_v4_get_source_address(dst, &src_ip);
        if (result != 0)
            return result;
    }

    size_t udp_len = DMUDP_HEADER_LEN + payload_len;
    size_t total = DMUDP_V4_PSEUDO_HEADER_LEN + udp_len;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return -ENOMEM;

    write_v4_pseudo_header(buf, &src_ip, dst, (uint16_t)udp_len);

    uint8_t* segment = buf + DMUDP_V4_PSEUDO_HEADER_LEN;
    dmudp_header_t header = { .src_port = src_port, .dst_port = dst_port, .length = (uint16_t)udp_len };
    dmudp_build_header(segment, udp_len, &header);
    if (payload_len > 0)
    {
        memcpy(segment + DMUDP_HEADER_LEN, payload, payload_len);
    }

    uint16_t checksum = dmip_checksum(buf, total);
    if (checksum == 0)
    {
        checksum = 0xFFFFu; /* RFC 768: a computed 0 is remapped, since 0 on the wire means "no checksum" */
    }
    write_u16_be(&segment[6], checksum);

    dmip_header_t ip_header = { 0 };
    ip_header.family = dmip_family_v4;
    ip_header.header.v4.ttl = DMIP_DEFAULT_TTL;
    ip_header.header.v4.protocol = DMIP_PROTO_UDP;
    ip_header.header.v4.identification = dmip_v4_next_identification();
    ip_header.header.v4.src = src_ip;
    ip_header.header.v4.dst = *dst;

    int result = (iface != NULL)
        ? dmip_v4_send_on_iface(iface, &ip_header.header.v4, segment, udp_len, arp_timeout_ms)
        : dmip_send(&ip_header, segment, udp_len, arp_timeout_ms);

    Dmod_Free(buf);
    return result;
}

/**
 * @brief Implementation of dmudp_send() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, int, _send, ( const dmip_addr_t* dst, uint16_t src_port, uint16_t dst_port, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ))
{
    return udp_send_common(NULL, dst, src_port, dst_port, payload, payload_len, arp_timeout_ms);
}

/**
 * @brief Implementation of dmudp_send_on_iface() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, int, _send_on_iface, ( dmnetif_iface_t iface, const dmip_addr_t* dst, uint16_t src_port, uint16_t dst_port,
    const void* payload, size_t payload_len, uint32_t arp_timeout_ms ))
{
    if (iface == NULL)
        return -EINVAL;
    return udp_send_common(iface, dst, src_port, dst_port, payload, payload_len, arp_timeout_ms);
}

/* ============================================================================
 *                      Receiving
 * ========================================================================== */

/**
 * @brief Look up `dst_port` in g_bindings and either call its handler or
 *        report the datagram as undeliverable - shared by the v4 and v6
 *        receive paths
 *
 * Mutex is held only for the lookup, released before calling out (same
 * "don't hold the lock into another module's/caller's code" reasoning
 * dmicmp.c's deliver_echo_reply() documents for itself), so a handler is
 * free to call dmudp_bind()/_unbind() again from inside itself.
 */
static void dispatch_to_port(dmip_family_t family, dmnetif_iface_t iface, const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port,
                              const uint8_t* payload, size_t payload_len, const uint8_t* original_packet, size_t original_packet_len)
{
    dmosi_mutex_lock(g_bind_mutex);
    struct dmudp_port_binding* entry = (struct dmudp_port_binding*)dmlist_find(g_bindings, &dst_port, compare_port);
    dmudp_datagram_handler_t handler = (entry != NULL) ? entry->handler : NULL;
    dmosi_mutex_unlock(g_bind_mutex);

    if (handler != NULL)
    {
        handler(src, src_port, dst_port, iface, payload, payload_len);
        return;
    }

    if (family == dmip_family_v4)
    {
        dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_port, original_packet, original_packet_len, DMUDP_DEFAULT_ARP_TIMEOUT_MS);
    }
    else
    {
        /* No ICMPv6 send capability yet - same gap dmicmp_handle_unclaimed_protocol()
         * documents for its own unclaimed-IPv6-protocol case. */
        DMOD_LOG_WARN("dmudp: received IPv6 datagram for unbound port %u, cannot send ICMPv6 port-unreachable - no dmip_v6_send() yet\n", (unsigned)dst_port);
    }
}

/**
 * @brief dmip_protocol_handler_t registered for DMIP_PROTO_UDP - see
 *        dmod_init()
 *
 * Parses `packet`'s IP header to locate the UDP segment and read
 * `src`/`dst`, validates the segment's declared length and checksum
 * (dropping silently on any failure - no ICMP reply for possibly-spoofed
 * or corrupt traffic), then dispatches to whichever port the datagram is
 * addressed to. `packet` is borrowed (see dmip_protocol_handler_t's own
 * doc comment in dmip.h) - nothing here keeps a pointer into it past the
 * call other than what dispatch_to_port() passes along inline to a
 * handler/dmicmp_v4_send_dest_unreachable(), both of which return before
 * this function does.
 */
static void dmudp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    if (family == dmip_family_v4)
    {
        dmip_v4_header_t ip_header = { 0 };
        size_t header_len = 0;
        if (dmip_v4_parse_header(packet, packet_len, &ip_header, &header_len) != 0)
            return;

        const uint8_t* segment = packet + header_len;
        size_t segment_len = packet_len - header_len;
        if (segment_len < DMUDP_HEADER_LEN)
            return;

        dmudp_header_t header = { 0 };
        if (dmudp_parse_header(segment, segment_len, &header) != 0)
            return;
        if (header.length < DMUDP_HEADER_LEN || header.length > segment_len)
            return;

        /* RFC 768: a wire checksum of 0 means "no checksum", skip verification -
         * this exception applies only to IPv4, and lives here (not inside
         * dmudp_v4_checksum_valid() itself). */
        if (header.checksum != 0 && !dmudp_v4_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        dispatch_to_port(dmip_family_v4, iface, &ip_header.src, header.src_port, header.dst_port,
                          segment + DMUDP_HEADER_LEN, segment_len - DMUDP_HEADER_LEN, packet, packet_len);
    }
    else /* dmip_family_v6 */
    {
        dmip_v6_header_t ip_header = { 0 };
        if (dmip_v6_parse_header(packet, packet_len, &ip_header) != 0)
            return;

        const uint8_t* segment = packet + DMIP_V6_HEADER_LEN;
        size_t segment_len = packet_len - DMIP_V6_HEADER_LEN;
        if (segment_len < DMUDP_HEADER_LEN)
            return;

        dmudp_header_t header = { 0 };
        if (dmudp_parse_header(segment, segment_len, &header) != 0)
            return;
        if (header.length < DMUDP_HEADER_LEN || header.length > segment_len)
            return;

        /* RFC 8200 8.1: mandatory, no "0 means none" allowance - always verify. */
        if (!dmudp_v6_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        dispatch_to_port(dmip_family_v6, iface, &ip_header.src, header.src_port, header.dst_port,
                          segment + DMUDP_HEADER_LEN, segment_len - DMUDP_HEADER_LEN, packet, packet_len);
    }
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the port-binding table and its
 *        guarding mutex, then registers with dmip
 */
int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    g_bindings = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_bind_mutex = dmosi_mutex_create(false);
    if (g_bindings == NULL || g_bind_mutex == NULL)
    {
        DMOD_LOG_ERROR("Failed to allocate dmudp state\n");
        return -1;
    }

    int result = dmip_register_protocol(DMIP_PROTO_UDP, dmudp_handle_ip_packet);
    if (result != 0)
    {
        DMOD_LOG_ERROR("dmudp: cannot register as the UDP protocol handler (%d)\n", result);
        return -1;
    }

    DMOD_LOG_INFO("DMUDP initialized\n");
    return 0;
}

/**
 * @brief Module deinitialization - unregisters from dmip, frees every
 *        remaining port binding, then the table and mutex
 */
int dmod_deinit(void)
{
    dmip_unregister_protocol(DMIP_PROTO_UDP);

    size_t count = dmlist_size(g_bindings);
    for (size_t i = 0; i < count; i++)
    {
        Dmod_Free(dmlist_pop_front(g_bindings));
    }
    dmlist_destroy(g_bindings);
    g_bindings = NULL;

    dmosi_mutex_destroy(g_bind_mutex);
    g_bind_mutex = NULL;

    DMOD_LOG_INFO("DMUDP deinitialized\n");
    return 0;
}

/**
 * @file dmudp.c
 * @brief DMOD UDP - Implementation
 *
 * Sending is still a one-shot build+checksum+send with no state of its
 * own. Receiving is no longer poll-based: dmudp registers itself with
 * dmip as the handler for DMIP_PROTO_UDP (dmip_register_protocol(), in
 * dmod_init()) and owns a small receive queue of its own
 * (g_rx_queue/g_rx_mutex/g_rx_signal) - dmudp_handle_ip_packet() is
 * called by dmip for every completed UDP packet (from whatever thread
 * happens to be pumping the interface it arrived on), parses out the
 * datagram and pushes it onto that queue; dmudp_receive() waits on it.
 * See dmip.h's "Protocol registration" section for why dmip dispatches
 * by protocol number instead of handing every packet to whoever last
 * called a generic receive function.
 *
 * Segments are built/parsed as raw byte buffers rather than packed C
 * structs, same reasoning as dmip.c/dmarp.c (dmod's minimal module
 * runtime gives no struct-packing guarantee).
 *
 * The IPv4/IPv6 pseudo-header (RFC 768 / RFC 8200 8.1) is never
 * transmitted on its own - it only exists to be summed alongside the
 * segment. Both _send() and _checksum_valid() build one buffer shaped
 * [pseudo-header][segment], run dmip_checksum() over the whole thing, and
 * (for _send()) reuse the segment portion of that same buffer as the
 * payload handed to dmip_v4_send() - no separate copy needed.
 */
#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmudp.h"
#include "dmlist.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

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

/**
 * @brief One received UDP datagram waiting to be picked up by
 *        dmudp_receive() - see g_rx_queue
 */
struct dmudp_rx_entry
{
    dmip_family_t   family;
    dmnetif_iface_t iface;
    dmip_addr_t     src_ip;
    uint16_t        src_port;
    uint16_t        dst_port;
    uint8_t*        payload;
    size_t          payload_len;
};

/**
 * @brief Queue of dmudp_rx_entry* waiting to be picked up by
 *        dmudp_receive() - same shape dmip.c's own receive queue used to
 *        be (dmlist_context_t* + dmosi_mutex_t + dmosi_semaphore_t, not a
 *        fixed-size dmosi_queue_t), fed by dmudp_handle_ip_packet()
 */
static dmlist_context_t* g_rx_queue = NULL;
static dmosi_mutex_t     g_rx_mutex = NULL;

/**
 * @brief Posted once per entry pushed onto g_rx_queue
 */
static dmosi_semaphore_t g_rx_signal = NULL;

/**
 * @brief Write the 12-byte IPv4 pseudo-header (RFC 768) into `buf`
 */
static void write_v4_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint16_t udp_length)
{
    memcpy(&buf[0], src_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&buf[4], dst_ip->addr.v4, DMIP_IPV4_ADDR_LEN);
    buf[8] = 0;
    buf[9] = DMIP_PROTO_UDP;
    write_u16_be(&buf[10], udp_length);
}

/**
 * @brief Write the 40-byte IPv6 pseudo-header (RFC 8200 8.1) into `buf`
 */
static void write_v6_pseudo_header(uint8_t* buf, const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, uint32_t udp_length)
{
    memcpy(&buf[0], src_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&buf[16], dst_ip->addr.v6, DMIP_IPV6_ADDR_LEN);
    buf[32] = (uint8_t)(udp_length >> 24);
    buf[33] = (uint8_t)(udp_length >> 16);
    buf[34] = (uint8_t)(udp_length >> 8);
    buf[35] = (uint8_t)(udp_length & 0xFFu);
    buf[36] = 0;
    buf[37] = 0;
    buf[38] = 0;
    buf[39] = DMIP_PROTO_UDP;
}

/**
 * @brief Implementation of dmudp_v4_checksum_valid() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, bool, _v4_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t segment_len ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || segment_len < DMUDP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v4 || dst_ip->family != dmip_family_v4)
        return false;

    size_t total = DMUDP_V4_PSEUDO_HEADER_LEN + segment_len;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return false;

    write_v4_pseudo_header(buf, src_ip, dst_ip, (uint16_t)segment_len);
    memcpy(buf + DMUDP_V4_PSEUDO_HEADER_LEN, segment, segment_len);

    bool valid = dmip_checksum(buf, total) == 0;
    Dmod_Free(buf);
    return valid;
}

/**
 * @brief Implementation of dmudp_v6_checksum_valid() - see dmudp.h
 */
dmod_dmudp_api_declaration(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t segment_len ))
{
    if (src_ip == NULL || dst_ip == NULL || segment == NULL || segment_len < DMUDP_HEADER_LEN)
        return false;
    if (src_ip->family != dmip_family_v6 || dst_ip->family != dmip_family_v6)
        return false;

    size_t total = DMUDP_V6_PSEUDO_HEADER_LEN + segment_len;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return false;

    write_v6_pseudo_header(buf, src_ip, dst_ip, (uint32_t)segment_len);
    memcpy(buf + DMUDP_V6_PSEUDO_HEADER_LEN, segment, segment_len);

    bool valid = dmip_checksum(buf, total) == 0;
    Dmod_Free(buf);
    return valid;
}

/**
 * @brief Implementation of dmudp_send() - see dmudp.h
 *
 * No separate dmudp_v4_send()/_v6_send(): a caller only ever has one
 * `dst_ip`, already carrying the family that decides everything else
 * here, so a second, function-name-level choice on top of that would be
 * pure redundancy - see dmip_send()'s own doc comment for the same
 * reasoning one layer down.
 */
dmod_dmudp_api_declaration(1.0, int, _send, ( const dmip_addr_t* dst_ip, uint16_t dst_port, uint16_t src_port, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ))
{
    if (dst_ip == NULL || (payload == NULL && payload_len > 0))
        return -EINVAL;

    if (dst_ip->family == dmip_family_v6)
    {
        /* No IPv6 send path yet - blocked on dmip_send()'s own
         * dmip_family_v6 case, itself blocked on there being no NDP
         * module (see dmudp.h's file comment). */
        return -ENOSYS;
    }

    if (dst_ip->family != dmip_family_v4)
        return -EINVAL;

    dmip_addr_t src_ip = { 0 };
    int result = dmip_v4_get_source_address(dst_ip, &src_ip);
    if (result != 0)
        return result;

    size_t udp_len = DMUDP_HEADER_LEN + payload_len;
    size_t total = DMUDP_V4_PSEUDO_HEADER_LEN + udp_len;
    uint8_t* buf = Dmod_Malloc(total);
    if (buf == NULL)
        return -ENOMEM;

    write_v4_pseudo_header(buf, &src_ip, dst_ip, (uint16_t)udp_len);

    uint8_t* segment = buf + DMUDP_V4_PSEUDO_HEADER_LEN;
    write_u16_be(&segment[0], src_port);
    write_u16_be(&segment[2], dst_port);
    write_u16_be(&segment[4], (uint16_t)udp_len);
    write_u16_be(&segment[6], 0);
    if (payload_len > 0)
        memcpy(&segment[DMUDP_HEADER_LEN], payload, payload_len);

    uint16_t checksum = dmip_checksum(buf, total);
    write_u16_be(&segment[6], (checksum == 0) ? 0xFFFFu : checksum); /* RFC 768: 0 means "no checksum" */

    dmip_header_t header = { 0 };
    header.family = dmip_family_v4;
    header.header.v4.ttl = DMIP_DEFAULT_TTL;
    header.header.v4.protocol = DMIP_PROTO_UDP;
    header.header.v4.identification = dmip_v4_next_identification();
    header.header.v4.src = src_ip;
    header.header.v4.dst = *dst_ip;

    result = dmip_send(&header, segment, udp_len, arp_timeout_ms);

    Dmod_Free(buf);
    return result;
}

/**
 * @brief Copy a UDP segment's payload out and read its ports, once the
 *        caller has already verified `segment` is at least
 *        DMUDP_HEADER_LEN bytes and passed checksum verification
 *
 * Shared tail of dmudp_handle_ip_packet()'s two family branches - the
 * only thing that differs between them is how `segment` was obtained
 * (IPv4 vs IPv6 packet, checksum-skip-on-zero vs always-verified).
 */
static int extract_udp_datagram(const uint8_t* segment, size_t segment_len, uint16_t* out_src_port, uint16_t* out_dst_port, uint8_t** out_payload, size_t* out_payload_len)
{
    size_t payload_len = segment_len - DMUDP_HEADER_LEN;
    uint8_t* payload = NULL;
    if (payload_len > 0)
    {
        payload = Dmod_Malloc(payload_len);
        if (payload == NULL)
            return -ENOMEM;
        memcpy(payload, segment + DMUDP_HEADER_LEN, payload_len);
    }

    *out_src_port = read_u16_be(&segment[0]);
    *out_dst_port = read_u16_be(&segment[2]);
    *out_payload = payload;
    *out_payload_len = payload_len;
    return 0;
}

/**
 * @brief Push a received datagram onto g_rx_queue and wake any waiter
 *
 * Takes ownership of `payload` - frees it itself if the queue entry
 * couldn't be allocated/pushed, so the caller never has to.
 */
static void push_rx_entry(dmip_family_t family, dmnetif_iface_t iface, const dmip_addr_t* src_ip, uint16_t src_port, uint16_t dst_port, uint8_t* payload, size_t payload_len)
{
    struct dmudp_rx_entry* entry = Dmod_Malloc(sizeof(*entry));
    if (entry == NULL)
    {
        Dmod_Free(payload);
        return;
    }

    entry->family = family;
    entry->iface = iface;
    entry->src_ip = *src_ip;
    entry->src_port = src_port;
    entry->dst_port = dst_port;
    entry->payload = payload;
    entry->payload_len = payload_len;

    dmosi_mutex_lock(g_rx_mutex);
    bool pushed = dmlist_push_back(g_rx_queue, entry);
    dmosi_mutex_unlock(g_rx_mutex);

    if (pushed)
    {
        dmosi_semaphore_post(g_rx_signal, 1);
    }
    else
    {
        Dmod_Free(entry->payload);
        Dmod_Free(entry);
    }
}

/**
 * @brief dmip_protocol_handler_t registered for DMIP_PROTO_UDP - see dmod_init()
 *
 * Parses `packet`'s IP header to locate the UDP segment, verifies its
 * checksum (dmudp_v4_checksum_valid(), skipped if the wire value is 0 per
 * RFC 768; or dmudp_v6_checksum_valid(), always required per RFC 8200),
 * and - on success - copies the payload out (extract_udp_datagram()) and
 * pushes it onto g_rx_queue. `packet` is borrowed (see
 * dmip_protocol_handler_t's own doc comment in dmip.h) - everything this
 * function wants to keep past the call is copied out before returning.
 * Anything that fails to parse or verify is silently dropped - there is
 * no caller here to report an error to, same as dmip's own
 * packet_received DIF implementation.
 */
static void dmudp_handle_ip_packet(dmip_family_t family, dmnetif_iface_t iface, const uint8_t* packet, size_t packet_len)
{
    dmip_addr_t src_ip = { 0 };
    const uint8_t* segment = NULL;
    size_t segment_len = 0;

    if (family == dmip_family_v4)
    {
        dmip_v4_header_t ip_header = { 0 };
        size_t header_len = 0;
        if (dmip_v4_parse_header(packet, packet_len, &ip_header, &header_len) != 0)
            return;

        segment = packet + header_len;
        segment_len = packet_len - header_len;
        if (segment_len < DMUDP_HEADER_LEN)
            return;

        uint16_t wire_checksum = read_u16_be(&segment[6]);
        if (wire_checksum != 0 && !dmudp_v4_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        src_ip = ip_header.src;
    }
    else /* dmip_family_v6 */
    {
        dmip_v6_header_t ip_header = { 0 };
        if (dmip_v6_parse_header(packet, packet_len, &ip_header) != 0)
            return;

        segment = packet + DMIP_V6_HEADER_LEN;
        segment_len = packet_len - DMIP_V6_HEADER_LEN;
        if (segment_len < DMUDP_HEADER_LEN)
            return;

        if (!dmudp_v6_checksum_valid(&ip_header.src, &ip_header.dst, segment, segment_len))
            return;

        src_ip = ip_header.src;
    }

    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint8_t* payload = NULL;
    size_t payload_len = 0;
    if (extract_udp_datagram(segment, segment_len, &src_port, &dst_port, &payload, &payload_len) != 0)
        return;

    push_rx_entry(family, iface, &src_ip, src_port, dst_port, payload, payload_len);
}

/**
 * @brief Implementation of dmudp_receive() - see dmudp.h
 *
 * No separate dmudp_v4_receive()/_v6_receive() (same reasoning as
 * dmudp_send() not having a per-family split): waits on g_rx_signal up to
 * `timeout_ms`, popping the first entry dmudp_handle_ip_packet() queued -
 * whichever family it happens to be.
 */
dmod_dmudp_api_declaration(1.0, int, _receive, ( uint32_t timeout_ms, dmip_family_t* out_family, dmip_addr_t* out_src_ip, uint16_t* out_src_port, uint16_t* out_dst_port, uint8_t** out_payload, size_t* out_payload_len, dmnetif_iface_t* out_iface ))
{
    if (out_family == NULL || out_src_ip == NULL || out_src_port == NULL || out_dst_port == NULL || out_payload == NULL || out_payload_len == NULL)
        return -EINVAL;

    uint32_t deadline = dmosi_get_tick_count() + timeout_ms;

    for (;;)
    {
        dmosi_mutex_lock(g_rx_mutex);
        struct dmudp_rx_entry* entry = (struct dmudp_rx_entry*)dmlist_pop_front(g_rx_queue);
        dmosi_mutex_unlock(g_rx_mutex);

        if (entry != NULL)
        {
            *out_family = entry->family;
            *out_src_ip = entry->src_ip;
            *out_src_port = entry->src_port;
            *out_dst_port = entry->dst_port;
            *out_payload = entry->payload;
            *out_payload_len = entry->payload_len;
            if (out_iface != NULL)
            {
                *out_iface = entry->iface;
            }
            Dmod_Free(entry);
            return 0;
        }

        int32_t remaining = (int32_t)(deadline - dmosi_get_tick_count());
        if (remaining <= 0)
            break;

        dmosi_semaphore_wait(g_rx_signal, 1, remaining);
    }

    return -EAGAIN;
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the receive queue and its
 *        guarding mutex/semaphore, then registers as the DMIP_PROTO_UDP
 *        handler
 */
int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    g_rx_queue = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_rx_mutex = dmosi_mutex_create(false);
    g_rx_signal = dmosi_semaphore_create(0, UINT32_MAX);
    if (g_rx_queue == NULL || g_rx_mutex == NULL || g_rx_signal == NULL)
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
 *        queued-but-unclaimed datagram, then the queue and its mutex/
 *        semaphore
 */
int dmod_deinit(void)
{
    dmip_unregister_protocol(DMIP_PROTO_UDP);

    size_t count = dmlist_size(g_rx_queue);
    for (size_t i = 0; i < count; i++)
    {
        struct dmudp_rx_entry* entry = (struct dmudp_rx_entry*)dmlist_pop_front(g_rx_queue);
        Dmod_Free(entry->payload);
        Dmod_Free(entry);
    }
    dmlist_destroy(g_rx_queue);
    g_rx_queue = NULL;

    dmosi_mutex_destroy(g_rx_mutex);
    g_rx_mutex = NULL;
    dmosi_semaphore_destroy(g_rx_signal);
    g_rx_signal = NULL;

    DMOD_LOG_INFO("DMUDP deinitialized\n");
    return 0;
}

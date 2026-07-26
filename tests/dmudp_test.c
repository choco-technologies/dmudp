/**
 * @file dmudp_test.c
 * @brief Test steps for dmudp
 *
 * Checksum steps replicate dmudp's own segment layout by hand (using
 * dmip_checksum() directly, same primitive dmudp.c itself calls, over a
 * hand-built pseudo-header + segment) to compute an expected checksum,
 * then check dmudp_v4_checksum_valid()/_v6_checksum_valid() agree - same
 * self-verification approach dmicmp_test.c uses for its own checksums.
 *
 * Dispatch steps register a "/dev/null"-backed dmnetif fixture interface
 * (same pattern as dmicmp_test.c/dmip_test.c) via dmod_test_setup()/
 * _teardown(), then drive a hand-built, correctly-checksummed IPv4/IPv6
 * packet straight into dmnetbridge's packet_received DIF implementation
 * via Dmod_GetNextDifModule()/Dmod_GetDifFunction() - the same discovery
 * dmnetbridge_handle_netif_rx() itself uses (see feed_frame()) - which
 * ends up in dmip's real DMIP_PROTO_UDP dispatch and from there in
 * dmudp's real dmudp_handle_ip_packet(). This lets a bound handler's
 * delivery be verified end-to-end without any real network I/O.
 *
 * Send-path steps can only be verified up through the point dmip_send()
 * itself can be tested without a real driver (route lookup, a hand-seeded
 * ARP cache hit), failing only at the final dmnetif_send() - same
 * documented limit dmicmp_test.c's own send tests describe for themselves.
 * The automatic ICMP Port Unreachable reply an unbound port triggers is
 * therefore only smoke-tested here (it returns void from dmudp's own
 * receive path, so its exact wire bytes can't be asserted from outside) -
 * same reasoning dmicmp_test.c gives for its own auto-reply smoke tests.
 *
 * Every send-path step uses a distinct destination network, since
 * dmroute's routes and dmarp's cache are both global state that outlives
 * a single step (same note dmicmp_test.c makes for itself).
 */
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmudp.h"
#include "dmroute.h"
#include "dmarp.h"
#include "dmnetbridge.h"
#include <string.h>
#include <errno.h>

static dmip_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

static dmip_addr_t make_v6(uint8_t last_byte)
{
    dmip_addr_t ip = { 0 };
    ip.family = dmip_family_v6;
    ip.addr.v6[0] = 0x20;
    ip.addr.v6[1] = 0x01;
    ip.addr.v6[DMIP_IPV6_ADDR_LEN - 1] = last_byte;
    return ip;
}

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

#define TEST_ETH_HEADER_LEN 14u
#define TEST_ETHERTYPE_IPV4 0x0800u
#define TEST_ETHERTYPE_IPV6 0x86DDu
#define TEST_MAX_PAYLOAD_LEN 64u
#define TEST_V4_PSEUDO_HEADER_LEN 12u
#define TEST_V6_PSEUDO_HEADER_LEN 40u

/**
 * @brief Wrap a complete IP packet in a minimal Ethernet frame and
 *        broadcast it to every packet_received DIF implementor (dmip's
 *        own, in practice) - the same discovery
 *        dmnetbridge_handle_netif_rx() itself uses
 */
static void feed_frame(dmnetif_iface_t iface, uint16_t ethertype, const uint8_t* packet, size_t packet_len)
{
    size_t frame_len = TEST_ETH_HEADER_LEN + packet_len;
    uint8_t* frame = Dmod_Malloc(frame_len);
    memset(frame, 0, TEST_ETH_HEADER_LEN);
    write_u16_be(&frame[12], ethertype);
    memcpy(frame + TEST_ETH_HEADER_LEN, packet, packet_len);

    Dmod_Context_t* implementor = NULL;
    while ((implementor = Dmod_GetNextDifModule(dmod_dmnetbridge_packet_received_sig, implementor)) != NULL)
    {
        dmod_dmnetbridge_packet_received_t fn =
            (dmod_dmnetbridge_packet_received_t)Dmod_GetDifFunction(implementor, dmod_dmnetbridge_packet_received_sig);
        if (fn != NULL)
        {
            fn(iface, frame, frame_len);
        }
    }

    Dmod_Free(frame);
}

/**
 * @brief Build a complete, correctly-checksummed UDP-over-IPv4 segment
 *        into `out` and return its length
 */
static size_t build_v4_segment(uint8_t* out, dmip_addr_t src, dmip_addr_t dst, uint16_t src_port, uint16_t dst_port, const uint8_t* payload, size_t payload_len)
{
    size_t udp_len = DMUDP_HEADER_LEN + payload_len;
    dmudp_header_t header = { .src_port = src_port, .dst_port = dst_port, .length = (uint16_t)udp_len };
    dmudp_build_header(out, udp_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMUDP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    memcpy(&pseudo_and_segment[0], src.addr.v4, DMIP_IPV4_ADDR_LEN);
    memcpy(&pseudo_and_segment[4], dst.addr.v4, DMIP_IPV4_ADDR_LEN);
    pseudo_and_segment[8] = 0;
    pseudo_and_segment[9] = DMIP_PROTO_UDP;
    write_u16_be(&pseudo_and_segment[10], (uint16_t)udp_len);
    memcpy(&pseudo_and_segment[TEST_V4_PSEUDO_HEADER_LEN], out, udp_len);

    write_u16_be(&out[6], dmip_checksum(pseudo_and_segment, TEST_V4_PSEUDO_HEADER_LEN + udp_len));
    return udp_len;
}

/**
 * @brief Build a complete, correctly-checksummed UDP-over-IPv6 segment
 *        into `out` and return its length
 */
static size_t build_v6_segment(uint8_t* out, dmip_addr_t src, dmip_addr_t dst, uint16_t src_port, uint16_t dst_port, const uint8_t* payload, size_t payload_len)
{
    size_t udp_len = DMUDP_HEADER_LEN + payload_len;
    dmudp_header_t header = { .src_port = src_port, .dst_port = dst_port, .length = (uint16_t)udp_len };
    dmudp_build_header(out, udp_len, &header);
    if (payload_len > 0)
    {
        memcpy(out + DMUDP_HEADER_LEN, payload, payload_len);
    }

    uint8_t pseudo_and_segment[TEST_V6_PSEUDO_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
    memcpy(&pseudo_and_segment[0], src.addr.v6, DMIP_IPV6_ADDR_LEN);
    memcpy(&pseudo_and_segment[16], dst.addr.v6, DMIP_IPV6_ADDR_LEN);
    pseudo_and_segment[32] = 0; pseudo_and_segment[33] = 0; pseudo_and_segment[34] = 0;
    pseudo_and_segment[35] = (uint8_t)udp_len;
    pseudo_and_segment[36] = 0; pseudo_and_segment[37] = 0; pseudo_and_segment[38] = 0;
    pseudo_and_segment[39] = DMIP_PROTO_UDP;
    memcpy(&pseudo_and_segment[TEST_V6_PSEUDO_HEADER_LEN], out, udp_len);

    write_u16_be(&out[6], dmip_checksum(pseudo_and_segment, TEST_V6_PSEUDO_HEADER_LEN + udp_len));
    return udp_len;
}

/**
 * @brief Build a complete IPv4 or IPv6 packet wrapping `segment` and feed
 *        it in via feed_frame()
 */
static void feed_udp_packet(dmnetif_iface_t iface, dmip_family_t family, dmip_addr_t src, dmip_addr_t dst, const uint8_t* segment, size_t segment_len)
{
    if (family == dmip_family_v4)
    {
        dmip_v4_header_t header = { 0 };
        header.total_length = (uint16_t)(DMIP_V4_HEADER_LEN + segment_len);
        header.ttl = DMIP_DEFAULT_TTL;
        header.protocol = DMIP_PROTO_UDP;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V4_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
        dmip_v4_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V4_HEADER_LEN, segment, segment_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV4, packet, DMIP_V4_HEADER_LEN + segment_len);
    }
    else
    {
        dmip_v6_header_t header = { 0 };
        header.payload_length = (uint16_t)segment_len;
        header.next_header = DMIP_PROTO_UDP;
        header.hop_limit = DMIP_DEFAULT_HOP_LIMIT;
        header.src = src;
        header.dst = dst;

        uint8_t packet[DMIP_V6_HEADER_LEN + DMUDP_HEADER_LEN + TEST_MAX_PAYLOAD_LEN];
        dmip_v6_build_header(packet, sizeof(packet), &header);
        memcpy(packet + DMIP_V6_HEADER_LEN, segment, segment_len);

        feed_frame(iface, TEST_ETHERTYPE_IPV6, packet, DMIP_V6_HEADER_LEN + segment_len);
    }
}

#define TEST_DEVICE_PATH "/dev/null"

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

/* ---- Header build/parse ---- */

DMOD_TEST_STEP(build_header_rejects_bad_arguments)
{
    uint8_t buffer[DMUDP_HEADER_LEN];
    dmudp_header_t header = { 0 };

    DMOD_TEST_EXPECT_EQ(dmudp_build_header(NULL, sizeof(buffer), &header), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_build_header(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_build_header(buffer, DMUDP_HEADER_LEN - 1, &header), -EINVAL);
}

DMOD_TEST_STEP(parse_header_rejects_bad_arguments)
{
    uint8_t buffer[DMUDP_HEADER_LEN] = { 0 };
    dmudp_header_t header = { 0 };

    DMOD_TEST_EXPECT_EQ(dmudp_parse_header(NULL, sizeof(buffer), &header), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_parse_header(buffer, sizeof(buffer), NULL), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_parse_header(buffer, DMUDP_HEADER_LEN - 1, &header), -EINVAL);
}

DMOD_TEST_STEP(build_and_parse_header_round_trip)
{
    dmudp_header_t header = { .src_port = 5353, .dst_port = 53, .length = DMUDP_HEADER_LEN + 4 };
    uint8_t buffer[DMUDP_HEADER_LEN];

    DMOD_TEST_EXPECT_EQ(dmudp_build_header(buffer, sizeof(buffer), &header), 0);

    dmudp_header_t parsed = { 0 };
    DMOD_TEST_EXPECT_EQ(dmudp_parse_header(buffer, sizeof(buffer), &parsed), 0);
    DMOD_TEST_EXPECT_EQ(parsed.src_port, (uint16_t)5353);
    DMOD_TEST_EXPECT_EQ(parsed.dst_port, (uint16_t)53);
    DMOD_TEST_EXPECT_EQ(parsed.length, (uint16_t)(DMUDP_HEADER_LEN + 4));
}

/* ---- Checksum ---- */

DMOD_TEST_STEP(v4_checksum_valid_round_trip)
{
    dmip_addr_t src = make_v4(10, 1, 0, 1);
    dmip_addr_t dst = make_v4(10, 1, 0, 2);
    uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 1234, 53, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmudp_v4_checksum_valid(&src, &dst, segment, segment_len));

    segment[DMUDP_HEADER_LEN] ^= 0xFF; /* corrupt the first payload byte */
    DMOD_TEST_EXPECT_FALSE(dmudp_v4_checksum_valid(&src, &dst, segment, segment_len));
}

DMOD_TEST_STEP(v6_checksum_valid_round_trip)
{
    dmip_addr_t src = make_v6(1);
    dmip_addr_t dst = make_v6(2);
    uint8_t payload[4] = { 'p', 'o', 'n', 'g' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v6_segment(segment, src, dst, 4321, 53, payload, sizeof(payload));

    DMOD_TEST_EXPECT_TRUE(dmudp_v6_checksum_valid(&src, &dst, segment, segment_len));

    segment[DMUDP_HEADER_LEN] ^= 0xFF;
    DMOD_TEST_EXPECT_FALSE(dmudp_v6_checksum_valid(&src, &dst, segment, segment_len));
}

DMOD_TEST_STEP(checksum_valid_rejects_bad_arguments)
{
    dmip_addr_t v4_addr = make_v4(1, 1, 1, 1);
    dmip_addr_t v6_addr = make_v6(1);
    uint8_t segment[DMUDP_HEADER_LEN] = { 0 };

    DMOD_TEST_EXPECT_FALSE(dmudp_v4_checksum_valid(NULL, &v4_addr, segment, sizeof(segment)));
    DMOD_TEST_EXPECT_FALSE(dmudp_v4_checksum_valid(&v4_addr, &v4_addr, segment, DMUDP_HEADER_LEN - 1));
    DMOD_TEST_EXPECT_FALSE(dmudp_v4_checksum_valid(&v6_addr, &v4_addr, segment, sizeof(segment)));

    DMOD_TEST_EXPECT_FALSE(dmudp_v6_checksum_valid(NULL, &v6_addr, segment, sizeof(segment)));
    DMOD_TEST_EXPECT_FALSE(dmudp_v6_checksum_valid(&v4_addr, &v6_addr, segment, sizeof(segment)));
    DMOD_TEST_EXPECT_FALSE(dmudp_v6_checksum_valid(&v6_addr, &v6_addr, segment, DMUDP_HEADER_LEN - 1));
}

/* ---- Bind / bind_any / unbind ---- */

static void unused_datagram_handler(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len)
{
    (void)src; (void)src_port; (void)dst_port; (void)iface; (void)payload; (void)payload_len;
}

#define TEST_FIXED_PORT_BASE 40000u

DMOD_TEST_STEP(bind_rejects_null_handler)
{
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE, NULL), -EINVAL);
}

DMOD_TEST_STEP(bind_rejects_port_zero)
{
    DMOD_TEST_EXPECT_EQ(dmudp_bind(0, unused_datagram_handler), -EINVAL);
}

DMOD_TEST_STEP(bind_twice_same_port_returns_eexist)
{
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 1, unused_datagram_handler), 0);
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 1, unused_datagram_handler), -EEXIST);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 1);
}

DMOD_TEST_STEP(unbind_unbound_port_is_safe)
{
    dmudp_unbind(TEST_FIXED_PORT_BASE + 999); /* must not crash */
}

DMOD_TEST_STEP(bind_after_unbind_succeeds)
{
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 2, unused_datagram_handler), 0);
    dmudp_unbind(TEST_FIXED_PORT_BASE + 2);
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 2, unused_datagram_handler), 0);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 2);
}

DMOD_TEST_STEP(bind_any_rejects_null_arguments)
{
    uint16_t port = 0;
    DMOD_TEST_EXPECT_EQ(dmudp_bind_any(NULL, &port), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_bind_any(unused_datagram_handler, NULL), -EINVAL);
}

DMOD_TEST_STEP(bind_any_returns_port_in_ephemeral_range)
{
    uint16_t port = 0;
    DMOD_TEST_EXPECT_EQ(dmudp_bind_any(unused_datagram_handler, &port), 0);
    DMOD_TEST_EXPECT_TRUE(port >= DMUDP_PORT_EPHEMERAL_FIRST && port <= DMUDP_PORT_EPHEMERAL_LAST);

    /* The port must really be reserved - binding it again explicitly must fail. */
    DMOD_TEST_EXPECT_EQ(dmudp_bind(port, unused_datagram_handler), -EEXIST);

    dmudp_unbind(port);
}

DMOD_TEST_STEP(bind_any_skips_already_bound_port)
{
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    DMOD_TEST_EXPECT_EQ(dmudp_bind_any(unused_datagram_handler, &port_a), 0);
    DMOD_TEST_EXPECT_EQ(dmudp_bind_any(unused_datagram_handler, &port_b), 0);

    DMOD_TEST_EXPECT_TRUE(port_a != port_b);

    dmudp_unbind(port_a);
    dmudp_unbind(port_b);
}

/* ---- Dispatch to a bound handler (end-to-end) ---- */

static bool     g_handler_called;
static dmip_addr_t g_handler_src;
static uint16_t g_handler_src_port;
static uint16_t g_handler_dst_port;
static uint8_t  g_handler_payload[TEST_MAX_PAYLOAD_LEN];
static size_t   g_handler_payload_len;

static void recording_datagram_handler(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len)
{
    (void)iface;
    g_handler_called = true;
    g_handler_src = *src;
    g_handler_src_port = src_port;
    g_handler_dst_port = dst_port;
    g_handler_payload_len = payload_len;
    if (payload_len > 0)
    {
        memcpy(g_handler_payload, payload, payload_len);
    }
}

static void reset_handler_recording(void)
{
    g_handler_called = false;
    memset(&g_handler_src, 0, sizeof(g_handler_src));
    g_handler_src_port = 0;
    g_handler_dst_port = 0;
    g_handler_payload_len = 0;
}

DMOD_TEST_STEP(dispatch_to_bound_handler_v4)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 10, recording_datagram_handler), 0);

    dmip_addr_t src = make_v4(10, 2, 0, 1);
    dmip_addr_t dst = make_v4(10, 2, 0, 2);
    uint8_t payload[4] = { 'd', 'a', 't', 'a' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 6000, TEST_FIXED_PORT_BASE + 10, payload, sizeof(payload));

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_TRUE(g_handler_called);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_handler_src.addr.v4, src.addr.v4, DMIP_IPV4_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(g_handler_src_port, (uint16_t)6000);
    DMOD_TEST_EXPECT_EQ(g_handler_dst_port, (uint16_t)(TEST_FIXED_PORT_BASE + 10));
    DMOD_TEST_EXPECT_EQ(g_handler_payload_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_handler_payload, payload, sizeof(payload)));

    dmudp_unbind(TEST_FIXED_PORT_BASE + 10);
}

DMOD_TEST_STEP(dispatch_to_bound_handler_v6)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 11, recording_datagram_handler), 0);

    dmip_addr_t src = make_v6(10);
    dmip_addr_t dst = make_v6(11);
    uint8_t payload[4] = { 'd', 'a', 't', 'a' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v6_segment(segment, src, dst, 6001, TEST_FIXED_PORT_BASE + 11, payload, sizeof(payload));

    feed_udp_packet(g_iface, dmip_family_v6, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_TRUE(g_handler_called);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_handler_src.addr.v6, src.addr.v6, DMIP_IPV6_ADDR_LEN));
    DMOD_TEST_EXPECT_EQ(g_handler_src_port, (uint16_t)6001);
    DMOD_TEST_EXPECT_EQ(g_handler_dst_port, (uint16_t)(TEST_FIXED_PORT_BASE + 11));
    DMOD_TEST_EXPECT_EQ(g_handler_payload_len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(bytes_equal(g_handler_payload, payload, sizeof(payload)));

    dmudp_unbind(TEST_FIXED_PORT_BASE + 11);
}

DMOD_TEST_STEP(checksum_zero_v4_datagram_still_dispatched)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 12, recording_datagram_handler), 0);

    dmip_addr_t src = make_v4(10, 3, 0, 1);
    dmip_addr_t dst = make_v4(10, 3, 0, 2);
    uint8_t payload[4] = { 'z', 'e', 'r', 'o' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 6002, TEST_FIXED_PORT_BASE + 12, payload, sizeof(payload));

    /* Corrupt the payload (so the real checksum would no longer match) and
     * then force the wire checksum to 0 - RFC 768 says that means "no
     * checksum", so verification must be skipped and the handler still
     * called despite the data no longer agreeing with the original checksum. */
    segment[DMUDP_HEADER_LEN] ^= 0xFF;
    write_u16_be(&segment[6], 0);

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_TRUE(g_handler_called);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 12);
}

DMOD_TEST_STEP(corrupted_checksum_v4_datagram_dropped_silently)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 13, recording_datagram_handler), 0);

    dmip_addr_t src = make_v4(10, 4, 0, 1);
    dmip_addr_t dst = make_v4(10, 4, 0, 2);
    uint8_t payload[4] = { 'b', 'a', 'd', '!' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 6003, TEST_FIXED_PORT_BASE + 13, payload, sizeof(payload));
    segment[DMUDP_HEADER_LEN] ^= 0xFF; /* corrupt payload, checksum field left nonzero and now wrong */

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_FALSE(g_handler_called);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 13);
}

DMOD_TEST_STEP(corrupted_checksum_v6_datagram_dropped_silently)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 14, recording_datagram_handler), 0);

    dmip_addr_t src = make_v6(20);
    dmip_addr_t dst = make_v6(21);
    uint8_t payload[4] = { 'b', 'a', 'd', '!' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v6_segment(segment, src, dst, 6004, TEST_FIXED_PORT_BASE + 14, payload, sizeof(payload));
    segment[DMUDP_HEADER_LEN] ^= 0xFF;

    feed_udp_packet(g_iface, dmip_family_v6, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_FALSE(g_handler_called);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 14);
}

DMOD_TEST_STEP(malformed_short_segment_dropped)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 15, recording_datagram_handler), 0);

    dmip_addr_t src = make_v4(10, 5, 0, 1);
    dmip_addr_t dst = make_v4(10, 5, 0, 2);
    uint8_t short_segment[DMUDP_HEADER_LEN - 1] = { 0 };

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, short_segment, sizeof(short_segment)); /* must not crash */

    DMOD_TEST_EXPECT_FALSE(g_handler_called);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 15);
}

DMOD_TEST_STEP(length_field_mismatch_dropped)
{
    reset_handler_recording();
    DMOD_TEST_EXPECT_EQ(dmudp_bind(TEST_FIXED_PORT_BASE + 16, recording_datagram_handler), 0);

    dmip_addr_t src = make_v4(10, 6, 0, 1);
    dmip_addr_t dst = make_v4(10, 6, 0, 2);
    uint8_t payload[4] = { 'l', 'e', 'n', '!' };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 6005, TEST_FIXED_PORT_BASE + 16, payload, sizeof(payload));
    write_u16_be(&segment[4], (uint16_t)(segment_len + 50)); /* declared length no longer matches the real segment */

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, segment, segment_len);

    DMOD_TEST_EXPECT_FALSE(g_handler_called);

    dmudp_unbind(TEST_FIXED_PORT_BASE + 16);
}

/* ---- Port unreachable / IPv6 gap (smoke tests - see this file's top
 * comment for why the exact ICMP reply bytes can't be asserted here) ---- */

DMOD_TEST_STEP(unbound_v4_port_triggers_dest_unreachable_without_crashing)
{
    dmip_addr_t src = make_v4(10, 7, 0, 1);
    dmip_addr_t dst = make_v4(10, 7, 0, 2);
    uint8_t payload[4] = { 0 };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v4_segment(segment, src, dst, 6006, TEST_FIXED_PORT_BASE + 20, payload, sizeof(payload));

    feed_udp_packet(g_iface, dmip_family_v4, src, dst, segment, segment_len); /* no handler bound to +20 */
}

DMOD_TEST_STEP(unbound_v6_port_logs_and_drops_without_crashing)
{
    dmip_addr_t src = make_v6(30);
    dmip_addr_t dst = make_v6(31);
    uint8_t payload[4] = { 0 };
    uint8_t segment[DMUDP_HEADER_LEN + sizeof(payload)];
    size_t segment_len = build_v6_segment(segment, src, dst, 6007, TEST_FIXED_PORT_BASE + 21, payload, sizeof(payload));

    feed_udp_packet(g_iface, dmip_family_v6, src, dst, segment, segment_len); /* no handler bound to +21 */
}

/* ---- Sending ---- */

DMOD_TEST_STEP(send_rejects_bad_arguments)
{
    dmip_addr_t dst = make_v4(10, 8, 0, 1);
    uint8_t payload[4] = { 0 };

    DMOD_TEST_EXPECT_EQ(dmudp_send(NULL, 1, 53, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_send(&dst, 1, 0, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmudp_send(&dst, 1, 53, NULL, DMUDP_MAX_PAYLOAD_LEN + 1, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

DMOD_TEST_STEP(send_v6_returns_enosys_immediately)
{
    dmip_addr_t dst = make_v6(1);
    uint8_t payload[4] = { 0 };

    DMOD_TEST_EXPECT_EQ(dmudp_send(&dst, 1, 53, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS), -ENOSYS);
}

DMOD_TEST_STEP(send_v4_no_route_returns_enetunreach)
{
    dmip_addr_t dst = make_v4(203, 0, 113, 5); /* TEST-NET-3 - no route added anywhere in this file */
    uint8_t payload[4] = { 0 };

    DMOD_TEST_EXPECT_EQ(dmudp_send(&dst, 1, 53, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS), -ENETUNREACH);
}

DMOD_TEST_STEP(send_v4_full_path_without_real_driver_returns_eio)
{
    dmip_addr_t dest_net = make_v4(172, 16, 13, 0);
    dmip_addr_t netmask = make_v4(255, 255, 255, 0);
    dmroute_route_t route = dmroute_add(&dest_net, &netmask, NULL, "test0", DMROUTE_DEFAULT_METRIC, dmroute_origin_static);

    dmip_addr_t dst = make_v4(172, 16, 13, 5);
    dmnetif_mac_addr_t fake_mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, 0x33 } };
    dmarp_cache_insert(g_iface, &dst, &fake_mac);

    uint8_t payload[3] = { 'h', 'i', '!' };
    DMOD_TEST_EXPECT_EQ(dmudp_send(&dst, 12345, 53, payload, sizeof(payload), DMARP_DEFAULT_TIMEOUT_MS), -EIO);

    dmarp_cache_remove(g_iface, &dst);
    dmroute_remove(route);
}

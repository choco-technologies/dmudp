#ifndef DMUDP_H
#define DMUDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmip.h"
#include "dmudp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmudp.h
 * @brief DMOD UDP - Public API
 *
 * dmudp builds/parses UDP segments (RFC 768) and plugs into dmip's
 * protocol dispatch the same way dmicmp does (see dmicmp.h): it registers
 * dmudp_handle_ip_packet() via dmip_register_protocol(DMIP_PROTO_UDP, ...)
 * in dmod_init() (see src/dmudp.c). Unlike dmicmp, there is no
 * dmip_register_default_protocol() registration here - claiming any IP
 * protocol nobody else wants is exclusively dmicmp's job.
 *
 * Sending calls straight into dmip's family-agnostic-by-destination-address
 * dmip_v4_send() (dmudp_send() itself has one signature for both families -
 * `dst`'s own family selects the path, no dmudp_v4_send()/_v6_send() split;
 * dmudp_send() returns -ENOSYS for an IPv6 destination, the same gap
 * dmip_send()/dmicmp already live with - see dmip.h - blocked on a missing
 * NDP module, not a dmudp-specific limitation).
 *
 * New relative to a plain "build/parse/checksum/send" layer: a per-port
 * registry. dmudp_bind()/_bind_any() reserve a UDP port and register a
 * dmudp_datagram_handler_t for it; dmudp_unbind() releases one. A matching
 * inbound datagram's handler is called synchronously, inline, from whatever
 * thread is pumping the interface it arrived on (see dmip_protocol_handler_t
 * in dmip.h) - the same delivery context dmicmp's own inline Echo Reply
 * relies on, specifically so a handler can reply (e.g. call dmudp_send()
 * right back) without needing its own thread or queue.
 *
 * A datagram addressed to a port with no registered handler is reported
 * back to its sender as an ICMP Port Unreachable for IPv4
 * (dmicmp_v4_send_dest_unreachable(dmicmp_v4_dest_unreachable_port, ...) -
 * exactly the case dmicmp.h documents dmudp as the motivating use for) or,
 * for IPv6, logged and dropped (DMOD_LOG_WARN) - there is no ICMPv6 send
 * capability yet (same gap dmicmp itself documents for its own unclaimed-
 * protocol IPv6 case). A malformed segment or one that fails checksum
 * verification is always dropped silently regardless of family - no ICMP
 * reply is sent for possibly-spoofed or corrupt traffic, only for a
 * genuinely well-formed datagram nobody is listening for.
 *
 * dmudp depends on dmip (dmip_send()/_v4_send()/_v4_get_source_address()
 * for transmit, dmip_checksum() for the pseudo-header checksum, protocol
 * registration) and dmicmp (dmicmp_v4_send_dest_unreachable(), for the
 * port-unreachable reply) and, transitively through them, dmroute
 * (dmip_addr_t's real definition) and dmnetif (dmnetif_iface_t) - dmudp
 * itself never calls a dmroute_* /dmnetif_* function directly. Also depends
 * on dmlist (the port-binding registry) and dmosi (the mutex guarding it).
 * Messages are built/parsed as raw byte buffers rather than packed C
 * structs, same reasoning dmip.c/dmicmp.c document for themselves (dmod's
 * minimal module runtime gives no struct-packing guarantee).
 */

/* ============================================================================
 *                      Common header (RFC 768)
 * ========================================================================== */

/**
 * @brief Length in bytes of the UDP header: source port, destination port,
 *        length, checksum - identical layout for IPv4 and IPv6
 */
#define DMUDP_HEADER_LEN 8u

/**
 * @brief Largest payload dmudp_send() can carry - the UDP length field is
 *        16 bits and counts header + payload
 */
#define DMUDP_MAX_PAYLOAD_LEN (65535u - DMUDP_HEADER_LEN)

/**
 * @brief Parsed UDP header fields (RFC 768)
 *
 * dmudp_build_header() ignores `checksum` (the caller computes and writes
 * it separately, over a pseudo-header + header + payload - see
 * dmudp_v4_checksum_valid()/_v6_checksum_valid() - the same two-step shape
 * dmicmp_build_header() uses and for the same reason: the checksum covers a
 * payload that doesn't exist yet at build_header() time). `length` is the
 * caller-declared header+payload byte count (DMUDP_HEADER_LEN +
 * payload_len); dmudp_build_header() writes it as given, dmudp_parse_header()
 * fills it from the wire unverified against the segment's actual received
 * length - that consistency check belongs to the receive dispatch logic,
 * not here.
 */
typedef struct
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;    /**< Parse output only - see above */
} dmudp_header_t;

/**
 * @brief Build an 8-byte UDP header (no payload) into `buffer`
 *
 * @param buffer     Output buffer, must be at least DMUDP_HEADER_LEN bytes
 * @param buffer_len Size of `buffer` in bytes
 * @param header     Fields to encode
 *
 * @return 0 on success, -EINVAL on a NULL argument or too-small buffer
 */
dmod_dmudp_api(1.0, int, _build_header, ( uint8_t* buffer, size_t buffer_len, const dmudp_header_t* header ));

/**
 * @brief Parse an 8-byte UDP header from `buffer`
 *
 * @param buffer Received bytes, header first
 * @param length Number of valid bytes in `buffer`, must be at least
 *                DMUDP_HEADER_LEN
 * @param header Output: parsed fields
 *
 * @return 0 on success, -EINVAL on a NULL argument or `length` shorter than
 *         DMUDP_HEADER_LEN
 */
dmod_dmudp_api(1.0, int, _parse_header, ( const uint8_t* buffer, size_t length, dmudp_header_t* header ));

/* ============================================================================
 *                      Checksum
 * ========================================================================== */

/**
 * @brief Check whether a received UDP-over-IPv4 segment's checksum is valid
 *
 * Unlike ICMPv4, UDP's checksum DOES need a pseudo-header (RFC 768: source
 * and destination address, zero byte, protocol, UDP length) - `src_ip`/
 * `dst_ip` are required here, the same as the IPv6 sibling below (this is
 * the one place UDP's checksum shape is symmetric across families where
 * ICMP's is not - see dmicmp_v4_checksum_valid()'s own doc comment).
 *
 * This function has no awareness of RFC 768's "checksum 0 means none"
 * allowance - a wire checksum of 0 is simply computed and compared like any
 * other value (and will therefore normally report false, since a genuine
 * checksum is essentially never exactly 0). Deciding to skip verification
 * for a 0 wire checksum on an IPv4 datagram is the receive dispatch logic's
 * job, made before calling this function at all - not this function's.
 *
 * @param src_ip  Source address the segment arrived from (family must be
 *                 dmip_family_v4)
 * @param dst_ip  Destination address it arrived on (family must be
 *                 dmip_family_v4)
 * @param segment Received bytes, UDP header first
 * @param length  Number of valid bytes in `segment`, must be at least
 *                 DMUDP_HEADER_LEN
 *
 * @return true if the checksum matches the segment's contents
 */
dmod_dmudp_api(1.0, bool, _v4_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ));

/**
 * @brief Check whether a received UDP-over-IPv6 segment's checksum is valid
 *
 * RFC 8200 8.1: mandatory, no "0 means none" allowance at all (unlike
 * IPv4) - the receive dispatch logic calls this unconditionally for every
 * IPv6 datagram, never skipping it based on the wire value.
 *
 * @param src_ip  Source address (family must be dmip_family_v6)
 * @param dst_ip  Destination address (family must be dmip_family_v6)
 * @param segment Received bytes, UDP header first
 * @param length  Number of valid bytes in `segment`, must be at least
 *                 DMUDP_HEADER_LEN
 *
 * @return true if the checksum matches the segment's contents
 */
dmod_dmudp_api(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t length ));

/* ============================================================================
 *                      Port binding
 * ========================================================================== */

/**
 * @brief First port dmudp_bind_any() will consider (RFC 6335 dynamic/
 *        private range)
 */
#define DMUDP_PORT_EPHEMERAL_FIRST 49152u

/**
 * @brief Last port dmudp_bind_any() will consider (RFC 6335 dynamic/
 *        private range)
 */
#define DMUDP_PORT_EPHEMERAL_LAST 65535u

/**
 * @brief Callback registered via dmudp_bind()/_bind_any() to receive every
 *        datagram addressed to the bound port
 *
 * Called inline, from whatever thread is pumping the interface the
 * datagram arrived on (see dmip_protocol_handler_t in dmip.h) - the same
 * delivery context dmicmp's own inline Echo Reply relies on, specifically
 * so a handler can reply synchronously (e.g. call dmudp_send() right back)
 * without needing its own thread or queue. `payload` is only valid for the
 * duration of the call - copy it out if you need it afterward.
 *
 * `dst_port` is redundant with whichever port this particular registration
 * was bound to - included anyway (the same reason
 * dmicmp_echo_reply_handler_t passes back its own `identifier`) since one
 * handler function can legitimately be bound to more than one port and
 * needs a way to tell them apart without a separate closure per port.
 *
 * @param src         Address the datagram came from
 * @param src_port    Source port the sender used
 * @param dst_port    The bound port this datagram was addressed to
 * @param iface       Interface the datagram arrived on
 * @param payload     UDP payload (segment with the 8-byte header stripped)
 * @param payload_len Length of `payload` in bytes
 */
typedef void (*dmudp_datagram_handler_t)( const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len );

/**
 * @brief Reserve `port` and register `handler` to receive every datagram
 *        addressed to it
 *
 * @param port    UDP port to bind, must be nonzero
 * @param handler Callback to invoke for every matching datagram
 *
 * @return 0 on success, -EINVAL if `handler` is NULL or `port` is 0,
 *         -EEXIST if `port` is already bound (call dmudp_unbind() first if
 *         you mean to replace it), -ENOMEM if the registration itself
 *         could not be allocated
 */
dmod_dmudp_api(1.0, int, _bind, ( uint16_t port, dmudp_datagram_handler_t handler ));

/**
 * @brief Reserve the first free port in the ephemeral range
 *        (DMUDP_PORT_EPHEMERAL_FIRST..DMUDP_PORT_EPHEMERAL_LAST) and
 *        register `handler` for it
 *
 * @param handler  Callback to invoke for every datagram addressed to the
 *                  chosen port
 * @param out_port Output: the port that was reserved
 *
 * @return 0 on success (`*out_port` set), -EINVAL if `handler` or
 *         `out_port` is NULL, -EADDRNOTAVAIL if every port in the
 *         ephemeral range is already bound, -ENOMEM if the registration
 *         itself could not be allocated
 */
dmod_dmudp_api(1.0, int, _bind_any, ( dmudp_datagram_handler_t handler, uint16_t* out_port ));

/**
 * @brief Undo dmudp_bind()/_bind_any() - safe to call for a port that was
 *        never bound (a no-op)
 *
 * @param port UDP port to release
 */
dmod_dmudp_api(1.0, void, _unbind, ( uint16_t port ));

/* ============================================================================
 *                      Sending
 * ========================================================================== */

/**
 * @brief Build, checksum and send a UDP datagram
 *
 * One function per direction, not one per family - `dst`'s own family
 * selects IPv4/IPv6, so there's no dmudp_v4_send()/_v6_send() split for a
 * caller to choose between.
 *
 * @param dst            Destination address
 * @param src_port       Source port to declare (0 is legal per RFC 768 -
 *                        a sender with no interest in a reply)
 * @param dst_port       Destination port, must be nonzero
 * @param payload        Data to send
 * @param payload_len    Length of `payload` in bytes, at most
 *                        DMUDP_MAX_PAYLOAD_LEN
 * @param arp_timeout_ms Forwarded to dmip_v4_send()
 *
 * @return 0 on success, -EINVAL (NULL/bad `dst`, `dst_port` 0, oversized
 *         payload), -ENOSYS for a dmip_family_v6 `dst` (no dmip_v6_send()
 *         yet - the same gap dmip/dmicmp already live with, blocked on a
 *         missing NDP module), otherwise whatever
 *         dmip_v4_get_source_address()/dmip_v4_send() return
 *         (-ENETUNREACH/-ENODEV/-EHOSTUNREACH/-EMSGSIZE/-ENOMEM/-EIO)
 */
dmod_dmudp_api(1.0, int, _send, ( const dmip_addr_t* dst, uint16_t src_port, uint16_t dst_port, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ));

#ifdef __cplusplus
}
#endif

#endif // DMUDP_H

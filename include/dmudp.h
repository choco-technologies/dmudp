#ifndef DMUDP_H
#define DMUDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmip.h"
#include "dmnetif.h"
#include "dmudp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmudp.h
 * @brief DMOD UDP - Public API
 *
 * dmudp builds/parses UDP segments (RFC 768) - source/destination port,
 * length, the pseudo-header checksum - and sends/receives them by calling
 * straight into dmip: dmudp itself never touches dmroute/dmarp/dmnetif
 * directly for I/O, dmip (and, below it, dmnetbridge) already does all of
 * that. Building on top of an IP layer that can actually put a packet on
 * the wire is the entire point of dmudp existing as its own module
 * rather than a few more functions bolted onto dmip.
 *
 * There is a single dmudp_send()/dmudp_receive() pair, not a per-family
 * split (dmudp_v4_send()/_v6_send(), dmudp_v4_receive()/_v6_receive()):
 * a caller only ever has one destination address, which already carries
 * the family that decides everything else, so making it *also* pick a
 * function name on top of that would be pure redundancy - see
 * dmudp_send()'s own doc comment. dmudp_send() can't actually reach IPv6
 * yet (dmip_send() itself has no v6 path - resolving a destination MAC
 * for IPv6 needs NDP, RFC 4861, and there is no NDP module in this tree
 * yet, the same boundary dmarp.h documents for itself) - it reports that
 * honestly (-ENOSYS) rather than pretending. Receiving has no such gap;
 * dmudp_receive() handles either family from the start.
 *
 * dmudp has no socket/bind concept - there is no per-port registry, and
 * dmudp_receive() hands back every datagram regardless of destination
 * port (a caller that wants to filter by `out_dst_port` itself can). It
 * does, however, own a small receive queue of its own: dmudp registers
 * itself with dmip as the handler for UDP's IP protocol number
 * (dmip_register_protocol(), see dmip.h's "Protocol registration"
 * section) rather than pulling packets from dmip directly, since dmip no
 * longer hands out packets to whoever asks - it dispatches by protocol.
 * See src/dmudp.c for the receive-side detail.
 *
 * dmudp depends on dmip (dmip_send(), dmip_checksum(), dmip_addr_t,
 * protocol registration) and, transitively through it, dmroute
 * (dmip_addr_t's real definition) and dmnetif (dmnetif_iface_t). Also
 * depends on dmlist (its own receive queue) and dmosi (the mutex/
 * semaphore guarding it). Every function here is plain Built-in API
 * (dmod_dmudp_api).
 */

/**
 * @brief Length in bytes of a UDP header (RFC 768): source port,
 *        destination port, length, checksum - 2 bytes each
 */
#define DMUDP_HEADER_LEN 8u

/**
 * @brief Verify a received IPv4 UDP segment's checksum
 *
 * Per RFC 768, a wire checksum of 0 means "no checksum was computed" -
 * callers should skip calling this in that case rather than treating it
 * as a failure; dmudp_receive() already does this.
 *
 * @param src_ip     Source IP address the segment arrived from (family
 *                    must be dmip_family_v4)
 * @param dst_ip     Destination IP address the segment arrived on (family
 *                    must be dmip_family_v4)
 * @param segment    UDP header + payload, as received (checksum field
 *                    included, not zeroed)
 * @param segment_len Length of `segment` in bytes, must be at least
 *                    DMUDP_HEADER_LEN
 *
 * @return true if the checksum matches the segment's contents
 */
dmod_dmudp_api(1.0, bool, _v4_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t segment_len ));

/**
 * @brief Verify a received IPv6 UDP segment's checksum
 *
 * Unlike IPv4, a UDP checksum over IPv6 is mandatory (RFC 8200 8.1) - a
 * wire value of 0 is simply invalid, not "no checksum" (dmudp_receive()
 * treats it as a checksum failure, not a special case, for an IPv6
 * datagram).
 *
 * @param src_ip     Source IP address the segment arrived from (family
 *                    must be dmip_family_v6)
 * @param dst_ip     Destination IP address the segment arrived on (family
 *                    must be dmip_family_v6)
 * @param segment    UDP header + payload, as received
 * @param segment_len Length of `segment` in bytes, must be at least
 *                    DMUDP_HEADER_LEN
 *
 * @return true if the checksum matches the segment's contents
 */
dmod_dmudp_api(1.0, bool, _v6_checksum_valid, ( const dmip_addr_t* src_ip, const dmip_addr_t* dst_ip, const uint8_t* segment, size_t segment_len ));

/**
 * @brief Build, checksum and send a UDP datagram to `dst_ip`
 *
 * For `dmip_family_v4`: calls dmip_v4_get_source_address() first to learn
 * the source address dmip_send() will use (needed up front - the
 * pseudo-header checksum has to be computed before the segment can be
 * handed off), builds the 8-byte UDP header and checksum, then calls
 * dmip_send() with the destination, DMIP_PROTO_UDP, and this segment as
 * the payload.
 *
 * There is no separate dmudp_v4_send()/_v6_send() - `dst_ip->family`
 * alone is what a per-family split would have keyed off anyway, and
 * `dst_ip` already carries it.
 *
 * @param dst_ip        Destination IP address (family must be
 *                        dmip_family_v4 or _v6)
 * @param dst_port      Destination port
 * @param src_port      Source port
 * @param payload       Data to send
 * @param payload_len   Length of `payload` in bytes
 * @param arp_timeout_ms Forwarded to dmip_send() (and from there, for
 *                        `dmip_family_v4`, to dmarp_resolve()) - use
 *                        DMARP_DEFAULT_TIMEOUT_MS if you don't have a
 *                        strong opinion
 *
 * @return 0 on success, -EINVAL (bad argument/family), -ENOSYS
 *         (`dst_ip->family` is `dmip_family_v6` - see this file's top
 *         comment for why), -ENETUNREACH/-ENODEV (no usable route to
 *         `dst_ip`), -EHOSTUNREACH (ARP resolution failed), -EMSGSIZE/
 *         -ENOMEM/-EIO - see dmip_send()/dmip_v4_send() for the exact
 *         meaning of each, all passed straight through
 */
dmod_dmudp_api(1.0, int, _send, ( const dmip_addr_t* dst_ip, uint16_t dst_port, uint16_t src_port, const void* payload, size_t payload_len, uint32_t arp_timeout_ms ));

/**
 * @brief Receive one UDP datagram of either family, if one is available
 *
 * Waits on dmudp's own receive queue, fed by dmudp_handle_ip_packet() -
 * registered with dmip as the DMIP_PROTO_UDP handler (see this file's
 * top comment and dmip.h's "Protocol registration" section) - which
 * verifies the checksum (dmudp_v4_checksum_valid()/_v6_checksum_valid()
 * depending on the packet's family) and copies out just the UDP payload
 * for every completed UDP packet dmip hands it, regardless of which
 * interface it arrived on. The caller here never sees the surrounding IP
 * packet.
 *
 * There is no separate dmudp_v4_receive()/_v6_receive(): unlike sending,
 * where the caller already knows the destination family up front,
 * *receiving* has no such split to key a function name off in the first
 * place - which family arrives is exactly what waiting is trying to find
 * out, so it can only ever be an output, never a choice of which
 * function to call.
 *
 * @param timeout_ms      Milliseconds to wait for a datagram if none is
 *                         already queued. 0 checks once and returns
 *                         immediately without waiting
 * @param out_family      Output: dmip_family_v4 or _v6, whichever the
 *                         datagram arrived as - only meaningful when this
 *                         returns 0
 * @param out_src_ip      Output: the datagram's source IP address
 * @param out_src_port    Output: the datagram's source port
 * @param out_dst_port    Output: the datagram's destination port
 * @param out_payload     Output: on 0, a heap-allocated copy of the UDP
 *                         payload. Owned by the caller - release with
 *                         Dmod_Free() when done
 * @param out_payload_len Output: length of `*out_payload` in bytes
 * @param out_iface       Output: the interface the datagram arrived on.
 *                         Optional - pass NULL if not needed
 *
 * @return 0 if a datagram was received, -EAGAIN if none arrived within
 *         `timeout_ms`, -EINVAL if a required output parameter is NULL.
 *         Unlike before this module registered with dmip
 *         (dmip_register_protocol()) instead of pulling packets itself,
 *         a malformed packet or a checksum mismatch is no longer
 *         something this call can report - dmudp_handle_ip_packet()
 *         (see src/dmudp.c) silently drops those the moment they're
 *         handed to it, before anything reaches this queue, the same way
 *         dmip's own packet_received DIF implementation silently drops
 *         what isn't a well-formed packet
 */
dmod_dmudp_api(1.0, int, _receive, ( uint32_t timeout_ms, dmip_family_t* out_family, dmip_addr_t* out_src_ip, uint16_t* out_src_port, uint16_t* out_dst_port, uint8_t** out_payload, size_t* out_payload_len, dmnetif_iface_t* out_iface ));

#ifdef __cplusplus
}
#endif

#endif // DMUDP_H

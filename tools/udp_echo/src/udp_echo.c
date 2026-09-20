/**
 * @file udp_echo.c
 * @brief udp_echo - bind a UDP port and echo every datagram back to its sender
 *
 * A thin CLI over dmudp's port-binding API (dmudp_bind()/_unbind(),
 * dmudp_send() - see include/dmudp.h). The datagram handler runs inline,
 * from whatever thread is pumping the receiving interface (dmudp.h's own
 * doc comment on dmudp_datagram_handler_t), which is exactly what lets it
 * reply with dmudp_send() directly instead of queueing the payload for a
 * separate thread.
 *
 * Only IPv4 senders get a reply (dmudp_send() itself has no IPv6 send path
 * yet - see dmudp.h) - an IPv6 datagram is logged and dropped.
 */
#include "dmod.h"
#include "dmudp.h"
#include "dmarp.h"
#include "dmosi.h"
#include <string.h>

#define UDP_ECHO_DEFAULT_DURATION_S 30u

static uint16_t g_bound_port;
static uint32_t g_echoed_count;

/* Parses a plain decimal uint32_t (dmod's minimal module runtime has no
 * strtol()/atoi() - see dmod/src/module/string.c's replacement set). */
static bool parse_uint32(const char* s, uint32_t* out)
{
    if (s == NULL || *s == '\0')
        return false;

    uint64_t value = 0;
    for (const char* p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return false;

        value = value * 10 + (uint64_t)(*p - '0');
        if (value > UINT32_MAX)
            return false;
    }

    *out = (uint32_t)value;
    return true;
}

/* Only ever invoked while dmudp_bind() is holding g_bound_port's
 * registration - main() blocks for the whole run without touching
 * g_echoed_count itself, so the single-writer-from-the-network-thread
 * invariant holds without extra locking (same reasoning ping.c's
 * echo_reply_handler() gives for its own statics). */
static void echo_handler(const dmip_addr_t* src, uint16_t src_port, uint16_t dst_port, dmnetif_iface_t iface, const uint8_t* payload, size_t payload_len)
{
    (void)dst_port;
    (void)iface;

    if (src->family != dmip_family_v4)
    {
        Dmod_Printf("udp_echo: dropping %u-byte datagram from an IPv6 sender - no IPv6 send path yet\n", (unsigned)payload_len);
        return;
    }

    Dmod_Printf("udp_echo: %u bytes from %u.%u.%u.%u:%u\n",
                (unsigned)payload_len,
                src->addr.v4[0], src->addr.v4[1], src->addr.v4[2], src->addr.v4[3],
                (unsigned)src_port);

    int send_ret = dmudp_send(src, g_bound_port, src_port, payload, payload_len, DMARP_DEFAULT_TIMEOUT_MS);
    if (send_ret != 0)
        Dmod_Printf("udp_echo: reply send failed (error %d)\n", send_ret);
    else
        g_echoed_count++;
}

static void print_usage(const char* prog)
{
    Dmod_Printf("Usage:\n");
    Dmod_Printf("  %s <port>          Bind <port> and echo every datagram back for %u s\n", prog, (unsigned)UDP_ECHO_DEFAULT_DURATION_S);
    Dmod_Printf("  %s --help | -h     Show this help\n", prog);
    Dmod_Printf("\n");
    Dmod_Printf("Options:\n");
    Dmod_Printf("  -t <seconds>       How long to listen before exiting (default: %u)\n", (unsigned)UDP_ECHO_DEFAULT_DURATION_S);
}

int main(int argc, char* argv[])
{
    const char* prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "udp_echo";

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
    {
        print_usage(prog);
        return 0;
    }

    if (argc < 2)
    {
        print_usage(prog);
        return 1;
    }

    uint32_t port_num;
    if (!parse_uint32(argv[1], &port_num) || port_num == 0 || port_num > 65535)
    {
        Dmod_Printf("%s: invalid port '%s'\n", prog, argv[1]);
        return 1;
    }

    uint32_t duration_s = UDP_ECHO_DEFAULT_DURATION_S;
    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
            if (!parse_uint32(argv[++i], &duration_s) || duration_s == 0)
            {
                Dmod_Printf("%s: invalid duration '%s'\n", prog, argv[i]);
                return 1;
            }
        }
        else
        {
            print_usage(prog);
            return 1;
        }
    }

    g_bound_port = (uint16_t)port_num;
    g_echoed_count = 0;

    int bind_ret = dmudp_bind(g_bound_port, echo_handler);
    if (bind_ret != 0)
    {
        Dmod_Printf("%s: failed to bind UDP port %u (error %d)\n", prog, (unsigned)g_bound_port, bind_ret);
        return 1;
    }

    Dmod_Printf("udp_echo: listening on UDP port %u for %u s\n", (unsigned)g_bound_port, (unsigned)duration_s);

    dmosi_thread_sleep(duration_s * 1000u);

    dmudp_unbind(g_bound_port);

    Dmod_Printf("udp_echo: done, echoed %u datagram(s)\n", (unsigned)g_echoed_count);
    return 0;
}

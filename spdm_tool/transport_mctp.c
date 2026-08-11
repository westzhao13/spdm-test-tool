/* SPDM test tool - MCTP transport via AF_MCTP socket
 * Path: AF_MCTP -> kernel mctp routing -> mctp_bridge0 extraction -> UDP -> receiver -> VU
 * Dual socket: bind smctp_type 0x05 (SPDM) and 0x06 (secured MCTP); sendto picks
 * the socket matching the frame's message_type byte (libspdm transport output).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/mctp.h>

#include "library/spdm_requester_lib.h"
#include "library/spdm_transport_mctp_lib.h"

#include "spdm_tool.h"

#define MCTP_TYPE_SPDM        0x05
#define MCTP_TYPE_SECURED_MCTP 0x06
#define MCTP_DEFAULT_EID      8

static int g_sock_spdm = -1;
static int g_sock_secured = -1;
static uint8_t g_rxbuf[LIBSPDM_RECEIVER_BUFFER_SIZE];
static uint8_t g_peer_eid = MCTP_DEFAULT_EID;

static int mctp_open_sock(uint8_t type)
{
    int sock;
    struct sockaddr_mctp src;

    sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[MCTP] socket");
        return -1;
    }
    memset(&src, 0, sizeof(src));
    src.smctp_family = AF_MCTP;
    src.smctp_network = MCTP_NET_ANY;
    src.smctp_addr.s_addr = MCTP_ADDR_ANY;
    src.smctp_type = type;
    src.smctp_tag = MCTP_TAG_OWNER;
    if (bind(sock, (struct sockaddr *)&src, sizeof(src)) != 0) {
        perror("[MCTP] bind");
        close(sock);
        return -1;
    }
    return sock;
}

static int mctp_send_on(int sock, uint8_t type, const void *buf, size_t len)
{
    struct sockaddr_mctp dst;

    memset(&dst, 0, sizeof(dst));
    dst.smctp_family = AF_MCTP;
    dst.smctp_network = 1;
    dst.smctp_addr.s_addr = g_peer_eid;
    dst.smctp_type = type;
    dst.smctp_tag = MCTP_TAG_OWNER;

    if (sendto(sock, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        fprintf(stderr, "[MCTP] sendto type=0x%02x failed: %s\n", type, strerror(errno));
        return -1;
    }
    return 0;
}

static libspdm_return_t mctp_device_send_message(void *spdm_context,
                                                 size_t message_size,
                                                 const void *message,
                                                 uint64_t timeout)
{
    uint8_t msg_type;

    /* libspdm mctp transport output = [mctp_message_header_t 1B][payload] */
    if (message_size < 1) {
        return LIBSPDM_STATUS_SEND_FAIL;
    }
    msg_type = ((const uint8_t *)message)[0];

    if (msg_type == MCTP_TYPE_SECURED_MCTP) {
        if (g_sock_secured < 0 || mctp_send_on(g_sock_secured, msg_type, message, message_size) != 0) {
            return LIBSPDM_STATUS_SEND_FAIL;
        }
    } else {
        if (g_sock_spdm < 0 || mctp_send_on(g_sock_spdm, msg_type, message, message_size) != 0) {
            return LIBSPDM_STATUS_SEND_FAIL;
        }
    }
    return LIBSPDM_STATUS_SUCCESS;
}

static libspdm_return_t mctp_device_receive_message(void *spdm_context,
                                                    size_t *message_size,
                                                    void **message,
                                                    uint64_t timeout)
{
    struct pollfd pfds[2];
    int nfds = 0;
    int ret;
    ssize_t n;
    struct sockaddr_mctp from;
    socklen_t from_len = sizeof(from);

    if (g_sock_spdm >= 0) {
        pfds[nfds].fd = g_sock_spdm;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    if (g_sock_secured >= 0) {
        pfds[nfds].fd = g_sock_secured;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    if (nfds == 0) {
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }

    int poll_ms = (timeout > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)timeout;

    ret = poll(pfds, nfds, poll_ms);
    if (ret <= 0) {
        fprintf(stderr, "[MCTP] recv poll timeout\n");
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    for (int i = 0; i < nfds; i++) {
        if (!(pfds[i].revents & POLLIN))
            continue;
        n = recvfrom(pfds[i].fd, g_rxbuf, sizeof(g_rxbuf), 0,
                     (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            *message_size = (size_t)n;
            *message = g_rxbuf;
            return LIBSPDM_STATUS_SUCCESS;
        }
    }
    return LIBSPDM_STATUS_RECEIVE_FAIL;
}

int tr_mctp_init(void *spdm_context, const spdm_tool_opts_t *opts)
{
    if (opts->mctp_eid != 0) {
        g_peer_eid = opts->mctp_eid;
    }

    g_sock_spdm = mctp_open_sock(MCTP_TYPE_SPDM);
    g_sock_secured = mctp_open_sock(MCTP_TYPE_SECURED_MCTP);
    if (g_sock_spdm < 0 || g_sock_secured < 0) {
        fprintf(stderr, "[MCTP] socket bind failed (need kernel MCTP + mctp_bridge0 route)\n");
        return -1;
    }
    printf("[MCTP] AF_MCTP sockets ready (EID %u, type 0x05/0x06)\n", g_peer_eid);

    libspdm_register_device_io_func(spdm_context,
                                    mctp_device_send_message,
                                    mctp_device_receive_message);
    libspdm_register_transport_layer_func(spdm_context,
                                          LIBSPDM_RECEIVER_BUFFER_SIZE -
                                              LIBSPDM_MCTP_TRANSPORT_HEADER_SIZE -
                                              LIBSPDM_MCTP_TRANSPORT_TAIL_SIZE,
                                          LIBSPDM_MCTP_TRANSPORT_HEADER_SIZE,
                                          LIBSPDM_MCTP_TRANSPORT_TAIL_SIZE,
                                          libspdm_transport_mctp_encode_message,
                                          libspdm_transport_mctp_decode_message);
    return 0;
}

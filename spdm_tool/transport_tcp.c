/* SPDM test tool - TCP transport (smoke test against spdm_responder_emu)
 * Protocol: requester listens, responder connects and sends Role-Inquiry
 * (DSP0287). Framing: [spdm_tcp_binding_header_t 4B][payload].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "library/spdm_requester_lib.h"
#include "library/spdm_transport_tcp_lib.h"
#include "industry_standard/spdm_tcp_binding.h"

#include "spdm_tool.h"

#define TCP_SPDM_PLATFORM_PORT 4194
#define SOCKET_TRANSPORT_TYPE_TCP 0x03
#define SOCKET_SPDM_COMMAND_NORMAL 0x0001

static int g_sock = -1;
static uint8_t g_rxbuf[LIBSPDM_RECEIVER_BUFFER_SIZE];

static int read_bytes(int fd, uint8_t *buf, size_t len, uint64_t timeout_ms)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = recv(fd, buf + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int write_bytes(int fd, const uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(fd, buf + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* spdm-emu platform framing: [command u32 BE][transport_type u32 BE][length u32 BE][payload] */
static libspdm_return_t tcp_device_send_message(void *spdm_context,
                                                size_t message_size,
                                                const void *message,
                                                uint64_t timeout)
{
    uint8_t hdr[12];
    uint32_t v;

    v = htonl(SOCKET_SPDM_COMMAND_NORMAL);
    memcpy(hdr, &v, 4);
    v = htonl(SOCKET_TRANSPORT_TYPE_TCP);
    memcpy(hdr + 4, &v, 4);
    v = htonl((uint32_t)message_size);
    memcpy(hdr + 8, &v, 4);

    if (write_bytes(g_sock, hdr, sizeof(hdr)) != 0 ||
        write_bytes(g_sock, message, message_size) != 0) {
        fprintf(stderr, "[TCP] send failed: %s\n", strerror(errno));
        return LIBSPDM_STATUS_SEND_FAIL;
    }
    return LIBSPDM_STATUS_SUCCESS;
}

static libspdm_return_t tcp_device_receive_message(void *spdm_context,
                                                   size_t *message_size,
                                                   void **message,
                                                   uint64_t timeout)
{
    uint8_t hdr[12];
    uint32_t command, transport_type, length;

    if (read_bytes(g_sock, hdr, sizeof(hdr), timeout) != 0) {
        fprintf(stderr, "[TCP] recv header failed: %s\n", strerror(errno));
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    memcpy(&command, hdr, 4);
    command = ntohl(command);
    memcpy(&transport_type, hdr + 4, 4);
    transport_type = ntohl(transport_type);
    memcpy(&length, hdr + 8, 4);
    length = ntohl(length);

    if (command != SOCKET_SPDM_COMMAND_NORMAL ||
        transport_type != SOCKET_TRANSPORT_TYPE_TCP) {
        fprintf(stderr, "[TCP] bad frame cmd=0x%x type=0x%x\n", command, transport_type);
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    if (length > sizeof(g_rxbuf)) {
        fprintf(stderr, "[TCP] frame too large: %u\n", length);
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    if (read_bytes(g_sock, g_rxbuf, length, timeout) != 0) {
        fprintf(stderr, "[TCP] recv payload failed\n");
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    *message_size = length;
    *message = g_rxbuf;
    fprintf(stderr, "[TCP] RX %u bytes:", length);
    for (uint32_t i = 0; i < length && i < 24; i++) {
        fprintf(stderr, " %02x", g_rxbuf[i]);
    }
    fprintf(stderr, "\n");
    return LIBSPDM_STATUS_SUCCESS;
}

int tr_tcp_init(void *spdm_context, const spdm_tool_opts_t *opts)
{
    int listen_fd = -1, sock = -1;
    struct sockaddr_in addr;
    socklen_t addrlen;
    uint16_t port = (opts->tcp_port != 0) ? opts->tcp_port : TCP_SPDM_PLATFORM_PORT;
    uint8_t role_inquiry[sizeof(spdm_tcp_binding_header_t)];
    size_t role_inquiry_size = sizeof(role_inquiry);
    uint8_t message_type = 0;
    libspdm_return_t status;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[TCP] socket");
        return -1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[TCP] bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) != 0) {
        perror("[TCP] listen");
        close(listen_fd);
        return -1;
    }
    printf("[TCP] requester listening on port %u, waiting for responder...\n", port);

    addrlen = sizeof(addr);
    sock = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
    close(listen_fd);
    if (sock < 0) {
        perror("[TCP] accept");
        return -1;
    }
    printf("[TCP] responder connected from %s\n", inet_ntoa(addr.sin_addr));

    /* Role-Inquiry exchange: responder sends, requester validates + ACKs */
    if (read_bytes(sock, role_inquiry, sizeof(role_inquiry), 5000) != 0) {
        fprintf(stderr, "[TCP] no role-inquiry from responder\n");
        close(sock);
        return -1;
    }
    status = libspdm_tcp_decode_discovery_message(
        role_inquiry_size, role_inquiry, &message_type);
    if (LIBSPDM_STATUS_IS_ERROR(status) ||
        message_type != SPDM_TCP_MESSAGE_TYPE_ROLE_INQUIRY) {
        fprintf(stderr, "[TCP] bad role-inquiry, type=0x%02x\n", message_type);
        close(sock);
        return -1;
    }
    printf("[TCP] role-inquiry exchange done\n");

    g_sock = sock;

    libspdm_register_device_io_func(spdm_context,
                                    tcp_device_send_message,
                                    tcp_device_receive_message);
    libspdm_register_transport_layer_func(spdm_context,
                                          LIBSPDM_RECEIVER_BUFFER_SIZE -
                                              LIBSPDM_TCP_TRANSPORT_HEADER_SIZE -
                                              LIBSPDM_TCP_TRANSPORT_TAIL_SIZE,
                                          LIBSPDM_TCP_TRANSPORT_HEADER_SIZE,
                                          LIBSPDM_TCP_TRANSPORT_TAIL_SIZE,
                                          libspdm_transport_tcp_encode_message,
                                          libspdm_transport_tcp_decode_message);
    return 0;
}

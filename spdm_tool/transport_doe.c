/* SPDM test tool - DOE transport via UDP client
 * Path: UDP -> receiver (cxl_test_tool --doe-port) -> /dev/doe0 ioctl -> device DOE.
 * DOE data objects (8B DOEHeader + payload) travel verbatim over UDP.
 * PCIe-layer DOE Discovery is performed by tr_doe_discovery() before SPDM starts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "library/spdm_requester_lib.h"
#include "library/spdm_transport_pcidoe_lib.h"
#include "industry_standard/pcidoe.h"

#include "spdm_tool.h"

#define DOE_UDP_TIMEOUT_MS 5000

static int g_udp_fd = -1;
static uint8_t g_rxbuf[LIBSPDM_RECEIVER_BUFFER_SIZE];
static struct sockaddr_in g_dst;

#pragma pack(push, 1)
typedef struct {
    uint16_t vendor_id;
    uint8_t  data_object_type;
    uint8_t  reserved;
    uint32_t length; /* in DW, includes 2-DW header */
} doe_hdr_t;
#pragma pack(pop)

/* One DOE data object exchange over UDP (blocking). */
static int doe_udp_exchange(const uint8_t *req, size_t req_len,
                            uint8_t *rsp, size_t *rsp_len)
{
    ssize_t n;

    if (send(g_udp_fd, req, req_len, 0) < 0) {
        fprintf(stderr, "[DOE] udp send failed: %s\n", strerror(errno));
        return -1;
    }
    n = recv(g_udp_fd, rsp, LIBSPDM_RECEIVER_BUFFER_SIZE, 0);
    if (n < 0) {
        fprintf(stderr, "[DOE] udp recv failed: %s\n", strerror(errno));
        return -1;
    }
    *rsp_len = (size_t)n;
    return 0;
}

static libspdm_return_t doe_device_send_message(void *spdm_context,
                                                size_t message_size,
                                                const void *message,
                                                uint64_t timeout)
{
    /* libspdm pci_doe transport output = complete DOE data object (header + payload) */
    if (send(g_udp_fd, message, message_size, 0) < 0) {
        fprintf(stderr, "[DOE] udp send failed: %s\n", strerror(errno));
        return LIBSPDM_STATUS_SEND_FAIL;
    }
    return LIBSPDM_STATUS_SUCCESS;
}

static libspdm_return_t doe_device_receive_message(void *spdm_context,
                                                   size_t *message_size,
                                                   void **message,
                                                   uint64_t timeout)
{
    ssize_t n;
    struct timeval tv;
    socklen_t tvlen = sizeof(tv);
    uint64_t tmo = (timeout == 0) ? DOE_UDP_TIMEOUT_MS : timeout;

    tv.tv_sec = (time_t)(tmo / 1000);
    tv.tv_usec = (suseconds_t)((tmo % 1000) * 1000);
    setsockopt(g_udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, tvlen);

    n = recv(g_udp_fd, g_rxbuf, sizeof(g_rxbuf), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "[DOE] udp recv timeout\n");
        } else {
            fprintf(stderr, "[DOE] udp recv failed: %s\n", strerror(errno));
        }
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    *message_size = (size_t)n;
    *message = g_rxbuf;
    return LIBSPDM_STATUS_SUCCESS;
}

/* PCIe DOE Discovery: iterate index, collect (vid, type) until next_index==0.
 * Verifies SPDM (0x01) and Secured SPDM (0x02) under PCI-SIG vendor 0x0001. */
int tr_doe_discovery(bool *spdm_ok, bool *secured_ok)
{
    uint8_t index = 0;
    int guard = 0;

    if (g_udp_fd < 0) {
        fprintf(stderr, "[DOE] discovery: UDP socket not connected\n");
        return -1;
    }
    *spdm_ok = false;
    *secured_ok = false;

    for (;;) {
        uint8_t req[12];
        uint8_t rsp[LIBSPDM_RECEIVER_BUFFER_SIZE];
        size_t rsp_len = sizeof(rsp);
        doe_hdr_t *hdr;
        uint32_t dw;
        uint16_t vid;
        uint8_t type, next_index;

        /* Discovery request: vendor PCI-SIG, type 0x00, len 3 DW (hdr + index) */
        memset(req, 0, sizeof(req));
        hdr = (doe_hdr_t *)req;
        hdr->vendor_id = PCI_DOE_VENDOR_ID_PCISIG;
        hdr->data_object_type = PCI_DOE_DATA_OBJECT_TYPE_DOE_DISCOVERY;
        hdr->length = 3;
        memcpy(req + sizeof(doe_hdr_t), &index, 1);

        if (doe_udp_exchange(req, sizeof(req), rsp, &rsp_len) != 0) {
            return -1;
        }
        if (rsp_len < sizeof(doe_hdr_t) + 4) {
            fprintf(stderr, "[DOE] short discovery response: %zu\n", rsp_len);
            return -1;
        }
        /* entry (1 DW) follows the 8B DOE data object header:
         * [15:0] vid, [23:16] type, [31:24] next_index */
        const uint8_t *entry = rsp + sizeof(doe_hdr_t);
        dw = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8) |
             ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24);
        vid = (uint16_t)(dw & 0xffff);
        type = (uint8_t)((dw >> 16) & 0xff);
        next_index = (uint8_t)((dw >> 24) & 0xff);

        printf("[DOE] discovery[%u]: vid=0x%04x type=0x%02x next=0x%02x\n",
               index, vid, type, next_index);
        if (vid == PCI_DOE_VENDOR_ID_PCISIG && type == PCI_DOE_DATA_OBJECT_TYPE_SPDM) {
            *spdm_ok = true;
        }
        if (vid == PCI_DOE_VENDOR_ID_PCISIG && type == PCI_DOE_DATA_OBJECT_TYPE_SECURED_SPDM) {
            *secured_ok = true;
        }
        index = next_index;
        if (index == 0 || ++guard > 64) {
            break;
        }
    }
    return 0;
}

int tr_doe_init(void *spdm_context, const spdm_tool_opts_t *opts)
{
    char host[128];
    uint16_t port;
    const char *sep;
    struct addrinfo hints, *res = NULL;
    int ret;

    if (opts->doe_udp == NULL) {
        fprintf(stderr, "[DOE] --doe-udp host:port required\n");
        return -1;
    }
    sep = strrchr(opts->doe_udp, ':');
    if (sep == NULL || (size_t)(sep - opts->doe_udp) >= sizeof(host)) {
        fprintf(stderr, "[DOE] bad --doe-udp (expect host:port)\n");
        return -1;
    }
    memcpy(host, opts->doe_udp, (size_t)(sep - opts->doe_udp));
    host[sep - opts->doe_udp] = '\0';
    port = (uint16_t)atoi(sep + 1);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    ret = getaddrinfo(host, sep + 1, &hints, &res);
    if (ret != 0) {
        fprintf(stderr, "[DOE] getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    g_udp_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_udp_fd < 0) {
        perror("[DOE] socket");
        freeaddrinfo(res);
        return -1;
    }
    if (connect(g_udp_fd, res->ai_addr, res->ai_addrlen) != 0) {
        perror("[DOE] connect");
        freeaddrinfo(res);
        close(g_udp_fd);
        g_udp_fd = -1;
        return -1;
    }
    memcpy(&g_dst, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    printf("[DOE] UDP client -> %s:%u\n", host, port);

    libspdm_register_device_io_func(spdm_context,
                                    doe_device_send_message,
                                    doe_device_receive_message);
    libspdm_register_transport_layer_func(spdm_context,
                                          LIBSPDM_RECEIVER_BUFFER_SIZE -
                                              LIBSPDM_PCI_DOE_TRANSPORT_HEADER_SIZE -
                                              LIBSPDM_PCI_DOE_TRANSPORT_TAIL_SIZE,
                                          LIBSPDM_PCI_DOE_TRANSPORT_HEADER_SIZE,
                                          LIBSPDM_PCI_DOE_TRANSPORT_TAIL_SIZE,
                                          libspdm_transport_pci_doe_encode_message,
                                          libspdm_transport_pci_doe_decode_message);
    return 0;
}

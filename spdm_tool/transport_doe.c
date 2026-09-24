/* SPDM test tool - DOE transport
 * Two exchange backends behind the same libspdm pci_doe transport:
 *   direct : /dev/doeN ioctl (doe.ko) -> device DOE mailbox
 *   udp    : UDP -> receiver (cxl_test_tool --doe-port) -> /dev/doe0 -> device
 * DOE data objects (8B DOEHeader + payload) travel verbatim in both modes.
 * PCIe-layer DOE Discovery is performed by tr_doe_discovery() before SPDM starts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "library/spdm_requester_lib.h"
#include "library/spdm_transport_pcidoe_lib.h"
#include "industry_standard/pcidoe.h"

#include "spdm_tool.h"

#define DOE_UDP_TIMEOUT_MS 5000

/* -- direct backend: doe.ko char device ioctl ------------------------- */

/* doe.ko uapi contract (doe_test_app/driver/doe_api.h + doe.h).
 * Two cmd encodings exist across driver generations; the driver copies
 * DOE_IOCTL_BUF_SIZE bytes both ways regardless of the _IOC size bits:
 *   deployed (Sep '26): _IO('N', 0xb7)          - 1MB payload does not fit _IOC bits
 *   older   (Aug '26):  _IOWR('N', 0xb7, 2048)  - struct doe_buf size encoded
 * g_mbox_cmd starts on the deployed encoding and falls back once on ENOTTY. */
#define DOE_IOCTL_MBOX_CMD_NEW  _IO('N', 0xb7)
struct doe_ioctl_2048 { char buf[2048]; };
#define DOE_IOCTL_MBOX_CMD_OLD  _IOWR('N', 0xb7, struct doe_ioctl_2048)
#define DOE_IOCTL_MAX_DW_SIZE  (1 << 18)                    /* doe.h */
#define DOE_IOCTL_BUF_SIZE  ((DOE_IOCTL_MAX_DW_SIZE + 1) * sizeof(uint32_t))

/* DOE Extended Capability instance offsets on this device's firmware
 * (doe_test_app Makefile -D flags, NOT spec values; driver ioctl accepts
 * only these two). normal=CDAT-class protocols, security=SPDM-class. */
#define DOE_CAP_NORMAL_OFF   0xd00u
#define DOE_CAP_SECURITY_OFF 0xd80u

static int g_udp_fd = -1;
static int g_dev_fd = -1;        /* direct backend */
static unsigned g_mbox_cmd = DOE_IOCTL_MBOX_CMD_NEW;
static uint8_t *g_dev_buf;       /* ioctl buffer, DOE_IOCTL_BUF_SIZE */
static uint32_t g_cap_off;       /* selected DOE instance */
static bool g_cap_raw;           /* cap offset came from --doe-cap-offset */
static uint8_t *g_rsp;            /* direct mode: response cached by send */
static size_t g_rsp_len;
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

/* One DOE data object exchange (blocking). *rsp points into a backend-owned
 * buffer valid until the next exchange. */
static int doe_exchange(const uint8_t *req, size_t req_len,
                        const uint8_t **rsp, size_t *rsp_len)
{
    if (g_dev_fd >= 0) {
        uint32_t len_dw;

        if (req_len < sizeof(doe_hdr_t) ||
            req_len + sizeof(uint32_t) > DOE_IOCTL_BUF_SIZE) {
            fprintf(stderr, "[DOE] bad request size %zu\n", req_len);
            return -1;
        }
        *(uint32_t *)g_dev_buf = g_cap_off;   /* DW[0]: instance selector */
        memcpy(g_dev_buf + sizeof(uint32_t), req, req_len);
        int rc = ioctl(g_dev_fd, g_mbox_cmd, g_dev_buf);
        if (rc != 0 && errno == ENOTTY &&
            g_mbox_cmd != (unsigned)DOE_IOCTL_MBOX_CMD_OLD) {
            /* doe.ko returns ENOTTY both for an unknown ioctl command and for a
             * cap offset this device does not expose (doe_main.c: "can't find
             * the required capability"). Try the legacy encoding, but latch onto
             * it only if it actually works: with a wrong --doe-cap both fail,
             * and latching there would blame the driver generation for what is
             * really a missing DOE instance. */
            int first_errno = errno;
            rc = ioctl(g_dev_fd, (unsigned)DOE_IOCTL_MBOX_CMD_OLD, g_dev_buf);
            if (rc == 0) {
                fprintf(stderr, "[DOE] cmd 0x%08x not recognized, "
                        "using legacy encoding 0x%08x\n",
                        g_mbox_cmd, (unsigned)DOE_IOCTL_MBOX_CMD_OLD);
                g_mbox_cmd = (unsigned)DOE_IOCTL_MBOX_CMD_OLD;
            } else {
                errno = first_errno;
            }
        }
        if (rc != 0) {
            /* EBUSY  = in-mailbox device exchange timeout (~1s)
             * EINVAL = cap offset is not one this driver build accepts
             * ENOTTY = neither ioctl encoding worked, or the cap offset is not
             *          registered on this device - the instance comes first */
            fprintf(stderr, "[DOE] ioctl(cmd 0x%08x, cap 0x%x) failed: %s\n",
                    g_mbox_cmd, g_cap_off, strerror(errno));
            if (errno == ENOTTY) {
                fprintf(stderr, "[DOE] no DOE capability at 0x%x on this device, or "
                        "neither ioctl encoding is recognized - try the other "
                        "instance (--doe-cap %s), then compare with: "
                        "strace ./doe -s <bdf> -t 1\n",
                        g_cap_off,
                        g_cap_off == DOE_CAP_SECURITY_OFF ? "normal" : "security");
            } else if (errno == EBUSY) {
                fprintf(stderr, "[DOE] device exchange timed out - firmware/IRQ state?\n");
            } else if (errno == EINVAL) {
                fprintf(stderr, "[DOE] this driver build accepts only 0xd00 (normal) / "
                        "0xd80 (security); cap 0x%x was rejected - "
                        "check `lspci -vv` for the device's real DOE offsets\n",
                        g_cap_off);
            }
            return -1;
        }
        /* The driver copies the response over the buffer from DW[0]. A
         * zero-length copy leaves the buffer untouched, so DW[0] would still
         * hold the selector we just wrote and DW[1] the request's own header
         * word - a bogus nonzero length that sails through the check below. A
         * real response always overwrites DW[0] with its DOE header, whose low
         * half is the PCI-SIG vendor id 0x0001; no accepted cap offset can
         * equal that, so this discriminates cleanly. */
        {
            uint32_t rsp_dw0;
            memcpy(&rsp_dw0, g_dev_buf, sizeof(rsp_dw0));
            if (rsp_dw0 == g_cap_off) {
                fprintf(stderr, "[DOE] device returned an empty response\n");
                return -1;
            }
        }
        /* response header: DW[0], length in DW[1] bits[17:0] (doe.h DOEHeader) */
        len_dw = *(const uint32_t *)(g_dev_buf + 4) & 0x3ffffu;
        *rsp_len = (size_t)len_dw * sizeof(uint32_t);
        if (len_dw == 0 || *rsp_len > DOE_IOCTL_BUF_SIZE) {
            fprintf(stderr, "[DOE] bad response length %u DW\n", len_dw);
            return -1;
        }
        *rsp = g_dev_buf;
        return 0;
    }

    if (g_udp_fd < 0) {
        fprintf(stderr, "[DOE] exchange: transport not initialized\n");
        return -1;
    }
    if (send(g_udp_fd, req, req_len, 0) < 0) {
        fprintf(stderr, "[DOE] udp send failed: %s\n", strerror(errno));
        return -1;
    }
    *rsp_len = recv(g_udp_fd, g_rxbuf, sizeof(g_rxbuf), 0);
    if ((ssize_t)*rsp_len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Bounded by the SO_RCVTIMEO set at socket creation - without it
             * discovery would block here forever when the receiver is down. */
            fprintf(stderr, "[DOE] udp recv timeout (%d ms) - receiver running?\n",
                    DOE_UDP_TIMEOUT_MS);
        } else {
            fprintf(stderr, "[DOE] udp recv failed: %s\n", strerror(errno));
        }
        return -1;
    }
    *rsp = g_rxbuf;
    return 0;
}

static libspdm_return_t doe_device_send_message(void *spdm_context,
                                                size_t message_size,
                                                const void *message,
                                                uint64_t timeout)
{
    /* libspdm pci_doe transport output = complete DOE data object (header + payload) */
    if (g_dev_fd >= 0) {
        /* ponytail: ioctl does the whole exchange; timeout arg is ignored
         * (driver's internal ~1s limit is the only ceiling). */
        if (doe_exchange(message, message_size, (const uint8_t **)&g_rsp,
                         &g_rsp_len) != 0) {
            return LIBSPDM_STATUS_SEND_FAIL;
        }
        return LIBSPDM_STATUS_SUCCESS;
    }
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
    uint64_t tmo;

    if (g_dev_fd >= 0) {
        if (g_rsp == NULL) {
            return LIBSPDM_STATUS_RECEIVE_FAIL;
        }
        *message = g_rsp;
        *message_size = g_rsp_len;
        g_rsp = NULL;   /* consumed */
        return LIBSPDM_STATUS_SUCCESS;
    }

    /* libspdm passes this in MICROseconds (spdm_common_lib.h: "The timeout, in
     * microsends"); a requester gets rtt + ST1, e.g. 0 + 100000 us. Our own
     * DOE_UDP_TIMEOUT_MS default is in MILLIseconds, so each is converted in
     * its own unit - treating the microsecond value as ms inflated the wait
     * 1000x (100 s instead of 100 ms). */
    if (timeout == 0) {
        tmo = DOE_UDP_TIMEOUT_MS * 1000;
    } else {
        tmo = timeout;
    }
    tv.tv_sec = (time_t)(tmo / 1000000);
    tv.tv_usec = (suseconds_t)(tmo % 1000000);
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

    if (g_udp_fd < 0 && g_dev_fd < 0) {
        fprintf(stderr, "[DOE] discovery: transport not initialized\n");
        return -1;
    }
    *spdm_ok = false;
    *secured_ok = false;

    for (;;) {
        uint8_t req[12];
        const uint8_t *rsp;
        size_t rsp_len;
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

        if (doe_exchange(req, sizeof(req), &rsp, &rsp_len) != 0) {
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
    if (g_dev_fd >= 0 && !*spdm_ok) {
        if (g_cap_raw) {
            fprintf(stderr, "[DOE] SPDM (type 0x01) not advertised on instance 0x%x; "
                    "try the other DOE instance's offset\n", g_cap_off);
        } else {
            fprintf(stderr, "[DOE] SPDM (type 0x01) not advertised on instance 0x%x; "
                    "retry with --doe-cap %s\n", g_cap_off,
                    g_cap_off == DOE_CAP_NORMAL_OFF ? "security" : "normal");
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

    if (opts->doe_dev != NULL) {
        if (opts->doe_cap_set) {
            g_cap_off = opts->doe_cap_offset;
            g_cap_raw = true;
        } else {
            g_cap_off = opts->doe_cap_security ? DOE_CAP_SECURITY_OFF
                                               : DOE_CAP_NORMAL_OFF;
        }
        g_dev_buf = malloc(DOE_IOCTL_BUF_SIZE);
        if (g_dev_buf == NULL) {
            fprintf(stderr, "[DOE] out of memory\n");
            return -1;
        }
        g_dev_fd = open(opts->doe_dev, O_RDWR | O_SYNC);
        if (g_dev_fd < 0) {
            fprintf(stderr, "[DOE] open %s failed: %s "
                    "(doe.ko loaded? try scripts/switch_mode.sh doe)\n",
                    opts->doe_dev, strerror(errno));
            free(g_dev_buf);
            g_dev_buf = NULL;
            return -1;
        }
        printf("[DOE] direct %s (instance %s, cap off 0x%x, ioctl cmd 0x%08x)\n",
               opts->doe_dev, opts->doe_cap_set ? "raw"
               : opts->doe_cap_security ? "security" : "normal",
               g_cap_off, g_mbox_cmd);
        goto register_transport;
    }

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

    /* Default bound for every recv on this socket, including the bare ones in
     * doe_exchange() that discovery uses. The per-call override in
     * doe_device_receive_message() only covers libspdm's own receive path, so
     * without this a missing receiver hangs discovery indefinitely. */
    {
        struct timeval tv;
        tv.tv_sec = DOE_UDP_TIMEOUT_MS / 1000;
        tv.tv_usec = (DOE_UDP_TIMEOUT_MS % 1000) * 1000;
        if (setsockopt(g_udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            perror("[DOE] SO_RCVTIMEO");
        }
    }

    printf("[DOE] UDP client -> %s:%u\n", host, port);

register_transport:
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

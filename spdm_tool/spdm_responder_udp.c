/* Minimal UDP DOE SPDM responder for end-to-end verification.
 * Uses the libspdm responder library with a UDP device-I/O layer.
 * DOE data objects travel verbatim over UDP (matching transport_doe.c).
 *
 * Run from spdm-emu/build/bin so the device_secret_lib_sample private-key
 * loader finds "ecp384/end_responder.key"; the certificate chain is read
 * from "ecp384/bundle_responder.certchain.der".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "library/spdm_responder_lib.h"
#include "library/spdm_common_lib.h"
#include "library/spdm_transport_pcidoe_lib.h"
#include "industry_standard/spdm.h"

#define DOE_UDP_PORT 2326
#define BUF_SIZE 0x2000

static int g_udp_fd = -1;
static struct sockaddr_in g_peer;
static socklen_t g_peer_len;
static uint8_t g_sender_buf[BUF_SIZE];
static uint8_t g_receiver_buf[BUF_SIZE];
static uint8_t g_pending_buf[BUF_SIZE];
static size_t g_pending_len;
static bool g_has_pending;

#pragma pack(push, 1)
typedef struct {
    uint16_t vendor_id;
    uint8_t data_object_type;
    uint8_t reserved;
    uint32_t length;
} doe_hdr_t;
#pragma pack(pop)

static libspdm_return_t doe_device_send_message(void *spdm_context,
                                                size_t message_size,
                                                const void *message,
                                                uint64_t timeout)
{
    int sent;

    sent = (int)sendto(g_udp_fd, message, message_size, 0,
                       (struct sockaddr *)&g_peer, g_peer_len);
    printf("[RSP] send %zuB to %s:%u -> %d (%s)\n",
           message_size, inet_ntoa(g_peer.sin_addr), ntohs(g_peer.sin_port),
           sent, sent < 0 ? strerror(errno) : "ok");
    fflush(stdout);
    if (sent < 0) {
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

    /* A non-discovery frame was already captured by the main loop. */
    if (g_has_pending) {
        *message_size = g_pending_len;
        *message = g_pending_buf;
        g_has_pending = false;
        return LIBSPDM_STATUS_SUCCESS;
    }
    g_peer_len = sizeof(g_peer);
    n = recvfrom(g_udp_fd, g_receiver_buf, sizeof(g_receiver_buf), 0,
                 (struct sockaddr *)&g_peer, &g_peer_len);
    if (n < 0) {
        perror("[RSP] udp recvfrom");
        return LIBSPDM_STATUS_RECEIVE_FAIL;
    }
    *message_size = (size_t)n;
    *message = g_receiver_buf;
    return LIBSPDM_STATUS_SUCCESS;
}

/* DOE Discovery (PCIe-layer, not SPDM): answer from a fixed capability table
 * so the requester sees SPDM(0x01)/SecuredSPDM(0x02) before starting SPDM. */
static void handle_discovery(const uint8_t *req, size_t req_len)
{
    uint8_t index = (req_len > 8) ? req[8] : 0;
    uint8_t rsp[12];
    doe_hdr_t *rh = (doe_hdr_t *)rsp;
    uint32_t entry;
    int sent;

    memset(rsp, 0, sizeof(rsp));
    rh->vendor_id = PCI_DOE_VENDOR_ID_PCISIG;
    rh->data_object_type = PCI_DOE_DATA_OBJECT_TYPE_DOE_DISCOVERY;
    rh->length = 3;
    if (index == 0) {
        entry = (uint32_t)PCI_DOE_VENDOR_ID_PCISIG |
                ((uint32_t)PCI_DOE_DATA_OBJECT_TYPE_SPDM << 16) |
                ((uint32_t)0x01 << 24);
    } else if (index == 1) {
        entry = (uint32_t)PCI_DOE_VENDOR_ID_PCISIG |
                ((uint32_t)PCI_DOE_DATA_OBJECT_TYPE_SECURED_SPDM << 16);
    } else {
        entry = 0; /* no more protocols */
    }
    memcpy(rsp + 8, &entry, sizeof(entry));
    sent = (int)sendto(g_udp_fd, rsp, sizeof(rsp), 0,
                       (struct sockaddr *)&g_peer, g_peer_len);
    printf("[RSP] discovery index=%u peer=%s:%u len=%u -> sendto %d (%s)\n",
           index, inet_ntoa(g_peer.sin_addr), ntohs(g_peer.sin_port),
           g_peer_len, sent, sent < 0 ? strerror(errno) : "ok");
    fflush(stdout);
}

static libspdm_return_t rsp_acquire_sender(void *ctx, void **buf)
{
    *buf = g_sender_buf;
    return LIBSPDM_STATUS_SUCCESS;
}
static void rsp_release_sender(void *ctx, const void *msg) {}
static libspdm_return_t rsp_acquire_receiver(void *ctx, void **buf)
{
    *buf = g_receiver_buf;
    return LIBSPDM_STATUS_SUCCESS;
}
static void rsp_release_receiver(void *ctx, const void *msg) {}

static bool read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    long len;
    uint8_t *buf;

    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) {
        fclose(fp);
        return false;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        fclose(fp);
        return false;
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    *data = buf;
    *size = (size_t)len;
    return true;
}

/* Integrator-provided file I/O required by spdm_device_secret_lib_sample. */
bool libspdm_read_input_file(const char *file_name, void **file_data,
                             size_t *file_size)
{
    return read_file(file_name, (uint8_t **)file_data, file_size);
}

bool libspdm_write_output_file(const char *file_name, const void *file_data,
                               size_t file_size)
{
    FILE *fp = fopen(file_name, "wb");
    if (fp == NULL) {
        return false;
    }
    if (file_size > 0 && fwrite(file_data, 1, file_size, fp) != file_size) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

void libspdm_dump_hex_str(const uint8_t *buffer, size_t buffer_size)
{
    size_t index;

    for (index = 0; index < buffer_size; index++) {
        printf("%02x", buffer[index]);
    }
}

/* Provided by spdm_device_secret_lib_sample; builds a proper SPDM cert chain
 * (length + root_hash + DER certs) from sample_key files. */
extern bool libspdm_read_responder_public_certificate_chain(
    uint32_t base_hash_algo, uint32_t base_asym_algo, void **data,
    size_t *size, void **hash, size_t *hash_size);

static void set_data(void *ctx, libspdm_data_type_t type, const void *data, size_t size)
{
    libspdm_data_parameter_t param;
    libspdm_return_t status;

    memset(&param, 0, sizeof(param));
    param.location = LIBSPDM_DATA_LOCATION_LOCAL;
    status = libspdm_set_data(ctx, type, &param, data, size);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "[RSP] set_data(0x%x) failed: 0x%x\n", type, status);
        exit(1);
    }
}

int main(void)
{
    void *ctx;
    size_t scratch_size;
    void *scratch;
    struct sockaddr_in addr;
    uint32_t flags, base_asym, base_hash, meas_hash;
    uint16_t dhe, aead, key_schedule;
    uint8_t meas_spec, other_params, slot_mask;
    libspdm_data_parameter_t param;

    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) {
        perror("[RSP] socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DOE_UDP_PORT);
    if (bind(g_udp_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[RSP] bind");
        return 1;
    }

    ctx = malloc(libspdm_get_context_size());
    if (ctx == NULL) {
        return 1;
    }
    libspdm_init_context(ctx);

    libspdm_register_device_io_func(ctx, doe_device_send_message,
                                    doe_device_receive_message);
    libspdm_register_transport_layer_func(ctx,
                                          BUF_SIZE - LIBSPDM_PCI_DOE_TRANSPORT_HEADER_SIZE -
                                          LIBSPDM_PCI_DOE_TRANSPORT_TAIL_SIZE,
                                          LIBSPDM_PCI_DOE_TRANSPORT_HEADER_SIZE,
                                          LIBSPDM_PCI_DOE_TRANSPORT_TAIL_SIZE,
                                          libspdm_transport_pci_doe_encode_message,
                                          libspdm_transport_pci_doe_decode_message);
    libspdm_register_device_buffer_func(ctx, BUF_SIZE, BUF_SIZE,
                                        rsp_acquire_sender, rsp_release_sender,
                                        rsp_acquire_receiver, rsp_release_receiver);
    scratch_size = libspdm_get_sizeof_required_scratch_buffer(ctx);
    scratch = malloc(scratch_size);
    if (scratch == NULL) {
        return 1;
    }
    libspdm_set_scratch_buffer(ctx, scratch, scratch_size);

    /* responder capabilities matching the tool's requester.
     * Omit PSK_CAP (0x400|0x800 -> psk_cap field=3, reserved) and use
     * MEAS_CAP_SIG only (MEAS_CAP = 0x8|0x10 -> meas_cap field=3, reserved). */
    flags = SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_CERT_CAP |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_CHAL_CAP |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_MEAS_CAP_SIG |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_ENCRYPT_CAP |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_MAC_CAP |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_KEY_EX_CAP |
            SPDM_GET_CAPABILITIES_RESPONSE_FLAGS_HANDSHAKE_IN_THE_CLEAR_CAP;
    set_data(ctx, LIBSPDM_DATA_CAPABILITY_FLAGS, &flags, sizeof(flags));

    base_asym = SPDM_ALGORITHMS_BASE_ASYM_ALGO_TPM_ALG_ECDSA_ECC_NIST_P384;
    base_hash = SPDM_ALGORITHMS_BASE_HASH_ALGO_TPM_ALG_SHA_384;
    dhe       = SPDM_ALGORITHMS_DHE_NAMED_GROUP_SECP_384_R1;
    aead      = SPDM_ALGORITHMS_AEAD_CIPHER_SUITE_AES_256_GCM;
    key_schedule = SPDM_ALGORITHMS_KEY_SCHEDULE_SPDM;
    meas_spec = SPDM_MEASUREMENT_SPECIFICATION_DMTF;
    meas_hash = SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_256 |
                SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_384 |
                SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_512;
    other_params = 0x01;

    set_data(ctx, LIBSPDM_DATA_BASE_ASYM_ALGO, &base_asym, sizeof(base_asym));
    set_data(ctx, LIBSPDM_DATA_BASE_HASH_ALGO, &base_hash, sizeof(base_hash));
    set_data(ctx, LIBSPDM_DATA_DHE_NAME_GROUP, &dhe, sizeof(dhe));
    set_data(ctx, LIBSPDM_DATA_AEAD_CIPHER_SUITE, &aead, sizeof(aead));
    set_data(ctx, LIBSPDM_DATA_KEY_SCHEDULE, &key_schedule, sizeof(key_schedule));
    set_data(ctx, LIBSPDM_DATA_MEASUREMENT_SPEC, &meas_spec, sizeof(meas_spec));
    set_data(ctx, LIBSPDM_DATA_MEASUREMENT_HASH_ALGO, &meas_hash, sizeof(meas_hash));
    set_data(ctx, LIBSPDM_DATA_OTHER_PARAMS_SUPPORT, &other_params, sizeof(other_params));

    /* responder cert chain, slot 0: use sample lib to build SPDM cert chain
     * (length + root_hash + DER) from sample_key/ecp384 bundle. */
    {
        void *cert_chain_data = NULL;
        size_t cert_chain_size = 0;

        if (!libspdm_read_responder_public_certificate_chain(
                SPDM_ALGORITHMS_BASE_HASH_ALGO_TPM_ALG_SHA_384,
                SPDM_ALGORITHMS_BASE_ASYM_ALGO_TPM_ALG_ECDSA_ECC_NIST_P384,
                &cert_chain_data, &cert_chain_size, NULL, NULL)) {
            fprintf(stderr, "[RSP] read responder cert chain failed (run from spdm-emu/build/bin)\n");
            return 1;
        }
        memset(&param, 0, sizeof(param));
        param.location = LIBSPDM_DATA_LOCATION_LOCAL;
        param.additional_data[0] = 0; /* slot 0 */
        if (LIBSPDM_STATUS_IS_ERROR(libspdm_set_data(ctx, LIBSPDM_DATA_LOCAL_PUBLIC_CERT_CHAIN,
                                                     &param, cert_chain_data, cert_chain_size))) {
            fprintf(stderr, "[RSP] set cert chain failed\n");
            return 1;
        }
    }

    /* SPDM 1.3+: DIGESTS response SupportedSlotMask (param1) must cover
     * ProvisionedSlotMask (param2); provision slot 0 as supported. */
    slot_mask = 0x01;
    memset(&param, 0, sizeof(param));
    param.location = LIBSPDM_DATA_LOCATION_LOCAL;
    if (LIBSPDM_STATUS_IS_ERROR(libspdm_set_data(ctx, LIBSPDM_DATA_LOCAL_SUPPORTED_SLOT_MASK,
                                                 &param, &slot_mask, sizeof(slot_mask)))) {
        fprintf(stderr, "[RSP] set supported slot mask failed\n");
        return 1;
    }

    printf("[RSP] UDP DOE responder ready on port %u\n", DOE_UDP_PORT);
    fflush(stdout);

    while (1) {
        ssize_t n;
        uint8_t type;

        g_peer_len = sizeof(g_peer);
        n = recvfrom(g_udp_fd, g_receiver_buf, sizeof(g_receiver_buf), 0,
                     (struct sockaddr *)&g_peer, &g_peer_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("[RSP] recvfrom");
            break;
        }
        if (n < (ssize_t)sizeof(doe_hdr_t))
            continue;

        type = ((doe_hdr_t *)g_receiver_buf)->data_object_type;
        if (type == PCI_DOE_DATA_OBJECT_TYPE_DOE_DISCOVERY) {
            handle_discovery(g_receiver_buf, (size_t)n);
        } else {
            memcpy(g_pending_buf, g_receiver_buf, (size_t)n);
            g_pending_len = (size_t)n;
            g_has_pending = true;
            libspdm_responder_dispatch_message(ctx);
        }
    }

    return 0;
}

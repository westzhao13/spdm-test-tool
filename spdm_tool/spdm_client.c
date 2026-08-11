/* SPDM test tool - libspdm client init + SPDM flow orchestration
 * Reference: spdm-emu spdm_requester_emu/spdm_requester_spdm.c:171-238
 *           libspdm doc/user_guide.md:74-100
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/spdm_requester_lib.h"
#include "library/spdm_common_lib.h"
#include "library/spdm_lib_config.h"
#include "library/spdm_transport_mctp_lib.h"
#include "library/spdm_transport_pcidoe_lib.h"
#include "library/spdm_transport_tcp_lib.h"
#include "industry_standard/spdm.h"

#include "spdm_tool.h"

/* transport init entry points (implemented per transport file) */
int tr_tcp_init(void *spdm_context, const spdm_tool_opts_t *opts);
int tr_mctp_init(void *spdm_context, const spdm_tool_opts_t *opts);
int tr_doe_init(void *spdm_context, const spdm_tool_opts_t *opts);
int tr_doe_discovery(bool *spdm_ok, bool *secured_ok);

static uint8_t *g_sender_buf;
static uint8_t *g_receiver_buf;

static libspdm_return_t spdm_device_acquire_sender_buffer(void *spdm_context,
                                                          void **msg_buf_ptr)
{
    *msg_buf_ptr = g_sender_buf;
    return LIBSPDM_STATUS_SUCCESS;
}

static void spdm_device_release_sender_buffer(void *spdm_context, const void *message)
{
}

static libspdm_return_t spdm_device_acquire_receiver_buffer(void *spdm_context,
                                                            void **msg_buf_ptr)
{
    *msg_buf_ptr = g_receiver_buf;
    return LIBSPDM_STATUS_SUCCESS;
}

static void spdm_device_release_receiver_buffer(void *spdm_context, const void *message)
{
}

/* Read a DER root cert file, return malloc'd buffer (caller frees) */
static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long len;

    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size = (size_t)len;
    return buf;
}

static void set_data(void *spdm_context, libspdm_data_type_t type,
                     libspdm_data_location_t location, const void *data, size_t size)
{
    libspdm_data_parameter_t parameter;
    libspdm_return_t status;

    memset(&parameter, 0, sizeof(parameter));
    parameter.location = location;
    status = libspdm_set_data(spdm_context, type, &parameter, data, size);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "spdm_set_data(0x%x) failed: 0x%x\n", type, status);
        exit(1);
    }
}

static void *spdm_client_init(const spdm_tool_opts_t *opts)
{
    void *spdm_context;
    size_t scratch_buffer_size;
    void *scratch_buffer;
    uint32_t cap_flags;
    uint32_t base_asym, base_hash, meas_hash;
    uint16_t dhe, aead, req_asym, key_schedule;
    uint8_t meas_spec, other_params;
    spdm_version_number_t ver = {0};
    uint32_t spdm_version = 0;
    uint8_t *cert = NULL;
    size_t cert_size = 0;

    spdm_context = malloc(libspdm_get_context_size());
    if (spdm_context == NULL) {
        return NULL;
    }
    libspdm_init_context(spdm_context);

    /* capabilities (requester) */
    cap_flags = SPDM_GET_CAPABILITIES_REQUEST_FLAGS_CERT_CAP |
                SPDM_GET_CAPABILITIES_REQUEST_FLAGS_CHAL_CAP |
                SPDM_GET_CAPABILITIES_REQUEST_FLAGS_ENCRYPT_CAP |
                SPDM_GET_CAPABILITIES_REQUEST_FLAGS_MAC_CAP |
                SPDM_GET_CAPABILITIES_REQUEST_FLAGS_KEY_EX_CAP |
                SPDM_GET_CAPABILITIES_REQUEST_FLAGS_HANDSHAKE_IN_THE_CLEAR_CAP;
    set_data(spdm_context, LIBSPDM_DATA_CAPABILITY_FLAGS, LIBSPDM_DATA_LOCATION_LOCAL,
             &cap_flags, sizeof(cap_flags));

    /* algorithms */
    base_asym = SPDM_ALGORITHMS_BASE_ASYM_ALGO_TPM_ALG_ECDSA_ECC_NIST_P384;
    base_hash = SPDM_ALGORITHMS_BASE_HASH_ALGO_TPM_ALG_SHA_384;
    dhe       = SPDM_ALGORITHMS_DHE_NAMED_GROUP_SECP_384_R1;
    aead      = SPDM_ALGORITHMS_AEAD_CIPHER_SUITE_AES_256_GCM;
    req_asym  = SPDM_ALGORITHMS_BASE_ASYM_ALGO_TPM_ALG_RSASSA_2048;
    meas_spec = SPDM_MEASUREMENT_SPECIFICATION_DMTF;
    meas_hash = SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_256 |
                SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_384 |
                SPDM_ALGORITHMS_MEASUREMENT_HASH_ALGO_TPM_ALG_SHA_512;

    set_data(spdm_context, LIBSPDM_DATA_BASE_ASYM_ALGO, LIBSPDM_DATA_LOCATION_LOCAL,
             &base_asym, sizeof(base_asym));
    set_data(spdm_context, LIBSPDM_DATA_BASE_HASH_ALGO, LIBSPDM_DATA_LOCATION_LOCAL,
             &base_hash, sizeof(base_hash));
    set_data(spdm_context, LIBSPDM_DATA_DHE_NAME_GROUP, LIBSPDM_DATA_LOCATION_LOCAL,
             &dhe, sizeof(dhe));
    set_data(spdm_context, LIBSPDM_DATA_AEAD_CIPHER_SUITE, LIBSPDM_DATA_LOCATION_LOCAL,
             &aead, sizeof(aead));
    set_data(spdm_context, LIBSPDM_DATA_REQ_BASE_ASYM_ALG, LIBSPDM_DATA_LOCATION_LOCAL,
             &req_asym, sizeof(req_asym));
    set_data(spdm_context, LIBSPDM_DATA_MEASUREMENT_SPEC, LIBSPDM_DATA_LOCATION_LOCAL,
             &meas_spec, sizeof(meas_spec));
    set_data(spdm_context, LIBSPDM_DATA_MEASUREMENT_HASH_ALGO, LIBSPDM_DATA_LOCATION_LOCAL,
             &meas_hash, sizeof(meas_hash));
    key_schedule = SPDM_ALGORITHMS_KEY_SCHEDULE_SPDM;
    set_data(spdm_context, LIBSPDM_DATA_KEY_SCHEDULE, LIBSPDM_DATA_LOCATION_LOCAL,
             &key_schedule, sizeof(key_schedule));
    other_params = 0x01; /* OpaqueDataFmt1 */
    set_data(spdm_context, LIBSPDM_DATA_OTHER_PARAMS_SUPPORT, LIBSPDM_DATA_LOCATION_LOCAL,
             &other_params, sizeof(other_params));

    /* peer root cert for CHALLENGE verification (optional) */
    if (opts->root_cert_path != NULL) {
        cert = read_file(opts->root_cert_path, &cert_size);
        if (cert == NULL) {
            fprintf(stderr, "failed to read root cert: %s\n", opts->root_cert_path);
            free(spdm_context);
            return NULL;
        }
        set_data(spdm_context, LIBSPDM_DATA_PEER_PUBLIC_ROOT_CERT,
                 LIBSPDM_DATA_LOCATION_LOCAL, cert, cert_size);
        free(cert);
    }

    g_sender_buf = malloc(LIBSPDM_SENDER_BUFFER_SIZE);
    g_receiver_buf = malloc(LIBSPDM_RECEIVER_BUFFER_SIZE);
    if (g_sender_buf == NULL || g_receiver_buf == NULL) {
        free(g_sender_buf);
        free(g_receiver_buf);
        free(spdm_context);
        return NULL;
    }

    /* transport-specific init registers device io + transport layer */
    if (opts->transport == SPDM_TOOL_TRANS_TCP) {
        if (tr_tcp_init(spdm_context, opts) != 0) {
            goto fail;
        }
    } else if (opts->transport == SPDM_TOOL_TRANS_MCTP) {
        if (tr_mctp_init(spdm_context, opts) != 0) {
            goto fail;
        }
    } else if (opts->transport == SPDM_TOOL_TRANS_DOE) {
        if (tr_doe_init(spdm_context, opts) != 0) {
            goto fail;
        }
    } else {
        goto fail;
    }

    libspdm_register_device_buffer_func(spdm_context,
                                        LIBSPDM_SENDER_BUFFER_SIZE,
                                        LIBSPDM_RECEIVER_BUFFER_SIZE,
                                        spdm_device_acquire_sender_buffer,
                                        spdm_device_release_sender_buffer,
                                        spdm_device_acquire_receiver_buffer,
                                        spdm_device_release_receiver_buffer);

    scratch_buffer_size = libspdm_get_sizeof_required_scratch_buffer(spdm_context);
    scratch_buffer = malloc(scratch_buffer_size);
    if (scratch_buffer == NULL) {
        goto fail;
    }
    libspdm_set_scratch_buffer(spdm_context, scratch_buffer, scratch_buffer_size);

    return spdm_context;

fail:
    free(g_sender_buf);
    free(g_receiver_buf);
    free(spdm_context);
    return NULL;
}

static int do_connection(void *spdm_context)
{
    libspdm_return_t status;

    status = libspdm_init_connection(spdm_context, false);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "init_connection failed: 0x%x\n", status);
        return -1;
    }
    printf("[SPDM] connection established (GET_VERSION/CAPABILITIES/ALGORITHMS OK)\n");
    return 0;
}

static int do_digest(void *spdm_context, uint8_t *slot_mask)
{
    libspdm_return_t status;
    /* libspdm writes digests for ALL provisioned slots; LIBSPDM_MAX_HASH_SIZE
     * only covers one slot. Allocate generously. */
    uint8_t digest[0x1000];

    status = libspdm_get_digest(spdm_context, NULL, slot_mask, digest);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "get_digest failed: 0x%x\n", status);
        return -1;
    }
    printf("[SPDM] GET_DIGESTS OK, slot_mask=0x%02x\n", *slot_mask);
    return 0;
}

static int do_certificate(void *spdm_context, uint8_t slot_id)
{
    libspdm_return_t status;
    uint8_t *cert_chain;
    size_t cert_chain_size = 0x28000;
    int rc = -1;

    cert_chain = malloc(cert_chain_size);
    if (cert_chain == NULL) {
        fprintf(stderr, "get_certificate: no memory\n");
        return -1;
    }

    status = libspdm_get_certificate(spdm_context, NULL, slot_id,
                                     &cert_chain_size, cert_chain);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "get_certificate failed: 0x%x\n", status);
        goto out;
    }
    printf("[SPDM] GET_CERTIFICATE slot%u OK, chain size=%zu\n", slot_id, cert_chain_size);
    rc = 0;

out:
    free(cert_chain);
    return rc;
}

static int do_challenge(void *spdm_context, uint8_t slot_id)
{
    libspdm_return_t status;
    uint8_t meas_hash[LIBSPDM_MAX_HASH_SIZE];
    uint8_t slot_mask;

    status = libspdm_challenge(spdm_context, NULL, slot_id,
                               SPDM_CHALLENGE_REQUEST_NO_MEASUREMENT_SUMMARY_HASH,
                               meas_hash, &slot_mask);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "challenge failed: 0x%x\n", status);
        return -1;
    }
    printf("[SPDM] CHALLENGE slot%u OK, slot_mask=0x%02x\n", slot_id, slot_mask);
    return 0;
}

static int do_measurement(void *spdm_context, uint8_t slot_id)
{
    libspdm_return_t status;
    uint8_t content_changed;
    uint8_t number_of_blocks;
    uint32_t record_length;
    uint8_t record[0x1000];

    status = libspdm_get_measurement(spdm_context, NULL,
                                     SPDM_GET_MEASUREMENTS_REQUEST_ATTRIBUTES_GENERATE_SIGNATURE,
                                     SPDM_GET_MEASUREMENTS_REQUEST_MEASUREMENT_OPERATION_ALL_MEASUREMENTS,
                                     slot_id, &content_changed, &number_of_blocks,
                                     &record_length, record);
    if (LIBSPDM_STATUS_IS_ERROR(status)) {
        fprintf(stderr, "get_measurement failed: 0x%x\n", status);
        return -1;
    }
    printf("[SPDM] GET_MEASUREMENTS OK, blocks=%u record_len=%u content_changed=%u\n",
           number_of_blocks, record_length, content_changed);
    return 0;
}

/* Required by libspdm sample device secret lib (key/cert loading).
 * Integrator-provided in spdm-emu (spdm_emu_common/support.c). */
bool libspdm_read_input_file(const char *file_name, void **file_data,
                             size_t *file_size)
{
    FILE *fp;
    size_t len;

    fp = fopen(file_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "read_input_file: cannot open %s\n", file_name);
        *file_data = NULL;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    len = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *file_data = malloc(len);
    if (*file_data == NULL) {
        fclose(fp);
        return false;
    }
    if (fread(*file_data, 1, len, fp) != len) {
        free(*file_data);
        *file_data = NULL;
        fclose(fp);
        return false;
    }
    fclose(fp);
    *file_size = len;
    return true;
}

int spdm_tool_main(const spdm_tool_opts_t *opts)
{
    void *spdm_context;
    int rc = 0;
    uint8_t slot_mask = 0;

    spdm_context = spdm_client_init(opts);
    if (spdm_context == NULL) {
        fprintf(stderr, "spdm client init failed\n");
        return -1;
    }

    if (opts->transport == SPDM_TOOL_TRANS_DOE) {
        bool spdm_ok = false, secured_ok = false;
        /* PCIe-layer DOE discovery after transport (UDP socket) is up */
        printf("[DOE] discovery...\n");
        if (tr_doe_discovery(&spdm_ok, &secured_ok) != 0 ||
            !spdm_ok) {
            fprintf(stderr, "[DOE] discovery failed or SPDM type (0x01) not advertised\n");
            rc = -1;
            goto out;
        }
        printf("[DOE] discovery OK: spdm=%d secured_spdm=%d\n", spdm_ok, secured_ok);
    }

    if (do_connection(spdm_context) != 0) {
        rc = -1;
        goto out;
    }
    if (opts->do_digest && do_digest(spdm_context, &slot_mask) != 0) {
        rc = -1;
        goto out;
    }
    if (opts->do_cert && do_certificate(spdm_context, opts->slot_id) != 0) {
        rc = -1;
        goto out;
    }
    if (opts->do_chal && do_challenge(spdm_context, opts->slot_id) != 0) {
        rc = -1;
        goto out;
    }
    if (opts->do_meas && do_measurement(spdm_context, opts->slot_id) != 0) {
        rc = -1;
        goto out;
    }

out:
    free(spdm_context);
    free(g_sender_buf);
    free(g_receiver_buf);
    g_sender_buf = NULL;
    g_receiver_buf = NULL;
    return rc;
}

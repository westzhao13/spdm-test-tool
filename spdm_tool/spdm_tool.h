/* SPDM test tool - common header
 * Host-side SPDM requester test tool based on libspdm.
 * Transports: TCP (smoke), MCTP (AF_MCTP), DOE (UDP -> receiver -> /dev/doe0)
 */
#ifndef SPDM_TOOL_H
#define SPDM_TOOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SPDM_TOOL_TRANS_TCP  1
#define SPDM_TOOL_TRANS_MCTP 2
#define SPDM_TOOL_TRANS_DOE  3

/* --cmd single-step modes (0 = full flow) */
#define SPDM_TOOL_CMD_NONE         0
#define SPDM_TOOL_CMD_VERSION      1
#define SPDM_TOOL_CMD_CAPABILITIES 2
#define SPDM_TOOL_CMD_ALGORITHMS   3
#define SPDM_TOOL_CMD_DIGEST       4
#define SPDM_TOOL_CMD_CERT         5
#define SPDM_TOOL_CMD_CHAL         6
#define SPDM_TOOL_CMD_MEAS         7

/* Buffer sizing, same policy as spdm-emu spdm_emu.h.
 * LIBSPDM_TCP_TRANSPORT_*_SIZE are not exported by libspdm 4.0.0
 * (spdm-emu submodule carries them); values match spdm-emu. */
#define LIBSPDM_TRANSPORT_ADDITIONAL_SIZE 64
#define LIBSPDM_SENDER_BUFFER_SIZE   (0x1100 + LIBSPDM_TRANSPORT_ADDITIONAL_SIZE)
#define LIBSPDM_RECEIVER_BUFFER_SIZE (0x1200 + LIBSPDM_TRANSPORT_ADDITIONAL_SIZE)
#ifndef LIBSPDM_TCP_TRANSPORT_HEADER_SIZE
#define LIBSPDM_TCP_TRANSPORT_HEADER_SIZE 12
#endif
#ifndef LIBSPDM_TCP_TRANSPORT_TAIL_SIZE
#define LIBSPDM_TCP_TRANSPORT_TAIL_SIZE 50
#endif

typedef struct {
    int         transport;
    uint16_t    tcp_port;         /* TCP listen port (trans=tcp) */
    uint8_t     mctp_eid;         /* remote MCTP EID (trans=mctp) */
    const char *doe_udp;          /* "host:port" for DOE UDP relay (trans=doe) */
    const char *doe_dev;          /* "/dev/doeN" for DOE direct ioctl (trans=doe) */
    bool        doe_cap_security; /* direct mode: security DOE instance (0xd80), not normal (0xd00) */
    bool        doe_cap_set;      /* direct mode: raw --doe-cap-offset given */
    uint32_t    doe_cap_offset;   /* direct mode: raw DOE cap offset override */
    const char *root_cert_path;   /* peer root cert (DER) for CHALLENGE, may be NULL */
    int         cmd;              /* SPDM_TOOL_CMD_* single-step mode */
    bool        do_digest;
    bool        do_cert;
    bool        do_chal;
    bool        do_meas;
    uint8_t     slot_id;
} spdm_tool_opts_t;

int spdm_tool_main(const spdm_tool_opts_t *opts);

#endif /* SPDM_TOOL_H */

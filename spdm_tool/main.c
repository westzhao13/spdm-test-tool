/* SPDM test tool - CLI entry
 * Usage: spdm_tool --trans tcp|mctp|doe [--port N] [--eid N]
 *                  [--doe-udp host:port]
 *                  [--cert <root-cert.der>] [--slot N] [--skip digest|cert|chal|meas]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "spdm_tool.h"

static void usage(const char *prog)
{
    printf("Usage: %s --trans tcp|mctp|doe [options]\n"
           "  --trans <tcp|mctp|doe>   transport to use\n"
           "  --port <n>               TCP listen port (default 4194, trans=tcp)\n"
           "  --eid <n>                remote MCTP EID (default 8, trans=mctp)\n"
           "  --doe-udp <host:port>    DOE receiver UDP endpoint (trans=doe)\n"
           "  --cert <file.der>        peer root cert for CHALLENGE verification\n"
           "  --slot <n>               responder slot id (default 0)\n"
           "  --skip <a,b,c,d>         skip digest/cert/chal/meas steps\n"
           "  --cmd <step>             run one step only: version|capabilities|algorithms|digest|cert|chal|meas\n"
           "Example:\n"
           "  %s --trans tcp                          # smoke vs spdm_responder_emu\n"
           "  %s --trans mctp --eid 8\n"
           "  %s --trans doe --doe-udp 127.0.0.1:2324\n"
           "  %s --trans mctp --eid 8 --cmd version   # discovery step only\n",
           prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    spdm_tool_opts_t opts;
    static const struct option long_opts[] = {
        {"trans",    required_argument, NULL, 't'},
        {"port",     required_argument, NULL, 'p'},
        {"eid",      required_argument, NULL, 'e'},
        {"doe-udp",  required_argument, NULL, 'u'},
        {"cert",     required_argument, NULL, 'r'},
        {"slot",     required_argument, NULL, 's'},
        {"skip",     required_argument, NULL, 'k'},
        {"cmd",      required_argument, NULL, 'C'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;

    memset(&opts, 0, sizeof(opts));
    opts.transport = 0;
    opts.slot_id = 0;
    opts.cmd = SPDM_TOOL_CMD_NONE;
    opts.do_digest = true;
    opts.do_cert = true;
    opts.do_chal = true;
    opts.do_meas = true;

    while ((opt = getopt_long(argc, argv, "t:p:e:u:c:r:s:k:C:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't':
            if (strcmp(optarg, "tcp") == 0)
                opts.transport = SPDM_TOOL_TRANS_TCP;
            else if (strcmp(optarg, "mctp") == 0)
                opts.transport = SPDM_TOOL_TRANS_MCTP;
            else if (strcmp(optarg, "doe") == 0)
                opts.transport = SPDM_TOOL_TRANS_DOE;
            else {
                fprintf(stderr, "bad --trans: %s\n", optarg);
                return 1;
            }
            break;
        case 'p':
            opts.tcp_port = (uint16_t)atoi(optarg);
            break;
        case 'e':
            opts.mctp_eid = (uint8_t)atoi(optarg);
            break;
        case 'u':
            opts.doe_udp = optarg;
            break;
        case 'r':
            opts.root_cert_path = optarg;
            break;
        case 's':
            opts.slot_id = (uint8_t)atoi(optarg);
            break;
        case 'k': {
            char *tok = strtok(optarg, ",");
            while (tok != NULL) {
                if (strcmp(tok, "digest") == 0)
                    opts.do_digest = false;
                else if (strcmp(tok, "cert") == 0)
                    opts.do_cert = false;
                else if (strcmp(tok, "chal") == 0)
                    opts.do_chal = false;
                else if (strcmp(tok, "meas") == 0)
                    opts.do_meas = false;
                tok = strtok(NULL, ",");
            }
            break;
        }
        case 'C': {
            static const struct { const char *name; int cmd; } cmds[] = {
                {"version",      SPDM_TOOL_CMD_VERSION},
                {"capabilities", SPDM_TOOL_CMD_CAPABILITIES},
                {"algorithms",   SPDM_TOOL_CMD_ALGORITHMS},
                {"digest",       SPDM_TOOL_CMD_DIGEST},
                {"cert",         SPDM_TOOL_CMD_CERT},
                {"chal",         SPDM_TOOL_CMD_CHAL},
                {"meas",         SPDM_TOOL_CMD_MEAS},
            };
            size_t i;
            for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
                if (strcmp(optarg, cmds[i].name) == 0) {
                    opts.cmd = cmds[i].cmd;
                    break;
                }
            }
            if (i == sizeof(cmds) / sizeof(cmds[0])) {
                fprintf(stderr, "bad --cmd: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (opts.transport == 0) {
        usage(argv[0]);
        return 1;
    }
    if (opts.transport == SPDM_TOOL_TRANS_DOE && opts.doe_udp == NULL) {
        fprintf(stderr, "--doe-udp required for trans=doe\n");
        return 1;
    }

    return spdm_tool_main(&opts);
}

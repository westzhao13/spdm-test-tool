/* SPDM test tool - integrator-supplied file I/O hooks.
 *
 * libspdm's device_secret_lib_sample calls these three symbols and expects the
 * integrator to provide them (spdm-emu ships equivalents in
 * spdm_emu_common/support.c). Both binaries in this directory link them, so they
 * live here rather than being duplicated in spdm_client.c / spdm_responder_udp.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

bool libspdm_read_input_file(const char *file_name, void **file_data,
                             size_t *file_size)
{
    FILE *fp;
    long len;
    uint8_t *buf;

    fp = fopen(file_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "read_input_file: cannot open %s\n", file_name);
        *file_data = NULL;
        return false;
    }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) {
        fclose(fp);
        *file_data = NULL;
        return false;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        fclose(fp);
        *file_data = NULL;
        return false;
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        *file_data = NULL;
        return false;
    }
    fclose(fp);
    *file_data = buf;
    *file_size = (size_t)len;
    return true;
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

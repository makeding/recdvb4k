/*
 * C wrapper for ACAS/MMT-TLV passthrough code derived from dantto4k.
 *
 * Original project: https://github.com/nekohkr/dantto4k.git
 * Local source revision: 141fd49cf674c9c9e3f3c770a20b3291ff292969
 * See acas_passthrough.cpp for the original dantto4k source file list.
 */
#ifndef _ACAS_PASSTHROUGH_H_
#define _ACAS_PASSTHROUGH_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct acas_passthrough acas_passthrough_t;

typedef int (*acas_output_callback)(void *opaque, const uint8_t *data, size_t size);

acas_passthrough_t *acas_passthrough_create(const char *reader_name);
void acas_passthrough_destroy(acas_passthrough_t *ctx);
int acas_passthrough_process(acas_passthrough_t *ctx,
                             const uint8_t *data,
                             size_t size,
                             acas_output_callback output,
                             void *opaque);
int acas_passthrough_flush(acas_passthrough_t *ctx,
                           acas_output_callback output,
                           void *opaque);

#ifdef __cplusplus
}
#endif

#endif

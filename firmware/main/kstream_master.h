#ifndef KSTREAM_MASTER_H
#define KSTREAM_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

typedef struct kstream_master_buffers {
    StreamBufferHandle_t downlink;
    StreamBufferHandle_t uplink;
    StreamBufferHandle_t console_rx;
    StreamBufferHandle_t console_tx;
} kstream_master_buffers_t;

void kstream_master_start(kstream_master_buffers_t *buffers);

#endif

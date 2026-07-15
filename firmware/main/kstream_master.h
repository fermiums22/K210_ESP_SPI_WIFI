#ifndef KSTREAM_MASTER_H
#define KSTREAM_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#define KSTREAM_TASK_PRIORITY 8u

typedef struct kstream_master_buffers {
    StreamBufferHandle_t downlink;
    StreamBufferHandle_t uplink;
    StreamBufferHandle_t console_rx;
    StreamBufferHandle_t console_tx;
    StreamBufferHandle_t update_rx;
    StreamBufferHandle_t update_tx;
} kstream_master_buffers_t;

void kstream_master_start(kstream_master_buffers_t *buffers);
void kstream_master_quiesce(void);
size_t kstream_master_diag(char *buffer, size_t size);

#endif

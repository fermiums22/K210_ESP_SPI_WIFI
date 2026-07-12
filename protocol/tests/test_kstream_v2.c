#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "knet_v2.h"
#include "kstream_v2.h"

int main(void)
{
    kstream_v2_command_t command;
    memset(&command, 0, sizeof(command));
    command.magic = KSTREAM_V2_MAGIC_CMD;
    command.version = KSTREAM_V2_VERSION;
    command.opcode = KSTREAM_V2_OP_PUSH;
    command.stream = KSTREAM_V2_STREAM_DOWNLINK;
    command.sequence = 42u;
    command.length = 4093u;
    command.arg0 = 4096u;
    kstream_v2_command_finalize(&command);
    assert(kstream_v2_command_valid(&command));
    command.inline_data[0] ^= 1u;
    assert(!kstream_v2_command_valid(&command));

    kstream_v2_response_t response;
    memset(&response, 0, sizeof(response));
    response.magic = KSTREAM_V2_MAGIC_RESPONSE;
    response.version = KSTREAM_V2_VERSION;
    response.sequence = 42u;
    response.downlink_free = 65532u;
    response.uplink_used = 4096u;
    kstream_v2_response_finalize(&response);
    assert(kstream_v2_response_valid(&response));

    knet_v2_header_t header;
    memset(&header, 0, sizeof(header));
    header.channel = KNET_V2_CH_VIDEO;
    header.sequence = 7u;
    header.payload_length = 32768u;
    header.payload_crc32 = 0x12345678u;
    knet_v2_header_finalize(&header);
    assert(knet_v2_header_valid(&header));
    header.payload_length = KNET_V2_MAX_PAYLOAD + 1u;
    assert(!knet_v2_header_valid(&header));

    puts("KSTREAM_V2_TESTS_PASS");
    return 0;
}

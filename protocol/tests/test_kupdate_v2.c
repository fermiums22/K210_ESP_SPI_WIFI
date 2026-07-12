#include "kupdate_v2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    kupdate_v2_open_t open;
    memset(&open, 0, sizeof(open));
    open.image_size = 123456u;
    for (unsigned i = 0; i < sizeof(open.image_sha256); ++i)
        open.image_sha256[i] = (uint8_t)i;
    kupdate_v2_open_finalize(&open);
    assert(kupdate_v2_open_valid(&open, 5u * 1024u * 1024u));
    open.image_sha256[7] ^= 1u;
    assert(!kupdate_v2_open_valid(&open, 5u * 1024u * 1024u));

    kupdate_v2_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = KUPDATE_V2_STATE_READY;
    status.target_slot = 1;
    kupdate_v2_status_finalize(&status);
    assert(kupdate_v2_status_valid(&status));
    status.offset++;
    assert(!kupdate_v2_status_valid(&status));
    puts("KUPDATE_V2_TESTS_PASS");
    return 0;
}

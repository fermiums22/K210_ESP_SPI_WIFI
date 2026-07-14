#include "kspi_v2.h"

#include <stdio.h>

static int check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        return 0;
    }
    printf("PASS %s\n", name);
    return 1;
}

int main(void)
{
    int ok = 1;
    uint32_t descriptor = kspi_v2_descriptor(
        KSPI_V2_OPERATION_EXCHANGE, KSPI_V2_REGION_KLINK,
        KSPI_V2_CELL_BYTES, 0x2au);
    ok &= check(descriptor == 0xa510402au, "descriptor-wire-value");
    ok &= check(kspi_v2_descriptor_valid(descriptor), "descriptor-valid");
    ok &= check(kspi_v2_descriptor_token(descriptor) == 0x2au,
                "descriptor-token");
    ok &= check(!kspi_v2_descriptor_valid(descriptor ^ (1u << 16u)),
                "descriptor-region-rejected");
    ok &= check(!kspi_v2_descriptor_valid(descriptor ^ (1u << 8u)),
                "descriptor-length-rejected");

    uint32_t result = kspi_v2_result(KSPI_V2_ERROR_NONE,
                                     KSPI_V2_PHASE_COMPLETE, 0x2au);
    ok &= check(result == 0x5a00032au, "result-wire-value");
    ok &= check(kspi_v2_result_valid(result, 0x2au), "result-valid");
    ok &= check(!kspi_v2_result_valid(result, 0x2bu),
                "result-token-rejected");
    ok &= check(!kspi_v2_result_valid(
                    kspi_v2_result(KSPI_V2_ERROR_CELL,
                                   KSPI_V2_PHASE_COMPLETE, 0x2au), 0x2au),
                "result-error-rejected");
    uint32_t quiesce = kspi_v2_result(KSPI_V2_ERROR_QUIESCE,
                                      KSPI_V2_PHASE_COMPLETE, 0x2au);
    ok &= check(kspi_v2_result_quiesce(quiesce, 0x2au),
                "result-quiesce-valid");
    ok &= check(!kspi_v2_result_quiesce(quiesce, 0x2bu),
                "result-quiesce-token-rejected");

    uint16_t inline_ok = kspi_v2_cell_result_flags(0x0035u,
                                                    KSPI_V2_ERROR_NONE,
                                                    0xfeu);
    ok &= check(inline_ok == 0xfe35u, "inline-result-wire-value");
    ok &= check(kspi_v2_cell_result(inline_ok) == KSPI_V2_CELL_RESULT_OK,
                "inline-result-ok");
    ok &= check(kspi_v2_cell_token(inline_ok) == 0xfeu,
                "inline-result-token");
    uint16_t inline_fatal = kspi_v2_cell_result_flags(
        0xffffu, KSPI_V2_ERROR_CELL, 0xffu);
    ok &= check(inline_fatal == 0xff7fu, "inline-result-fatal-wire-value");
    ok &= check(kspi_v2_cell_result(inline_fatal) ==
                    KSPI_V2_CELL_RESULT_FATAL,
                "inline-result-fatal");
    uint16_t inline_quiesce = kspi_v2_cell_result_flags(
        0u, KSPI_V2_ERROR_QUIESCE, 0u);
    ok &= check(inline_quiesce == 0x0080u,
                "inline-result-quiesce-wrap-wire-value");
    ok &= check(kspi_v2_cell_result(inline_quiesce) ==
                    KSPI_V2_CELL_RESULT_QUIESCE &&
                    kspi_v2_cell_token(inline_quiesce) == 0u,
                "inline-result-quiesce-wrap");
    return ok ? 0 : 1;
}

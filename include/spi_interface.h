#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SpiNum_SPI = 0,
    SpiNum_HSPI = 1,
} SpiNum;

typedef enum {
    SpiBitOrder_MSBFirst = 0,
    SpiBitOrder_LSBFirst = 1,
} SpiBitOrder;

typedef enum {
    SpiOpMode_Master = 0,
    SpiOpMode_Slave = 1,
} SpiOpMode;

typedef enum {
    SpiSubMode_0 = 0,
    SpiSubMode_1 = 1,
    SpiSubMode_2 = 2,
    SpiSubMode_3 = 3,
} SpiSubMode;

typedef struct {
    SpiBitOrder bitOrder;
    SpiOpMode mode;
    SpiSubMode subMode;
    uint32_t speed;
} SpiAttr;

void SPIInit(SpiNum spiNum, SpiAttr *pAttr);
void SPISlaveSendData(SpiNum spiNum, uint32_t *pInData, uint8_t inLen);
void SPISlaveRecvData(SpiNum spiNum, uint32_t *pOutData, uint8_t outLen);

#ifdef __cplusplus
}
#endif

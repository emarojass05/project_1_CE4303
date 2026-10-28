#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"

static int failureCount = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);    \
            failureCount++;                                            \
        }                                                              \
    } while (0)

static const uint8_t checkInput[] = "123456789";
enum {
    CheckInputLength = sizeof(checkInput) - 1,
    CheckValue = 0x29B1
};

static void TestStandardCheckValue(void)
{
    CHECK(Crc16Compute(checkInput, CheckInputLength) == CheckValue);
}

static void TestEmptyInputKeepsInitialValue(void)
{
    uint8_t unused = 0;

    CHECK(Crc16Compute(&unused, 0) == Crc16Initial);
    CHECK(Crc16Compute(NULL, 0) == Crc16Initial);
    CHECK(Crc16Update(0x1234, NULL, 5) == 0x1234);
}

static void TestIncrementalMatchesSinglePass(void)
{
    for (size_t split = 0; split <= CheckInputLength; split++) {
        uint16_t crc = Crc16Update(Crc16Initial, checkInput, split);

        crc = Crc16Update(crc, checkInput + split, CheckInputLength - split);
        CHECK(crc == CheckValue);
    }
}

static void TestSingleBitFlipChangesCrc(void)
{
    uint8_t data[CheckInputLength];

    for (size_t i = 0; i < CheckInputLength; i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(data, checkInput, CheckInputLength);
            data[i] ^= (uint8_t)(1u << bit);
            CHECK(Crc16Compute(data, CheckInputLength) != CheckValue);
        }
    }
}

static void TestKnownSingleBytes(void)
{
    const uint8_t zero = 0x00;
    const uint8_t all = 0xFF;

    CHECK(Crc16Compute(&zero, 1) == 0xE1F0);
    CHECK(Crc16Compute(&all, 1) == 0xFF00);
}

int main(void)
{
    TestStandardCheckValue();
    TestEmptyInputKeepsInitialValue();
    TestIncrementalMatchesSinglePass();
    TestSingleBitFlipChangesCrc();
    TestKnownSingleBytes();

    if (failureCount == 0) {
        printf("test_crc16: all passed\n");
        return 0;
    }
    printf("test_crc16: %d failure(s)\n", failureCount);
    return 1;
}

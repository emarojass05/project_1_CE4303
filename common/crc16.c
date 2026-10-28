#include "crc16.h"

enum {
    Crc16Polynomial = 0x1021
};

/* Bitwise instead of a 512-byte lookup table: frames are at most a few dozen
   bytes, and the table would cost RAM on the nodes for no measurable gain. */
static uint16_t Crc16UpdateByte(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)((uint16_t)byte << 8);
    for (int bit = 0; bit < 8; bit++) {
        if (crc & 0x8000u) {
            crc = (uint16_t)((crc << 1) ^ Crc16Polynomial);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t Crc16Update(uint16_t crc, const uint8_t *data, size_t length)
{
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < length; i++) {
        crc = Crc16UpdateByte(crc, data[i]);
    }
    return crc;
}

uint16_t Crc16Compute(const uint8_t *data, size_t length)
{
    return Crc16Update(Crc16Initial, data, length);
}

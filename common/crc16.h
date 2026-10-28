#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection,
   no final XOR. The same code runs on the host and on every node, so both
   ends always agree. Check value: "123456789" gives 0x29B1. */

enum {
    Crc16Initial = 0xFFFF
};

/* Folds length bytes into crc; start with Crc16Initial */
uint16_t Crc16Update(uint16_t crc, const uint8_t *data, size_t length);

/* CRC of the whole buffer; data may be NULL only when length is 0 */
uint16_t Crc16Compute(const uint8_t *data, size_t length);

#endif /* CRC16_H */

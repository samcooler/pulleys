#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pulleys {
static constexpr uint32_t FW_SERIAL_BAUD = 921600;
static constexpr size_t FW_CHUNK_BYTES = 4096;
static constexpr size_t FW_LINE_BYTES = ((FW_CHUNK_BYTES + 2) / 3) * 4 + 160;
static constexpr size_t FW_SERIAL_RX_BYTES = 16 * 1024;

// Same IEEE CRC-32 as Python zlib.crc32. Retry a damaged chunk immediately;
// the full image SHA-256 remains the final installation integrity check.
inline uint32_t fw_chunk_crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
}

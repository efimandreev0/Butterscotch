#pragma once

#include <stddef.h>
#include <stdint.h>

size_t CtrAdpcm_byteCount(uint32_t frames);

void CtrAdpcm_encode(const int16_t *samples, uint32_t frames,
                     uint8_t *outAdpcm, int16_t outCoefs[16]);

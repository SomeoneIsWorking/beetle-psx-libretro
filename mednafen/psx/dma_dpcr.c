/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "dma_dpcr.h"

static uint32_t DMAControl;

static int IsDPCR(uint32_t A) {
  return ((A & 0x7F) >> 4) == 7 && (A & 0xC) == 0;
}

void DMA_DPCR_Power(void) {
  DMAControl = DMA_DPCR_RESET;
}

int DMA_DPCR_Read(uint32_t A, uint32_t *V) {
  if (!IsDPCR(A)) {
    return 0;
  }
  *V = DMAControl >> ((A & 3) * 8);
  return 1;
}

int DMA_DPCR_Write(uint32_t A, uint32_t V) {
  if (!IsDPCR(A)) {
    return 0;
  }
  DMAControl = V << ((A & 3) * 8);
  return 1;
}

uint32_t DMA_DPCR_SaveStateValue(void) {
  return DMAControl;
}

void DMA_DPCR_LoadStateValue(uint32_t V) {
  DMAControl = V;
}

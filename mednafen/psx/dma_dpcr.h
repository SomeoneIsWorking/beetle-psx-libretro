#ifndef MDFN_PSX_DMA_DPCR_H
#define MDFN_PSX_DMA_DPCR_H

#include <stdint.h>

#define DMA_DPCR_RESET 0x07654321u

void DMA_DPCR_Power(void);

/* DPCR is an on-die register: sub-word writes shift the complete source value
 * by the addressed byte lane and replace the register rather than merging
 * byte enables. Return nonzero only for one of DPCR's four byte lanes. */
int DMA_DPCR_Read(uint32_t A, uint32_t *V);
int DMA_DPCR_Write(uint32_t A, uint32_t V);

uint32_t DMA_DPCR_SaveStateValue(void);
void DMA_DPCR_LoadStateValue(uint32_t V);

#endif

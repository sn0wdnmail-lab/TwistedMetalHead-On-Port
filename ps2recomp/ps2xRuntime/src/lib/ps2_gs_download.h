#pragma once
// GS -> EE downloads (local->host transfers, e.g. sceGsExecStoreImage). The game moves the data
// with VIF1 turned around (VIF1_STAT.FDR): DMA channel 1 with DIR clear, and 128-bit reads of the
// VIF1 FIFO. The runtime tells the memory system where that data comes from: the GPU renderer's
// readback FIFO, or the software GS.
#include <cstdint>
#include <functional>

void ps2SetGsDownloadSource(std::function<uint32_t(uint8_t *dst, uint32_t bytes)> source);

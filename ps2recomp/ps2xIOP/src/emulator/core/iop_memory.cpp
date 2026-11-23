#include "iop_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        [[nodiscard]] bool isSpu2Register(uint32_t phys)
        {
            return phys >= Spu2::RegisterBase && phys < Spu2::RegisterBase + Spu2::RegisterSize;
        }

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        uint32_t alignDown(uint32_t value, uint32_t alignment)
        {
            return value & ~(alignment - 1u);
        }

        // PS2X_IOP_ALLOC_TRACE=1: print every IOP allocation and free (stderr).
        bool allocTrace()
        {
            static const bool on = []()
            {
                const char *value = std::getenv("PS2X_IOP_ALLOC_TRACE");
                return value && *value && *value != '0';
            }();
            return on;
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        m_spu2.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 1u) == 0u && isSpu2Register(phys))
            return m_spu2.read16(phys - Spu2::RegisterBase);
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 1u) == 0u && isSpu2Register(phys))
        {
            m_spu2.write16(phys - Spu2::RegisterBase, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        if (isSpu2Register(address))
            return static_cast<uint32_t>(m_spu2.read16(address - Spu2::RegisterBase)) |
                   (static_cast<uint32_t>(m_spu2.read16(address + 2u - Spu2::RegisterBase)) << 16);
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        if (isSpu2Register(address))
        {
            m_spu2.write16(address - Spu2::RegisterBase, static_cast<uint16_t>(value));
            m_spu2.write16(address + 2u - Spu2::RegisterBase, static_cast<uint16_t>(value >> 16));
            return;
        }
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        const uint32_t madr = readHardware32(address - 8u) & 0x00FFFFFFu;
        const uint32_t blockControl = readHardware32(address - 4u);
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t bytes = static_cast<uint64_t>(wordsPerBlock) * blockCount * 4u;
        uint64_t delay = 64u;
        const uint32_t phys = physicalAddress(madr);
        if (phys < RamSize && bytes <= RamSize - phys)
        {
            auto *halfwords = reinterpret_cast<uint16_t *>(m_ram.data() + phys);
            if (value & 1u) // from IOP RAM to the SPU2
                delay = m_spu2.dmaToSpu(secondCore ? 1 : 0, halfwords, static_cast<size_t>(bytes / 2u));
            else
            {
                delay = m_spu2.dmaFromSpu(secondCore ? 1 : 0, halfwords, static_cast<size_t>(bytes / 2u));
                markOwned(phys, static_cast<size_t>(bytes));
            }
        }
        m_hardware[address - 8u] = madr + static_cast<uint32_t>(bytes);
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(delay, 64u),
        };
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed, Placement placement)
    {
        alignment = std::max<uint32_t>(alignment, 16u);
        if ((alignment & (alignment - 1u)) != 0u || size > HeapLimit - HeapBase)
            return 0u;
        size = alignUp(std::max(size, 1u), 16u);

        const auto insert = [&](size_t index, uint32_t address)
        {
            m_allocations.insert(m_allocations.begin() + static_cast<std::ptrdiff_t>(index), Allocation{address, size});
            markOwned(address, size);
            if (allocTrace())
                std::fprintf(stderr, "[iopmem] alloc %u%s -> 0x%x (largest free %u, total free %u)\n", size,
                             fixed ? " fixed" : placement == Placement::High ? " high" : "", address, maxFreeMemory(), totalFreeMemory());
            return address;
        };
        const auto firstAtOrAfter = [&](uint32_t address)
        {
            return static_cast<size_t>(std::lower_bound(m_allocations.begin(), m_allocations.end(), address,
                                                        [](const Allocation &block, uint32_t value)
                                                        { return block.address < value; }) -
                                       m_allocations.begin());
        };

        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address > HeapLimit || size > HeapLimit - address)
                return 0u;
            const size_t index = firstAtOrAfter(address);
            if (index > 0u && m_allocations[index - 1u].address + m_allocations[index - 1u].size > address)
                return 0u;
            if (index < m_allocations.size() && m_allocations[index].address < address + size)
                return 0u;
            return insert(index, address);
        }

        if (placement == Placement::High)
        {
            // Walk the gaps from the top down: [end of block i-1, start of block i).
            uint32_t gapEnd = HeapLimit;
            for (size_t i = m_allocations.size() + 1u; i-- > 0u;)
            {
                const uint32_t gapStart = i == 0u ? HeapBase : m_allocations[i - 1u].address + m_allocations[i - 1u].size;
                if (gapEnd >= gapStart + size)
                {
                    const uint32_t address = alignDown(gapEnd - size, alignment);
                    if (address >= gapStart)
                        return insert(i, address);
                }
                if (i > 0u)
                    gapEnd = m_allocations[i - 1u].address;
            }
            return 0u;
        }

        uint32_t candidate = alignUp(HeapBase, alignment);
        for (size_t i = 0; i <= m_allocations.size(); ++i)
        {
            const uint32_t gapEnd = i < m_allocations.size() ? m_allocations[i].address : HeapLimit;
            if (candidate <= gapEnd && size <= gapEnd - candidate)
                return insert(i, candidate);
            if (i < m_allocations.size())
                candidate = std::max(candidate, alignUp(m_allocations[i].address + m_allocations[i].size, alignment));
        }
        return 0u;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::lower_bound(m_allocations.begin(), m_allocations.end(), address,
                                            [](const Allocation &candidate, uint32_t value)
                                            { return candidate.address < value; });
        if (allocTrace())
            std::fprintf(stderr, "[iopmem] free 0x%x%s\n", address,
                         block == m_allocations.end() || block->address != address ? " (not allocated)" : "");
        if (block == m_allocations.end() || block->address != address)
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        uint32_t largest = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : m_allocations)
        {
            if (block.address > cursor)
                largest = std::max(largest, block.address - cursor);
            cursor = std::max(cursor, block.address + block.size);
        }
        if (HeapLimit > cursor)
            largest = std::max(largest, HeapLimit - cursor);
        return alignDown(largest, SysmemUnit);
    }

    uint32_t IopMemory::totalFreeMemory() const
    {
        uint32_t total = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : m_allocations)
        {
            if (block.address > cursor)
                total += block.address - cursor;
            cursor = std::max(cursor, block.address + block.size);
        }
        if (HeapLimit > cursor)
            total += HeapLimit - cursor;
        return alignDown(total, SysmemUnit);
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        auto block = std::upper_bound(m_allocations.begin(), m_allocations.end(), address,
                                      [](uint32_t value, const Allocation &candidate)
                                      { return value < candidate.address; });
        if (block == m_allocations.begin())
            return std::nullopt;
        --block;
        if (address >= block->address + block->size)
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}

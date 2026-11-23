#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <optional>
#include <string>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory(mode, size, address): 0 low, 1 high, 2 at the given address
        {
            const uint32_t unit = IopMemory::SysmemUnit;
            const uint32_t size = a1 > 0xFFFFFFFFu - unit ? 0u : (a1 + unit - 1u) & ~(unit - 1u);
            uint32_t address = 0u;
            if (size != 0u)
            {
                if (a0 == 2u)
                    address = m_memory.allocate(size, unit, IopMemory::physicalAddress(a2) & ~(unit - 1u));
                else
                    address = m_memory.allocate(size, unit, std::nullopt,
                                                a0 == 1u ? IopMemory::Placement::High : IopMemory::Placement::Low);
            }
            if (address == 0u)
            {
                static int reported = 0;
                if (reported < 20)
                {
                    ++reported;
                    m_host.log(LogLevel::Warning, "[IOP] AllocSysMemory(mode " + std::to_string(a0) + ", " + std::to_string(a1) +
                                                      " bytes) failed: largest free block " + std::to_string(m_memory.maxFreeMemory()) +
                                                      ", total free " + std::to_string(m_memory.totalFreeMemory()));
                }
            }
            setV0(address);
            return true;
        }
        case 5: // FreeSysMemory
            setV0(m_memory.freeAllocation(a0) ? 0u : 0xFFFFFFFFu);
            return true;
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize
            setV0(m_memory.maxFreeMemory());
            return true;
        case 8: // QueryTotalFreeMemSize
            setV0(m_memory.totalFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->address);
            else
                setV0(0u);
            return true;
        case 10: // QueryBlockSize
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->size);
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 14: // Kprintf
        {
            const std::string format = m_memory.readString(a0, 512u);
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + format);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}

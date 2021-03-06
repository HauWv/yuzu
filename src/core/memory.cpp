// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/page_table.h"
#include "common/swap.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/physical_memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/memory.h"
#include "video_core/gpu.h"

namespace Memory {

// Implementation class used to keep the specifics of the memory subsystem hidden
// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl {
    explicit Impl(Core::System& system_) : system{system_} {}

    void SetCurrentPageTable(Kernel::Process& process) {
        current_page_table = &process.VMManager().page_table;

        const std::size_t address_space_width = process.VMManager().GetAddressSpaceWidth();

        system.ArmInterface(0).PageTableChanged(*current_page_table, address_space_width);
        system.ArmInterface(1).PageTableChanged(*current_page_table, address_space_width);
        system.ArmInterface(2).PageTableChanged(*current_page_table, address_space_width);
        system.ArmInterface(3).PageTableChanged(*current_page_table, address_space_width);
    }

    void MapMemoryRegion(Common::PageTable& page_table, VAddr base, u64 size,
                         Kernel::PhysicalMemory& memory, VAddr offset) {
        MapMemoryRegion(page_table, base, size, memory.data() + offset);
    }

    void MapMemoryRegion(Common::PageTable& page_table, VAddr base, u64 size, u8* target) {
        ASSERT_MSG((size & PAGE_MASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & PAGE_MASK) == 0, "non-page aligned base: {:016X}", base);
        MapPages(page_table, base / PAGE_SIZE, size / PAGE_SIZE, target, Common::PageType::Memory);
    }

    void MapIoRegion(Common::PageTable& page_table, VAddr base, u64 size,
                     Common::MemoryHookPointer mmio_handler) {
        ASSERT_MSG((size & PAGE_MASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & PAGE_MASK) == 0, "non-page aligned base: {:016X}", base);
        MapPages(page_table, base / PAGE_SIZE, size / PAGE_SIZE, nullptr,
                 Common::PageType::Special);

        const auto interval = boost::icl::discrete_interval<VAddr>::closed(base, base + size - 1);
        const Common::SpecialRegion region{Common::SpecialRegion::Type::IODevice,
                                           std::move(mmio_handler)};
        page_table.special_regions.add(
            std::make_pair(interval, std::set<Common::SpecialRegion>{region}));
    }

    void UnmapRegion(Common::PageTable& page_table, VAddr base, u64 size) {
        ASSERT_MSG((size & PAGE_MASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & PAGE_MASK) == 0, "non-page aligned base: {:016X}", base);
        MapPages(page_table, base / PAGE_SIZE, size / PAGE_SIZE, nullptr,
                 Common::PageType::Unmapped);

        const auto interval = boost::icl::discrete_interval<VAddr>::closed(base, base + size - 1);
        page_table.special_regions.erase(interval);
    }

    void AddDebugHook(Common::PageTable& page_table, VAddr base, u64 size,
                      Common::MemoryHookPointer hook) {
        const auto interval = boost::icl::discrete_interval<VAddr>::closed(base, base + size - 1);
        const Common::SpecialRegion region{Common::SpecialRegion::Type::DebugHook, std::move(hook)};
        page_table.special_regions.add(
            std::make_pair(interval, std::set<Common::SpecialRegion>{region}));
    }

    void RemoveDebugHook(Common::PageTable& page_table, VAddr base, u64 size,
                         Common::MemoryHookPointer hook) {
        const auto interval = boost::icl::discrete_interval<VAddr>::closed(base, base + size - 1);
        const Common::SpecialRegion region{Common::SpecialRegion::Type::DebugHook, std::move(hook)};
        page_table.special_regions.subtract(
            std::make_pair(interval, std::set<Common::SpecialRegion>{region}));
    }

    bool IsValidVirtualAddress(const Kernel::Process& process, const VAddr vaddr) const {
        const auto& page_table = process.VMManager().page_table;

        const u8* const page_pointer = page_table.pointers[vaddr >> PAGE_BITS];
        if (page_pointer != nullptr) {
            return true;
        }

        if (page_table.attributes[vaddr >> PAGE_BITS] == Common::PageType::RasterizerCachedMemory) {
            return true;
        }

        if (page_table.attributes[vaddr >> PAGE_BITS] != Common::PageType::Special) {
            return false;
        }

        return false;
    }

    bool IsValidVirtualAddress(VAddr vaddr) const {
        return IsValidVirtualAddress(*system.CurrentProcess(), vaddr);
    }

    /**
     * Gets a pointer to the exact memory at the virtual address (i.e. not page aligned)
     * using a VMA from the current process
     */
    u8* GetPointerFromVMA(const Kernel::Process& process, VAddr vaddr) {
        const auto& vm_manager = process.VMManager();

        const auto it = vm_manager.FindVMA(vaddr);
        DEBUG_ASSERT(vm_manager.IsValidHandle(it));

        u8* direct_pointer = nullptr;
        const auto& vma = it->second;
        switch (vma.type) {
        case Kernel::VMAType::AllocatedMemoryBlock:
            direct_pointer = vma.backing_block->data() + vma.offset;
            break;
        case Kernel::VMAType::BackingMemory:
            direct_pointer = vma.backing_memory;
            break;
        case Kernel::VMAType::Free:
            return nullptr;
        default:
            UNREACHABLE();
        }

        return direct_pointer + (vaddr - vma.base);
    }

    /**
     * Gets a pointer to the exact memory at the virtual address (i.e. not page aligned)
     * using a VMA from the current process.
     */
    u8* GetPointerFromVMA(VAddr vaddr) {
        return GetPointerFromVMA(*system.CurrentProcess(), vaddr);
    }

    u8* GetPointer(const VAddr vaddr) {
        u8* const page_pointer = current_page_table->pointers[vaddr >> PAGE_BITS];
        if (page_pointer != nullptr) {
            return page_pointer + vaddr;
        }

        if (current_page_table->attributes[vaddr >> PAGE_BITS] ==
            Common::PageType::RasterizerCachedMemory) {
            return GetPointerFromVMA(vaddr);
        }

        LOG_ERROR(HW_Memory, "Unknown GetPointer @ 0x{:016X}", vaddr);
        return nullptr;
    }

    u8 Read8(const VAddr addr) {
        return Read<u8>(addr);
    }

    u16 Read16(const VAddr addr) {
        return Read<u16_le>(addr);
    }

    u32 Read32(const VAddr addr) {
        return Read<u32_le>(addr);
    }

    u64 Read64(const VAddr addr) {
        return Read<u64_le>(addr);
    }

    void Write8(const VAddr addr, const u8 data) {
        Write<u8>(addr, data);
    }

    void Write16(const VAddr addr, const u16 data) {
        Write<u16_le>(addr, data);
    }

    void Write32(const VAddr addr, const u32 data) {
        Write<u32_le>(addr, data);
    }

    void Write64(const VAddr addr, const u64 data) {
        Write<u64_le>(addr, data);
    }

    std::string ReadCString(VAddr vaddr, std::size_t max_length) {
        std::string string;
        string.reserve(max_length);
        for (std::size_t i = 0; i < max_length; ++i) {
            const char c = Read8(vaddr);
            if (c == '\0') {
                break;
            }
            string.push_back(c);
            ++vaddr;
        }
        string.shrink_to_fit();
        return string;
    }

    void ReadBlock(const Kernel::Process& process, const VAddr src_addr, void* dest_buffer,
                   const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;

        std::size_t remaining_size = size;
        std::size_t page_index = src_addr >> PAGE_BITS;
        std::size_t page_offset = src_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped ReadBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, src_addr, size);
                std::memset(dest_buffer, 0, copy_amount);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                const u8* const src_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                std::memcpy(dest_buffer, src_ptr, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                const u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                system.GPU().FlushRegion(current_vaddr, copy_amount);
                std::memcpy(dest_buffer, host_ptr, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    void ReadBlockUnsafe(const Kernel::Process& process, const VAddr src_addr, void* dest_buffer,
                         const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;

        std::size_t remaining_size = size;
        std::size_t page_index = src_addr >> PAGE_BITS;
        std::size_t page_offset = src_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped ReadBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, src_addr, size);
                std::memset(dest_buffer, 0, copy_amount);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                const u8* const src_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                std::memcpy(dest_buffer, src_ptr, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                const u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                std::memcpy(dest_buffer, host_ptr, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    void ReadBlock(const VAddr src_addr, void* dest_buffer, const std::size_t size) {
        ReadBlock(*system.CurrentProcess(), src_addr, dest_buffer, size);
    }

    void ReadBlockUnsafe(const VAddr src_addr, void* dest_buffer, const std::size_t size) {
        ReadBlockUnsafe(*system.CurrentProcess(), src_addr, dest_buffer, size);
    }

    void WriteBlock(const Kernel::Process& process, const VAddr dest_addr, const void* src_buffer,
                    const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = dest_addr >> PAGE_BITS;
        std::size_t page_offset = dest_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped WriteBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, dest_addr, size);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                u8* const dest_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                std::memcpy(dest_ptr, src_buffer, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                system.GPU().InvalidateRegion(current_vaddr, copy_amount);
                std::memcpy(host_ptr, src_buffer, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    void WriteBlockUnsafe(const Kernel::Process& process, const VAddr dest_addr,
                          const void* src_buffer, const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = dest_addr >> PAGE_BITS;
        std::size_t page_offset = dest_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped WriteBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, dest_addr, size);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                u8* const dest_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                std::memcpy(dest_ptr, src_buffer, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                std::memcpy(host_ptr, src_buffer, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    void WriteBlock(const VAddr dest_addr, const void* src_buffer, const std::size_t size) {
        WriteBlock(*system.CurrentProcess(), dest_addr, src_buffer, size);
    }

    void WriteBlockUnsafe(const VAddr dest_addr, const void* src_buffer, const std::size_t size) {
        WriteBlockUnsafe(*system.CurrentProcess(), dest_addr, src_buffer, size);
    }

    void ZeroBlock(const Kernel::Process& process, const VAddr dest_addr, const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = dest_addr >> PAGE_BITS;
        std::size_t page_offset = dest_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped ZeroBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, dest_addr, size);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                u8* dest_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                std::memset(dest_ptr, 0, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                system.GPU().InvalidateRegion(current_vaddr, copy_amount);
                std::memset(host_ptr, 0, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
    }

    void ZeroBlock(const VAddr dest_addr, const std::size_t size) {
        ZeroBlock(*system.CurrentProcess(), dest_addr, size);
    }

    void CopyBlock(const Kernel::Process& process, VAddr dest_addr, VAddr src_addr,
                   const std::size_t size) {
        const auto& page_table = process.VMManager().page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = src_addr >> PAGE_BITS;
        std::size_t page_offset = src_addr & PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(PAGE_SIZE) - page_offset, remaining_size);
            const auto current_vaddr = static_cast<VAddr>((page_index << PAGE_BITS) + page_offset);

            switch (page_table.attributes[page_index]) {
            case Common::PageType::Unmapped: {
                LOG_ERROR(HW_Memory,
                          "Unmapped CopyBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr, src_addr, size);
                ZeroBlock(process, dest_addr, copy_amount);
                break;
            }
            case Common::PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);
                const u8* src_ptr =
                    page_table.pointers[page_index] + page_offset + (page_index << PAGE_BITS);
                WriteBlock(process, dest_addr, src_ptr, copy_amount);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                const u8* const host_ptr = GetPointerFromVMA(process, current_vaddr);
                system.GPU().FlushRegion(current_vaddr, copy_amount);
                WriteBlock(process, dest_addr, host_ptr, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            dest_addr += static_cast<VAddr>(copy_amount);
            src_addr += static_cast<VAddr>(copy_amount);
            remaining_size -= copy_amount;
        }
    }

    void CopyBlock(VAddr dest_addr, VAddr src_addr, std::size_t size) {
        return CopyBlock(*system.CurrentProcess(), dest_addr, src_addr, size);
    }

    void RasterizerMarkRegionCached(VAddr vaddr, u64 size, bool cached) {
        if (vaddr == 0) {
            return;
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        u64 num_pages = ((vaddr + size - 1) >> PAGE_BITS) - (vaddr >> PAGE_BITS) + 1;
        for (unsigned i = 0; i < num_pages; ++i, vaddr += PAGE_SIZE) {
            Common::PageType& page_type = current_page_table->attributes[vaddr >> PAGE_BITS];

            if (cached) {
                // Switch page type to cached if now cached
                switch (page_type) {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::Memory:
                    page_type = Common::PageType::RasterizerCachedMemory;
                    current_page_table->pointers[vaddr >> PAGE_BITS] = nullptr;
                    break;
                case Common::PageType::RasterizerCachedMemory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already marked as cached.
                    break;
                default:
                    UNREACHABLE();
                }
            } else {
                // Switch page type to uncached if now uncached
                switch (page_type) {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::Memory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already unmarked as cached.
                    break;
                case Common::PageType::RasterizerCachedMemory: {
                    u8* pointer = GetPointerFromVMA(vaddr & ~PAGE_MASK);
                    if (pointer == nullptr) {
                        // It's possible that this function has been called while updating the
                        // pagetable after unmapping a VMA. In that case the underlying VMA will no
                        // longer exist, and we should just leave the pagetable entry blank.
                        page_type = Common::PageType::Unmapped;
                    } else {
                        page_type = Common::PageType::Memory;
                        current_page_table->pointers[vaddr >> PAGE_BITS] =
                            pointer - (vaddr & ~PAGE_MASK);
                    }
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    /**
     * Maps a region of pages as a specific type.
     *
     * @param page_table The page table to use to perform the mapping.
     * @param base       The base address to begin mapping at.
     * @param size       The total size of the range in bytes.
     * @param memory     The memory to map.
     * @param type       The page type to map the memory as.
     */
    void MapPages(Common::PageTable& page_table, VAddr base, u64 size, u8* memory,
                  Common::PageType type) {
        LOG_DEBUG(HW_Memory, "Mapping {} onto {:016X}-{:016X}", fmt::ptr(memory), base * PAGE_SIZE,
                  (base + size) * PAGE_SIZE);

        // During boot, current_page_table might not be set yet, in which case we need not flush
        if (system.IsPoweredOn()) {
            auto& gpu = system.GPU();
            for (u64 i = 0; i < size; i++) {
                const auto page = base + i;
                if (page_table.attributes[page] == Common::PageType::RasterizerCachedMemory) {
                    gpu.FlushAndInvalidateRegion(page << PAGE_BITS, PAGE_SIZE);
                }
            }
        }

        const VAddr end = base + size;
        ASSERT_MSG(end <= page_table.pointers.size(), "out of range mapping at {:016X}",
                   base + page_table.pointers.size());

        std::fill(page_table.attributes.begin() + base, page_table.attributes.begin() + end, type);

        if (memory == nullptr) {
            std::fill(page_table.pointers.begin() + base, page_table.pointers.begin() + end,
                      memory);
        } else {
            while (base != end) {
                page_table.pointers[base] = memory - (base << PAGE_BITS);
                ASSERT_MSG(page_table.pointers[base],
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                memory += PAGE_SIZE;
            }
        }
    }

    /**
     * Reads a particular data type out of memory at the given virtual address.
     *
     * @param vaddr The virtual address to read the data type from.
     *
     * @tparam T The data type to read out of memory. This type *must* be
     *           trivially copyable, otherwise the behavior of this function
     *           is undefined.
     *
     * @returns The instance of T read from the specified virtual address.
     */
    template <typename T>
    T Read(const VAddr vaddr) {
        const u8* const page_pointer = current_page_table->pointers[vaddr >> PAGE_BITS];
        if (page_pointer != nullptr) {
            // NOTE: Avoid adding any extra logic to this fast-path block
            T value;
            std::memcpy(&value, &page_pointer[vaddr], sizeof(T));
            return value;
        }

        const Common::PageType type = current_page_table->attributes[vaddr >> PAGE_BITS];
        switch (type) {
        case Common::PageType::Unmapped:
            LOG_ERROR(HW_Memory, "Unmapped Read{} @ 0x{:08X}", sizeof(T) * 8, vaddr);
            return 0;
        case Common::PageType::Memory:
            ASSERT_MSG(false, "Mapped memory page without a pointer @ {:016X}", vaddr);
            break;
        case Common::PageType::RasterizerCachedMemory: {
            const u8* const host_ptr = GetPointerFromVMA(vaddr);
            system.GPU().FlushRegion(vaddr, sizeof(T));
            T value;
            std::memcpy(&value, host_ptr, sizeof(T));
            return value;
        }
        default:
            UNREACHABLE();
        }
        return {};
    }

    /**
     * Writes a particular data type to memory at the given virtual address.
     *
     * @param vaddr The virtual address to write the data type to.
     *
     * @tparam T The data type to write to memory. This type *must* be
     *           trivially copyable, otherwise the behavior of this function
     *           is undefined.
     *
     * @returns The instance of T write to the specified virtual address.
     */
    template <typename T>
    void Write(const VAddr vaddr, const T data) {
        u8* const page_pointer = current_page_table->pointers[vaddr >> PAGE_BITS];
        if (page_pointer != nullptr) {
            // NOTE: Avoid adding any extra logic to this fast-path block
            std::memcpy(&page_pointer[vaddr], &data, sizeof(T));
            return;
        }

        const Common::PageType type = current_page_table->attributes[vaddr >> PAGE_BITS];
        switch (type) {
        case Common::PageType::Unmapped:
            LOG_ERROR(HW_Memory, "Unmapped Write{} 0x{:08X} @ 0x{:016X}", sizeof(data) * 8,
                      static_cast<u32>(data), vaddr);
            return;
        case Common::PageType::Memory:
            ASSERT_MSG(false, "Mapped memory page without a pointer @ {:016X}", vaddr);
            break;
        case Common::PageType::RasterizerCachedMemory: {
            u8* const host_ptr{GetPointerFromVMA(vaddr)};
            system.GPU().InvalidateRegion(vaddr, sizeof(T));
            std::memcpy(host_ptr, &data, sizeof(T));
            break;
        }
        default:
            UNREACHABLE();
        }
    }

    Common::PageTable* current_page_table = nullptr;
    Core::System& system;
};

Memory::Memory(Core::System& system) : impl{std::make_unique<Impl>(system)} {}
Memory::~Memory() = default;

void Memory::SetCurrentPageTable(Kernel::Process& process) {
    impl->SetCurrentPageTable(process);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, VAddr base, u64 size,
                             Kernel::PhysicalMemory& memory, VAddr offset) {
    impl->MapMemoryRegion(page_table, base, size, memory, offset);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, VAddr base, u64 size, u8* target) {
    impl->MapMemoryRegion(page_table, base, size, target);
}

void Memory::MapIoRegion(Common::PageTable& page_table, VAddr base, u64 size,
                         Common::MemoryHookPointer mmio_handler) {
    impl->MapIoRegion(page_table, base, size, std::move(mmio_handler));
}

void Memory::UnmapRegion(Common::PageTable& page_table, VAddr base, u64 size) {
    impl->UnmapRegion(page_table, base, size);
}

void Memory::AddDebugHook(Common::PageTable& page_table, VAddr base, u64 size,
                          Common::MemoryHookPointer hook) {
    impl->AddDebugHook(page_table, base, size, std::move(hook));
}

void Memory::RemoveDebugHook(Common::PageTable& page_table, VAddr base, u64 size,
                             Common::MemoryHookPointer hook) {
    impl->RemoveDebugHook(page_table, base, size, std::move(hook));
}

bool Memory::IsValidVirtualAddress(const Kernel::Process& process, const VAddr vaddr) const {
    return impl->IsValidVirtualAddress(process, vaddr);
}

bool Memory::IsValidVirtualAddress(const VAddr vaddr) const {
    return impl->IsValidVirtualAddress(vaddr);
}

u8* Memory::GetPointer(VAddr vaddr) {
    return impl->GetPointer(vaddr);
}

const u8* Memory::GetPointer(VAddr vaddr) const {
    return impl->GetPointer(vaddr);
}

u8 Memory::Read8(const VAddr addr) {
    return impl->Read8(addr);
}

u16 Memory::Read16(const VAddr addr) {
    return impl->Read16(addr);
}

u32 Memory::Read32(const VAddr addr) {
    return impl->Read32(addr);
}

u64 Memory::Read64(const VAddr addr) {
    return impl->Read64(addr);
}

void Memory::Write8(VAddr addr, u8 data) {
    impl->Write8(addr, data);
}

void Memory::Write16(VAddr addr, u16 data) {
    impl->Write16(addr, data);
}

void Memory::Write32(VAddr addr, u32 data) {
    impl->Write32(addr, data);
}

void Memory::Write64(VAddr addr, u64 data) {
    impl->Write64(addr, data);
}

std::string Memory::ReadCString(VAddr vaddr, std::size_t max_length) {
    return impl->ReadCString(vaddr, max_length);
}

void Memory::ReadBlock(const Kernel::Process& process, const VAddr src_addr, void* dest_buffer,
                       const std::size_t size) {
    impl->ReadBlock(process, src_addr, dest_buffer, size);
}

void Memory::ReadBlock(const VAddr src_addr, void* dest_buffer, const std::size_t size) {
    impl->ReadBlock(src_addr, dest_buffer, size);
}

void Memory::ReadBlockUnsafe(const Kernel::Process& process, const VAddr src_addr,
                             void* dest_buffer, const std::size_t size) {
    impl->ReadBlockUnsafe(process, src_addr, dest_buffer, size);
}

void Memory::ReadBlockUnsafe(const VAddr src_addr, void* dest_buffer, const std::size_t size) {
    impl->ReadBlockUnsafe(src_addr, dest_buffer, size);
}

void Memory::WriteBlock(const Kernel::Process& process, VAddr dest_addr, const void* src_buffer,
                        std::size_t size) {
    impl->WriteBlock(process, dest_addr, src_buffer, size);
}

void Memory::WriteBlock(const VAddr dest_addr, const void* src_buffer, const std::size_t size) {
    impl->WriteBlock(dest_addr, src_buffer, size);
}

void Memory::WriteBlockUnsafe(const Kernel::Process& process, VAddr dest_addr,
                              const void* src_buffer, std::size_t size) {
    impl->WriteBlockUnsafe(process, dest_addr, src_buffer, size);
}

void Memory::WriteBlockUnsafe(const VAddr dest_addr, const void* src_buffer,
                              const std::size_t size) {
    impl->WriteBlockUnsafe(dest_addr, src_buffer, size);
}

void Memory::ZeroBlock(const Kernel::Process& process, VAddr dest_addr, std::size_t size) {
    impl->ZeroBlock(process, dest_addr, size);
}

void Memory::ZeroBlock(VAddr dest_addr, std::size_t size) {
    impl->ZeroBlock(dest_addr, size);
}

void Memory::CopyBlock(const Kernel::Process& process, VAddr dest_addr, VAddr src_addr,
                       const std::size_t size) {
    impl->CopyBlock(process, dest_addr, src_addr, size);
}

void Memory::CopyBlock(VAddr dest_addr, VAddr src_addr, std::size_t size) {
    impl->CopyBlock(dest_addr, src_addr, size);
}

void Memory::RasterizerMarkRegionCached(VAddr vaddr, u64 size, bool cached) {
    impl->RasterizerMarkRegionCached(vaddr, size, cached);
}

bool IsKernelVirtualAddress(const VAddr vaddr) {
    return KERNEL_REGION_VADDR <= vaddr && vaddr < KERNEL_REGION_END;
}

} // namespace Memory

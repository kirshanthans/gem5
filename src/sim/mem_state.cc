/*
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Michael LeBeane
 *         Brandon Potter
 */

#include "sim/mem_state.hh"

#include <cassert>

#include "mem/se_translating_port_proxy.hh"
#include "mem/vma.hh"
#include "sim/process.hh"
#include "sim/syscall_debug_macros.hh"
#include "sim/system.hh"

MemState::MemState(Process *owner, Addr brk_point, Addr stack_base,
                   Addr max_stack_size, Addr next_thread_stack_base,
                   Addr mmap_end, System *system,
                   std::shared_ptr<EmulationPageTable> p_table,
                   Addr vsyscall_point, Addr vsyscall_size)
    : _pTable(p_table), _ownerProcesses{owner}, _brkPoint(brk_point),
      _stackBase(stack_base), _maxStackSize(max_stack_size),
      _nextThreadStackBase(next_thread_stack_base),
      _mmapEnd(mmap_end), _vsyscallPoint(vsyscall_point),
      _vsyscallSize(vsyscall_size), _endBrkPoint(brk_point),
      _virtMem(system->getSystemPort(), system->cacheLineSize(), this,
               SETranslatingPortProxy::Always)
{
}

MemState&
MemState::operator=(const MemState &other)
{
    if (this == &other)
        return *this;

    _brkPoint = other._brkPoint;
    _stackBase = other._stackBase;
    _maxStackSize = other._maxStackSize;
    _nextThreadStackBase = other._nextThreadStackBase;
    _mmapEnd = other._mmapEnd;
    _vmaList = other._vmaList; /* This assignment does a deep copy. */

    /* Do a page-table deep copy between the two MemState objects. */
    typedef std::vector<std::pair<Addr,Addr>> MapVec;
    MapVec mappings;
    other._pTable->getMappings(&mappings);
    for (auto map : mappings) {
        Addr paddr, vaddr = map.first;
        bool alloc_page = !(_pTable->translate(vaddr, paddr));
        replicatePage(other, vaddr, paddr, alloc_page);
    }

    return *this;
}

void
MemState::addOwner(Process *owner)
{
    auto it = _ownerProcesses.begin();
    while (it != _ownerProcesses.end()) {
        if (*it == owner)
            break;
        it++;
    }

    if (it == _ownerProcesses.end())
        _ownerProcesses.push_back(owner);
}

void
MemState::removeOwner(Process *owner)
{
    auto it = _ownerProcesses.begin();
    while (it != _ownerProcesses.end()) {
        if (*it == owner)
            break;
        it++;
    }

    if (it != _ownerProcesses.end())
        _ownerProcesses.erase(it);
}

void
MemState::updateBrkRegion(Addr old_brk, Addr new_brk)
{
    /**
     * To make this simple, avoid reducing the heap memory area if the
     * new_brk point is less than the old_brk; this occurs when the heap is
     * receding because the application has given back memory. The brk point
     * is still tracked in the MemState class as an independent field so that
     * it can be returned to the application; we just do not update the
     * VMA region unless we expand it out.
     */
    if (new_brk < old_brk)
        return;

    /**
     * The VMAs must be page aligned but the break point can be set on
     * byte boundaries. Ensure that the restriction is maintained here by
     * extending the request out to the end of the page. (The roundUp
     * function will not round up an already aligned page.)
     */
    new_brk = roundUp(new_brk, TheISA::PageBytes);

    /**
     * Create a new mapping for the heap region. We only create a mapping
     * for the extra memory that is requested so we do not create a situation
     * where there can be overlapping mappings in the VMA regions.
     * Since we do not track the type of the region and we also do not
     * coalesce the regions together, we can create a fragmented set of
     * heap VMAs. To resolve this, we keep the furthest point ever mapped
     * by the _endBrkPoint field.
     */
    if (new_brk > _endBrkPoint) {
        /**
         * Note that the heap regions are always contiguous but there is no
         * mechanism right now to coalesce VMAs together that belong to the
         * same region with similar access permissions. This could be
         * implemented if it actually becomes necessary; probably only
         * necessary if the list becomes too long to walk.
         */
        mapVMARegion(_endBrkPoint, new_brk - _endBrkPoint, -1, 0,
                     std::string("heap"));
        _endBrkPoint = new_brk;
    }
}

void
MemState::mapVMARegion(Addr start_addr, Addr length, int sim_fd, Addr offset,
                       std::string vma_name)
{
    DPRINTF(SyscallVerbose, "vma: creating region (%s) [0x%x - 0x%x]\n",
            vma_name.c_str(), start_addr, start_addr + length - 1);

    /**
     * Avoid creating a region that has preexisting mappings. This should
     * not happen under normal circumstances so consider this to be a bug.
     */
    auto end = start_addr + length;
    for (auto start = start_addr; start < end; start += TheISA::PageBytes)
        assert(_pTable->lookup(start) == nullptr);

    /**
     * Record the region in our list structure.
     */
    _vmaList.emplace_back(start_addr, length, sim_fd, offset, vma_name);
}

void
MemState::unmapVMARegion(Addr start_addr, Addr length)
{
    Addr end_addr = start_addr + length - 1;
    const AddrRange range(start_addr, end_addr);

    auto vma = std::begin(_vmaList);
    while (vma != std::end(_vmaList)) {
        if (vma->isStrictSuperset(range)) {
            DPRINTF(SyscallVerbose, "vma: split region [0x%x - 0x%x] into "
                                    "[0x%x - 0x%x] and [0x%x - 0x%x]\n",
                                    vma->start(), vma->end(),
                                    vma->start(), start_addr - 1,
                                    end_addr + 1, vma->end());
            /**
             * Need to split into two smaller regions.
             * Create a clone of the old VMA and slice it to the right.
             */
            _vmaList.push_back(*vma);
            _vmaList.back().sliceRegionRight(start_addr);

            /**
             * Slice old VMA to encapsulate the left region.
             */
            vma->sliceRegionLeft(end_addr);

            /**
             * Region cannot be in any more VMA, because it is completely
             * contained in this one!
             */
            break;
        } else if (vma->isSubset(range)) {
            DPRINTF(SyscallVerbose, "vma: destroying region [0x%x - 0x%x]\n",
                                    vma->start(), vma->end());
            /**
             * Need to nuke the existing VMA.
             */
            vma = _vmaList.erase(vma);

            continue;

        } else if (vma->intersects(range)) {
            /**
             * Trim up the existing VMA.
             */
            if (vma->start() < start_addr) {
                DPRINTF(SyscallVerbose, "vma: resizing region [0x%x - 0x%x] "
                                        "into [0x%x - 0x%x]\n",
                                        vma->start(), vma->end(),
                                        vma->start(), start_addr - 1);
                /**
                 * Overlaps from the right.
                 */
                vma->sliceRegionRight(start_addr);
            } else {
                DPRINTF(SyscallVerbose, "vma: resizing region [0x%x - 0x%x] "
                                        "into [0x%x - 0x%x]\n",
                                        vma->start(), vma->end(),
                                        end_addr + 1, vma->end());
                /**
                 * Overlaps from the left.
                 */
                vma->sliceRegionLeft(end_addr);
            }
        }

        vma++;
    }

    updateTLBs();

    do {
        if (!_pTable->isUnmapped(start_addr, TheISA::PageBytes))
            _pTable->unmap(start_addr, TheISA::PageBytes);

        start_addr += TheISA::PageBytes;

        /**
         * The regions need to always be page-aligned otherwise the while
         * condition will loop indefinitely. (The Addr type is currently
         * defined to be uint64_t in src/base/types.hh; it can underflow
         * since it is unsigned.)
         */
        length -= TheISA::PageBytes;
    } while (length > 0);
}

void
MemState::remapVMARegion(Addr start_addr, Addr new_start_addr,
                         Addr length)
{
    Addr end_addr = start_addr + length - 1;
    const AddrRange range(start_addr, end_addr);

    auto vma = std::begin(_vmaList);
    while (vma != std::end(_vmaList)) {
        if (vma->isStrictSuperset(range)) {
            /**
             * Create clone of the old VMA and slice right.
             */
            _vmaList.push_back(*vma);
            _vmaList.back().sliceRegionRight(start_addr);

            /**
             * Create clone of the old VMA and slice it left.
             */
            _vmaList.push_back(*vma);
            _vmaList.back().sliceRegionLeft(end_addr);

            /**
             * Slice the old VMA left and right to adjust the file backing,
             * then overwrite the virtual addresses!
             */
            vma->sliceRegionLeft(start_addr - 1);
            vma->sliceRegionRight(end_addr + 1);
            vma->remap(new_start_addr);

            /**
             * The region cannot be in any more VMAs, because it is
             * completely contained in this one!
             */
            break;
        } else if (vma->isSubset(range)) {
            /**
             * Just go ahead and remap it!
             */
            vma->remap(vma->start() - start_addr + new_start_addr);
        } else if (vma->intersects(range)) {
            /**
             * Create a clone of the old VMA.
             */
            _vmaList.push_back(*vma);

            if (vma->start() < start_addr) {
                /**
                 * Overlaps from the right.
                 */
                _vmaList.back().sliceRegionRight(start_addr);

                /**
                 * Remap the old region.
                 */
                vma->sliceRegionLeft(start_addr - 1);
                vma->remap(new_start_addr);
            } else {
                /**
                 * Overlaps from the left.
                 */
                _vmaList.back().sliceRegionLeft(end_addr);

                /**
                 * Remap the old region.
                 */
                vma->sliceRegionRight(end_addr + 1);
                vma->remap(new_start_addr + vma->start() - start_addr);
            }
        }

        vma++;
    }

    updateTLBs();

    do {
        if (!_pTable->isUnmapped(start_addr, TheISA::PageBytes))
            _pTable->remap(start_addr, TheISA::PageBytes, new_start_addr);

        start_addr += TheISA::PageBytes;

        /**
         * The regions need to always be page-aligned otherwise the while
         * condition will loop indefinitely. (The Addr type is currently
         * defined to be uint64_t in src/base/types.hh; it can underflow
         * since it is unsigned.)
         */
        length -= TheISA::PageBytes;
    } while (length > 0);
}

bool
MemState::translate(Addr va)
{
    return (_pTable->translate(va) || fixupFault(va));
}

bool
MemState::translate(Addr va, Addr &pa)
{
    return (_pTable->translate(va, pa) || (fixupFault(va) &&
            _pTable->translate(va, pa)));
}

const EmulationPageTable::Entry*
MemState::lookup(Addr va)
{
    auto entry = _pTable->lookup(va);
    if (entry == nullptr) {
        fixupFault(va);
        entry = _pTable->lookup(va);
    }
    return entry;
}

bool
MemState::fixupFault(Addr vaddr)
{
    /**
     * Check if we are accessing a mapped virtual address. If so then we
     * just haven't allocated it a physical page yet and can do so here.
     */
    for (const auto &vma : _vmaList) {
        if (vma.contains(vaddr)) {
            Addr vpage_start = roundDown(vaddr, TheISA::PageBytes);
            allocateMem(vpage_start, TheISA::PageBytes);

            /**
             * We are assuming that fresh pages are zero-filled, so there is
             * no need to zero them out when there is no backing file.
             * This assumption will not hold true if/when physical pages
             * are recycled.
             */
            if (vma.hasHostBuf()) {
                vma.fillMemPages(vpage_start, TheISA::PageBytes, _virtMem);
            }
            return true;
        }
    }
    return false;
}

bool
MemState::map(Addr vaddr, Addr paddr, int size, bool cacheable)
{
    auto flags = cacheable ? EmulationPageTable::MappingFlags(0) :
                             EmulationPageTable::Uncacheable;
    _pTable->map(vaddr, paddr, size, flags);
    return true;
}

void
MemState::allocateMem(Addr vaddr, int64_t size, bool clobber)
{
    int npages = divCeil(size, (int64_t)TheISA::PageBytes);
    Addr paddr = system()->allocPhysPages(npages);
    auto flags = clobber ? EmulationPageTable::Clobber :
                           EmulationPageTable::MappingFlags(0);
    _pTable->map(vaddr, paddr, size, flags);
}

void
MemState::replicatePage(const MemState &in, Addr vaddr, Addr new_paddr,
                        bool allocate_page)
{
    if (allocate_page)
        new_paddr = system()->allocPhysPages(1);

    /**
     * Read from old physical page.
     */
    uint8_t *buf_p = new uint8_t[TheISA::PageBytes];
    in._virtMem.readBlob(vaddr, buf_p, TheISA::PageBytes);

    /**
     * Create new mapping in process address space by clobbering existing
     * mapping (if any existed) and then write to the new physical page.
     */
    bool clobber = true;
    _pTable->map(vaddr, new_paddr, TheISA::PageBytes, clobber);
    _virtMem.writeBlob(vaddr, buf_p, TheISA::PageBytes);
    delete[] buf_p;
}

System *
MemState::system() const
{
    /**
     * Grab the system field from one of our parent processes; we assume
     * that the processes will all belong to the same system object.
     */
    assert(_ownerProcesses.size());
    return _ownerProcesses[0]->system;
}

void
MemState::updateTLBs()
{
    /**
     * FIXME: Flush only the necessary translation look-aside buffer entries.
     *
     * The following code results in functionally correct execution, but real
     * systems do not flush all entries when a single mapping changes since
     * it tanks performance. The problem is that there is no general method
     * across all of the implementations which can flush just part of the
     * address space.
     */
    for (auto tc : system()->threadContexts) {
        tc->getDTBPtr()->flushAll();
        tc->getITBPtr()->flushAll();
    }
}

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
 * Author: Brandon Potter
 */

#ifndef SRC_SIM_MEM_STATE_HH
#define SRC_SIM_MEM_STATE_HH

#include <list>
#include <memory>
#include <string>
#include <vector>
#include <sys/mman.h>

#include "mem/page_table.hh"
#include "mem/se_translating_port_proxy.hh"
#include "mem/vma.hh"
#include "sim/serialize.hh"

class Process;
class ProcessParams;
class System;

/**
 * This class holds the memory state for the Process class and all of its
 * derived, architecture-specific children.
 *
 * The class fields change dynamically as the process object is run in the
 * simulator. They are updated by system calls and faults. Each change
 * represents a modification to the process address space.
 *
 * The class is meant to be allocated dynamically and shared through a
 * pointer interface. Multiple process can potentially share their virtual
 * address space if specific options are passed into the clone(2) system call.
 */
class MemState : public Serializable
{
  public:
    MemState(Process *owner, Addr brk_point, Addr stack_base,
             Addr max_stack_size, Addr next_thread_stack_base,
             Addr mmap_end, System *system,
             std::shared_ptr<EmulationPageTable> p_table,
             Addr vsyscall_point = 0, Addr vsyscall_size = 0);

    MemState& operator=(const MemState &in);

    void addOwner(Process *owner);
    void removeOwner(Process *owner);

    Addr getBrkPoint() const { return _brkPoint; }
    Addr getStackBase() const { return _stackBase; }
    Addr getStackSize() const { return _stackSize; }
    Addr getMaxStackSize() const { return _maxStackSize; }
    Addr getStackMin() const { return _stackMin; }
    Addr getNextThreadStackBase() const { return _nextThreadStackBase; }
    Addr getMmapEnd() const { return _mmapEnd; }
    void setBrkPoint(Addr brk_point) { _brkPoint = brk_point; }
    void setStackBase(Addr stack_base) { _stackBase = stack_base; }
    void setStackSize(Addr stack_size) { _stackSize = stack_size; }
    void setMaxStackSize(Addr max_stack) { _maxStackSize = max_stack; }
    void setStackMin(Addr stack_min) { _stackMin = stack_min; }
    void setNextThreadStackBase(Addr ntsb) { _nextThreadStackBase = ntsb; }
    void setMmapEnd(Addr mmap_end) { _mmapEnd = mmap_end; }

    const SETranslatingPortProxy& getVirtMem() const { return _virtMem; }

    void mapVMARegion(Addr start_addr, Addr length, int sim_fd,
                      Addr offset, std::string vma_name);

    void unmapVMARegion(Addr start_addr, Addr length);

    void remapVMARegion(Addr start_addr, Addr new_start_addr, Addr length);

    void updateBrkRegion(Addr old_brk, Addr new_brk);

    /**
     * This method returns a flag which specifies the direction of growth for
     * the mmap region in process address space.
     *
     * @return true if the mmap region grows downward and false otherwise
     */
    virtual bool mmapGrowsDown() const
#if THE_ISA == ALPHA_ISA || THE_ISA == RISCV_ISA
        { return false; }
#else
        { return true; }
#endif

    /**
     * These methods perform the same behavior as the methods in the
     * PageTable class with the additional semantic that they will attempt to
     * allocate physical pages (frames) to resolve faults.
     * vaddr is valid but the page table does not have an entry yet.
     */
    bool translate(Addr va);
    bool translate(Addr va, Addr &pa);
    const EmulationPageTable::Entry *lookup(Addr va);

    /**
     * Maps a contiguous range of virtual addresses in this process's
     * address space to a contiguous range of physical addresses.
     * This function exists primarily to expose the map operation to
     * python, so that configuration scripts can set up mappings in SE mode.
     *
     * @param vaddr The starting virtual address of the range.
     * @param paddr The starting physical address of the range.
     * @param size The length of the range in bytes.
     * @param cacheable Specifies whether accesses are cacheable.
     * @return True if the map operation was successful. (At this point in
     *         time, the map operation always succeeds.)
     */
    bool map(Addr vaddr, Addr paddr, int size, bool cacheable = true);

    /**
     * Attempt to fix up a fault at vaddr by allocating a page. The fault
     * likely occurred because a virtual page which does not have physical
     * page assignment is being accessed.
     *
     * @param vaddr The virtual address which is causing the fault.
     * @return Whether the fault has been fixed.
     */
    bool fixupFault(Addr vaddr);

    /**
     * Given the vaddr and size, this method will chunk the allocation into
     * page granularity and then request physical pages (frames) from the
     * system object. After retrieving a frame, the method updates the page
     * table mappings.
     *
     * @param vaddr The virtual address in need of a frame allocation.
     * @param size The size in bytes of the requested mapping.
     * @param clobber This flag specifies whether mappings in the page tables
     *        can be overwritten and replaced with the new mapping.
     */
    void allocateMem(Addr vaddr, int64_t size, bool clobber = false);

    /**
     * The _pTable member is either an architectural page table object or a
     * functional page table object. Both implementations share the same API
     * and can be accessed through this base class object.
     */
    std::shared_ptr<EmulationPageTable> _pTable;

    /**
     * This port proxy provides a handle to access the process address space
     * (represented by this object). In SE Mode, all references to the
     * process address space (using virtual addresses) should be directed
     * through this proxy.
     *
     * Creating duplicates or storing copies of this proxy in other
     * objects is frowned upon; please do not do it! Note, the proxy is
     * owned by this object and will become obsolete if this object
     * deconstructs. (This will happen if all processes release their hold
     * on this object; this is particularly an issue when calling the exec
     * system call.)
     */
    const SETranslatingPortProxy& getVirtProxy() { return _virtMem; }

    void
    serialize(CheckpointOut &cp) const override
    {
        paramOut(cp, "brkPoint", _brkPoint);
        paramOut(cp, "stackBase", _stackBase);
        paramOut(cp, "stackSize", _stackSize);
        paramOut(cp, "maxStackSize", _maxStackSize);
        paramOut(cp, "stackMin", _stackMin);
        paramOut(cp, "nextThreadStackBase", _nextThreadStackBase);
        paramOut(cp, "mmapEnd", _mmapEnd);
    }
    void
    unserialize(CheckpointIn &cp) override
    {
        paramIn(cp, "brkPoint", _brkPoint);
        paramIn(cp, "stackBase", _stackBase);
        paramIn(cp, "stackSize", _stackSize);
        paramIn(cp, "maxStackSize", _maxStackSize);
        paramIn(cp, "stackMin", _stackMin);
        paramIn(cp, "nextThreadStackBase", _nextThreadStackBase);
        paramIn(cp, "mmapEnd", _mmapEnd);
    }

    const std::list<VMA> & vmaList() { return _vmaList; }

  private:
    void replicatePage(const MemState &in, Addr vaddr, Addr new_paddr,
                       bool alloc_page);

    void updateTLBs();

    System * system() const;

    void checkAlign(Addr start, Addr length);

    /**
     * Holds the list of Process objects which have a reference to this
     * MemState object. The owner Processes act as parents in an object tree
     * for MemState.
     */
    std::vector<Process*> _ownerProcesses;

    Addr _brkPoint;
    Addr _stackBase;
    Addr _stackSize;
    Addr _maxStackSize;
    Addr _stackMin;
    Addr _nextThreadStackBase;
    Addr _mmapEnd;
    Addr _vsyscallPoint;
    Addr _vsyscallSize;

    /**
     * Keeps record of the furthest mapped heap location for the VMAs.
     */
    Addr _endBrkPoint;

    /**
     * The _vmaList member is a list of virtual memory areas in the target
     * application space that have been allocated by the target. In most
     * operating systems, lazy allocation is used and these structures (or
     * equivalent ones) are used to track the valid address ranges.
     *
     * This could use a more efficient data structure like an interval
     * tree, but it is unclear whether the vmas will be modified often enough
     * for the improvement in lookup time to matter.
     */
    std::list<VMA> _vmaList;

    /**
     * The _virtMem member acts as a proxy for accessing virtual memory. This
     * handle is responsible for reading and writing blobs and strings both to
     * and from the virtual memory space for Process objects.
     */
    SETranslatingPortProxy _virtMem;
};

#endif

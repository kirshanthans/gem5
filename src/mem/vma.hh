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
 */

#ifndef SRC_MEM_VMA_HH
#define SRC_MEM_VMA_HH

#include <string>

#include "base/addr_range.hh"
#include "base/mapped_buf.hh"
#include "base/types.hh"
#include "mem/se_translating_port_proxy.hh"

class VMA : public AddrRange
{
  public:
    VMA(Addr sa, Addr len, int fd, int off, std::string vma_name)
        : AddrRange(sa, sa + len - 1), _origHostBuf(nullptr),
          _hostBuf(nullptr), _hostBufLen(0), _vmaName(vma_name)
    {
        if (fd != -1) {
            _origHostBuf = std::make_shared<MappedFileBuffer>(fd, len, off);
            _hostBuf = _origHostBuf->buf;
            _hostBufLen = _origHostBuf->len;
        }

        CHECK();
    }

    /**
     * Remap the virtual memory area starting at new_start.
     */
    void remap(Addr new_start)
    {
        _end = new_start + size() - 1;
        _start = new_start;

        CHECK();
    }

    /**
     * Check if the virtual memory area has an equivalent buffer on the
     * host machine.
     */
    bool hasHostBuf() const { return _origHostBuf != nullptr; }

    /**
     * Copy memory from a buffer which resides on the host machine into a
     * section of memory on the target.
     */
    void fillMemPages(Addr start, Addr size,
                      SETranslatingPortProxy &port) const;

    /**
     * Returns true if desired range exists within this virtual memory area
     * and does not include the start and end addresses.
     */
    bool isStrictSuperset(const AddrRange &range) const;

    /**
     * Remove the address range to the right of slice_addr.
     */
    void sliceRegionRight(Addr slice_addr);

    /**
     * Remove the address range to the left of slice_addr.
     */
    void sliceRegionLeft(Addr slice_addr);

    Addr getStart() { return _start; }
    Addr getEnd() { return _end; }
    std::string getName() { return _vmaName; }

  private:
    void CHECK();

    /**
     * The host file backing will be chopped up and reassigned as pages are
     * mapped, remapped, and unmapped. In addition to the current host
     * pointer and length, each virtual memory area will also keep a
     * reference-counted handle to the original host memory. The last virtual
     * memory area to die cleans up the host memory it handles.
     */
    std::shared_ptr<MappedFileBuffer> _origHostBuf;

    /**
     * Host buffer ptr for this virtual memory area.
     */
    char *_hostBuf;

    /**
     * Length of host buffer for this virtual memory area.
     */
    Addr _hostBufLen;

    /**
     * Human-readable name associated with the virtual memory area.
     * The name is useful for debugging and also exposing vma state through
     * the psuedo file system (i.e. Linux's /proc/self/maps) to the
     * application.
     */
    std::string _vmaName;
};

#endif

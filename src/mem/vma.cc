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

#include "mem/vma.hh"

void
VMA::fillMemPages(Addr start, Addr size, SETranslatingPortProxy &port) const
{
    auto offset = start - _start;

    /**
     * Try to copy a full page, but don't overrun the size of the file.
     */
    if (offset < _hostBufLen) {
        auto size = std::min(_hostBufLen - offset, TheISA::PageBytes);
        port.writeBlob(start, (uint8_t*) _hostBuf + offset, size);
    }
}

bool
VMA::isStrictSuperset(const AddrRange &r) const
{
    return (r.start() > _start && r.end() < _end);
}

void
VMA::sliceRegionRight(Addr slice_addr)
{
    if (hasHostBuf()) {
        auto nonoverlap_len = slice_addr - _start;
        _hostBufLen = std::min(_hostBufLen, nonoverlap_len);
    }

    _end = slice_addr - 1;

    CHECK();
}

void
VMA::sliceRegionLeft(Addr slice_addr)
{
    if (hasHostBuf()) {
        auto overlap_len = slice_addr - _start + 1;

        if (overlap_len >= _hostBufLen) {
            _hostBufLen = 0;
            _hostBuf = nullptr;
            _origHostBuf = nullptr;
        } else {
            _hostBufLen -= overlap_len;
        }

        _hostBuf += overlap_len;
    }

    _start = slice_addr + 1;

    CHECK();
}

void
VMA::CHECK()
{
    /**
     * Avoid regions without a length.
     */
    assert(_start != _end);

    /**
     * Avoid regions with an end point before the start point
     */
    assert(_start < _end);

    /**
     *  Avoid non-aligned regions; we assume in the code that the
     *  regions are page aligned so consider this to be a bug.
     */
    assert(!(_start % TheISA::PageBytes));
    assert(!((_end + 1) % TheISA::PageBytes));
}

/*
 * Copyright (c) 2016 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * For use for simulation and test purposes only
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Michael LeBeane
 */

#ifndef __BASE_MAPPED_BUF_HH__
#define __BASE_MAPPED_BUF_HH__

#include <sys/mman.h>
#include <sys/stat.h>

#include "base/logging.hh"
#include "base/types.hh"

/**
 * MappedFileBuffer is a wrapper around a region of host memory backed by a
 * file. The constructor attempts to map a file from host memory, and the
 * destructor attempts to unmap it.  If there is a problem with the host
 * mapping/unmapping, then we panic.
 */
struct MappedFileBuffer
{
    char *buf;          // Host buffer ptr
    uint64_t len;       // Length of host ptr

    MappedFileBuffer(int fd, uint64_t length, uint64_t offset)
        : buf(nullptr), len(0)
    {
        struct stat file_stat;
        if (fstat(fd, &file_stat) > 0)
            panic("mmap: cannot stat file");
        // Don't bother mapping more than the actual file size
        len = std::min((uint64_t)file_stat.st_size - offset, length);
        // cannot call mmap with len == 0
        if (len) {
            buf = (char *)mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, offset);
            if (buf == MAP_FAILED)
                panic("mmap: failed to map file into host address space");
        }
    }

    ~MappedFileBuffer()
    {
        if (buf)
            if (munmap(buf, len) == -1)
                panic("mmap: failed to unmap file-backed host memory");
    }
};

#endif // __BASE_MAPPED_BUF_HH__

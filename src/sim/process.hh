/*
 * Copyright (c) 2014-2016 Advanced Micro Devices, Inc.
 * Copyright (c) 2001-2005 The Regents of The University of Michigan
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
 * Authors: Nathan Binkert
 *          Steve Reinhardt
 *          Brandon Potter
 */

#ifndef __PROCESS_HH__
#define __PROCESS_HH__

#include <inttypes.h>

#include <string>
#include <vector>

#include "arch/registers.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "sim/fd_array.hh"
#include "sim/fd_entry.hh"
#include "sim/mem_state.hh"
#include "sim/sim_object.hh"

struct ProcessParams;

class EmulatedDriver;
class ObjectFile;
class EmulationPageTable;
class SyscallDesc;
class SyscallReturn;
class System;
class ThreadContext;

class Process : public SimObject
{
  public:
    Process(ProcessParams *params, ObjectFile *obj_file);

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    void initState() override;
    DrainState drain() override;

    virtual void syscall(int64_t callnum, ThreadContext *tc, Fault *fault);
    virtual TheISA::IntReg getSyscallArg(ThreadContext *tc, int &i) = 0;
    virtual TheISA::IntReg getSyscallArg(ThreadContext *tc, int &i, int width);
    virtual void setSyscallArg(ThreadContext *tc, int i,
                               TheISA::IntReg val) = 0;
    virtual void setSyscallReturn(ThreadContext *tc,
                                  SyscallReturn return_value) = 0;
    virtual SyscallDesc *getDesc(int callnum) = 0;

    inline uint64_t uid() { return _uid; }
    inline uint64_t euid() { return _euid; }
    inline uint64_t gid() { return _gid; }
    inline uint64_t egid() { return _egid; }
    inline uint64_t pid() { return _pid; }
    inline uint64_t ppid() { return _ppid; }
    inline uint64_t pgid() { return _pgid; }
    inline uint64_t tgid() { return _tgid; }
    inline void setpgid(uint64_t pgid) { _pgid = pgid; }

    const char *progName() const { return executable.c_str(); }

    std::string absPath(const std::string &file_name);

    std::string getTgtCwd() const { return tgt_cwd; }
    void setTgtCwd(std::string path) { tgt_cwd = path; }

    std::string getHostCwd() const { return host_cwd; }
    void setHostCwd(std::string path) { host_cwd = path; }

    // Redirect file path if it matches any keys defined by system.
    // Always returns an absolute path.
    std::string checkPathRedirect(const std::string &filename);

    /**
     * Find an emulated device driver.
     *
     * @param filename Name of the device (under /dev)
     * @return Pointer to driver object if found, else nullptr
     */
    EmulatedDriver *findDriver(std::string filename);

    // This function acts as a callback to update the bias value in
    // the object file because the parameters needed to calculate the
    // bias are not available when the object file is created.
    void updateBias();
    Addr getBias();
    Addr getStartPC();
    ObjectFile *getInterpreter();

    // override of virtual SimObject method: register statistics
    void regStats() override;

    // After getting registered with system object, tell process which
    // system-wide context id it is assigned.
    void
    assignThreadContext(ContextID context_id)
    {
        contextIds.push_back(context_id);
    }

    // Find a free context to use
    ThreadContext *findFreeContext();

    /**
     * After delegating a thread context to a child process
     * no longer should relate to the ThreadContext
     */
    void revokeThreadContext(int context_id);

    bool map(Addr vaddr, Addr paddr, int size, bool cacheable = true)
    {
        return memState->map(vaddr, paddr, size, cacheable);
    };

    virtual void clone(ThreadContext *old_tc, ThreadContext *new_tc,
                       Process *new_p, TheISA::IntReg flags);

    // thread contexts associated with this process
    std::vector<ContextID> contextIds;

    // system object which owns this process
    System *system;

    Stats::Scalar numSyscalls;  // track how many system calls are executed

    bool kvmInSE;   // running KVM requires special initialization
    /**
     * Flag used by initState to determine when the page table should be
     * cleared.
     */
    bool useForClone;

    ObjectFile *objFile;
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    std::string host_cwd;
    std::string tgt_cwd;
    std::string executable;

    // Id of the owner of the process
    uint64_t _uid;
    uint64_t _euid;
    uint64_t _gid;
    uint64_t _egid;

    // pid of the process and it's parent
    uint64_t _pid;
    uint64_t _ppid;
    uint64_t _pgid;
    uint64_t _tgid;

    // Emulated drivers available to this process
    std::vector<EmulatedDriver *> drivers;

    std::shared_ptr<FDArray> fds;

    bool *exitGroup;

    /**
     * Calls a futex wakeup at the address specified by this pointer when
     * this process exits.
     */
    uint64_t childClearTID;

    // Process was forked with SIGCHLD set.
    bool *sigchld;

    ProcessParams *_params;

    const std::shared_ptr<MemState> &getMemState() const { return memState; }

  protected:
    std::shared_ptr<MemState> memState;
};

#endif // __PROCESS_HH__

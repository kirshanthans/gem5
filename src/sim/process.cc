/*
 * Copyright (c) 2014-2016 Advanced Micro Devices, Inc.
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
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
 *          Ali Saidi
 *          Brandon Potter
 */

#include "sim/process.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <climits>
#include <csignal>
#include <map>
#include <string>
#include <vector>

#include "base/intmath.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/thread_context.hh"
#include "params/Process.hh"
#include "sim/emul_driver.hh"
#include "sim/fd_array.hh"
#include "sim/fd_entry.hh"
#include "sim/redirect_path.hh"
#include "sim/syscall_desc.hh"
#include "sim/system.hh"

#if THE_ISA == ALPHA_ISA
#include "arch/alpha/linux/process.hh"

#elif THE_ISA == SPARC_ISA
#include "arch/sparc/linux/process.hh"
#include "arch/sparc/solaris/process.hh"

#elif THE_ISA == MIPS_ISA
#include "arch/mips/linux/process.hh"

#elif THE_ISA == ARM_ISA
#include "arch/arm/freebsd/process.hh"
#include "arch/arm/linux/process.hh"

#elif THE_ISA == X86_ISA
#include "arch/x86/linux/process.hh"

#elif THE_ISA == POWER_ISA
#include "arch/power/linux/process.hh"

#elif THE_ISA == RISCV_ISA
#include "arch/riscv/linux/process.hh"

#else
#error "THE_ISA not set"
#endif


using namespace std;
using namespace TheISA;

Process::Process(ProcessParams *params, ObjectFile *obj_file)
    : SimObject(params), system(params->system),
      kvmInSE(params->kvmInSE),
      useForClone(false),
      objFile(obj_file),
      argv(params->cmd), envp(params->env),
      host_cwd(checkPathRedirect(params->cwd)), tgt_cwd(params->cwd),
      executable(params->executable),
      _uid(params->uid), _euid(params->euid),
      _gid(params->gid), _egid(params->egid),
      _pid(params->pid), _ppid(params->ppid),
      _pgid(params->pgid), drivers(params->drivers),
      fds(make_shared<FDArray>(params->input, params->output, params->errout)),
      childClearTID(0),
      _params(params)
{
    if (_pid >= System::maxPID)
        fatal("_pid is too large: %d", _pid);

    auto ret_pair = system->PIDs.emplace(_pid);
    if (!ret_pair.second)
        fatal("_pid %d is already used", _pid);

    /**
     * Linux bundles together processes into this concept called a thread
     * group. The thread group is responsible for recording which processes
     * behave as threads within a process context. The thread group leader
     * is the process who's tgid is equal to its pid. Other processes which
     * belong to the thread group, but do not lead the thread group, are
     * treated as child threads. These threads are created by the clone system
     * call with options specified to create threads (differing from the
     * options used to implement a fork). By default, set up the tgid/pid
     * with a new, equivalent value. If CLONE_THREAD is specified, patch
     * the tgid value with the old process' value.
     */
    _tgid = params->pid;

    exitGroup = new bool();
    sigchld = new bool();

    if (!debugSymbolTable) {
        debugSymbolTable = new SymbolTable();
        if (!objFile->loadGlobalSymbols(debugSymbolTable) ||
            !objFile->loadLocalSymbols(debugSymbolTable) ||
            !objFile->loadWeakSymbols(debugSymbolTable)) {
            delete debugSymbolTable;
            debugSymbolTable = nullptr;
        }
    }
}

void
Process::clone(ThreadContext *otc, ThreadContext *ntc,
               Process *np, TheISA::IntReg flags)
{
#ifndef CLONE_VM
#define CLONE_VM 0
#endif
#ifndef CLONE_FILES
#define CLONE_FILES 0
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD 0
#endif
    if (CLONE_VM & flags) {
        /**
         * Share the process memory address space between the new process
         * and the old process. Changes in one will be visible in the other
         * due to the pointer use.
         */
        np->memState = memState;
        memState->addOwner(np);
    } else {
        *np->memState = *memState;
    }

    if (CLONE_FILES & flags) {
        /**
         * The parent and child file descriptors are shared because the
         * two FDArray pointers are pointing to the same FDArray. Opening
         * and closing file descriptors will be visible to both processes.
         */
        np->fds = fds;
    } else {
        /**
         * Copy the file descriptors from the old process into the new
         * child process. The file descriptors entry can be opened and
         * closed independently of the other process being considered. The
         * host file descriptors are also dup'd so that the flags for the
         * host file descriptor is independent of the other process.
         */
        for (int tgt_fd = 0; tgt_fd < fds->getSize(); tgt_fd++) {
            std::shared_ptr<FDArray> nfds = np->fds;
            std::shared_ptr<FDEntry> this_fde = (*fds)[tgt_fd];
            if (!this_fde) {
                nfds->setFDEntry(tgt_fd, nullptr);
                continue;
            }
            nfds->setFDEntry(tgt_fd, this_fde->clone());

            auto this_hbfd = std::dynamic_pointer_cast<HBFDEntry>(this_fde);
            if (!this_hbfd)
                continue;

            int this_sim_fd = this_hbfd->getSimFD();
            if (this_sim_fd <= 2)
                continue;

            int np_sim_fd = dup(this_sim_fd);
            assert(np_sim_fd != -1);

            auto nhbfd = std::dynamic_pointer_cast<HBFDEntry>((*nfds)[tgt_fd]);
            nhbfd->setSimFD(np_sim_fd);
        }
    }

    if (CLONE_THREAD & flags) {
        np->_tgid = _tgid;
        delete np->exitGroup;
        np->exitGroup = exitGroup;
    }

    np->argv.insert(np->argv.end(), argv.begin(), argv.end());
    np->envp.insert(np->envp.end(), envp.begin(), envp.end());
}

void
Process::regStats()
{
    SimObject::regStats();

    using namespace Stats;

    numSyscalls
        .name(name() + ".numSyscalls")
        .desc("Number of system calls")
        ;
}

ThreadContext *
Process::findFreeContext()
{
    for (auto &it : system->threadContexts) {
        if (ThreadContext::Halted == it->status())
            return it;
    }
    return nullptr;
}

void
Process::revokeThreadContext(int context_id)
{
    std::vector<ContextID>::iterator it;
    for (it = contextIds.begin(); it != contextIds.end(); it++) {
        if (*it == context_id) {
            contextIds.erase(it);
            return;
        }
    }
    warn("Unable to find thread context to revoke");
}

void
Process::initState()
{
    if (contextIds.empty())
        fatal("Process %s is not associated with any HW contexts!\n", name());

    // first thread context for this process... initialize & enable
    ThreadContext *tc = system->getThreadContext(contextIds[0]);

    // mark this context as active so it will start ticking.
    tc->activate();
}

DrainState
Process::drain()
{
    fds->updateFileOffsets();
    return DrainState::Drained;
}

void
Process::serialize(CheckpointOut &cp) const
{
    warn("Checkpoints in SE mode currently do not work.");
    /**
     * Checkpoints for file descriptors currently do not work. Need to
     * come back and fix them at a later date.
     */

#if 0
    memState->serialize(cp);
     pTable->serialize(cp);
    for (int x = 0; x < fds->getSize(); x++)
        (*fds)[x].serializeSection(cp, csprintf("FDEntry%d", x));
#endif

}

void
Process::unserialize(CheckpointIn &cp)
{
    warn("Checkpoints in SE mode currently do not work.");
#if 0
    memState->unserialize(cp);
    pTable->unserialize(cp);
    for (int x = 0; x < fds->getSize(); x++)
        (*fds)[x]->unserializeSection(cp, csprintf("FDEntry%d", x));
    fds->restoreFileOffsets();
#endif
    // The above returns a bool so that you could do something if you don't
    // find the param in the checkpoint if you wanted to, like set a default
    // but in this case we'll just stick with the instantiated value if not
    // found.
}

void
Process::syscall(int64_t callnum, ThreadContext *tc, Fault *fault)
{
    numSyscalls++;

    SyscallDesc *desc = getDesc(callnum);
    if (desc == nullptr)
        fatal("Syscall %d out of range", callnum);

    desc->doSyscall(callnum, tc, fault);
}

IntReg
Process::getSyscallArg(ThreadContext *tc, int &i, int width)
{
    return getSyscallArg(tc, i);
}

EmulatedDriver *
Process::findDriver(std::string filename)
{
    for (EmulatedDriver *d : drivers) {
        if (d->match(filename))
            return d;
    }

    return nullptr;
}

std::string
Process::checkPathRedirect(const std::string &filename)
{
    if (filename.empty()) return filename;

    std::string full = absPath(filename);

    std::vector<std::string> dests;
    std::vector<RedirectPath*> redirect_paths = system->redirectPaths();

    // find first matching src
    for (auto path : redirect_paths) {
        if (startswith(filename, path->src())) {
            std::string tail = filename.substr(path->src().size());

            for (auto dest : path->dests()) {
                struct stat buf;
                // use if the file exists
                if (stat((dest + tail).c_str(), &buf) != -1) {
                    return dest + tail;
                }
            }
            // no existing file on matching redirect path, use first dest
            return path->dests()[0] + tail;
        }
    }

    // Because all RedirectPath src and dests begin with '/', all relative
    // paths will end up here, as well as absolute paths with no matches.
    // If a relative path, redirection only if the destination path or our
    // current tgt_cwd is in a redirected location.
    if (!startswith(filename, "/") && !host_cwd.empty() && !tgt_cwd.empty()) {
        char dest_tgt_cwd[PATH_MAX];
        if (realpath((tgt_cwd + "/" + filename).c_str(), dest_tgt_cwd) == NULL) {
            warn("Could not get canonical path name while redirecting.");
        }
        if (tgt_cwd != host_cwd ||
            dest_tgt_cwd != checkPathRedirect(dest_tgt_cwd)) {
            return checkPathRedirect(dest_tgt_cwd);
        }
    }

    // if no matches anywhere, return absolute filename with no redirection
    return full;
}

void
Process::updateBias()
{
    ObjectFile *interp = objFile->getInterpreter();

    if (!interp || !interp->relocatable())
        return;

    // Determine how large the interpreters footprint will be in the process
    // address space.
    Addr interp_mapsize = roundUp(interp->mapSize(), TheISA::PageBytes);

    // We are allocating the memory area; set the bias to the lowest address
    // in the allocated memory region.
    Addr mmap_end = memState->getMmapEnd();
    Addr ld_bias = memState->mmapGrowsDown() ?
                   mmap_end - interp_mapsize : mmap_end;

    // Adjust the process mmap area to give the interpreter room; the real
    // execve system call would just invoke the kernel's internal mmap
    // functions to make these adjustments.
    mmap_end = memState->mmapGrowsDown() ? ld_bias : mmap_end + interp_mapsize;
    memState->setMmapEnd(mmap_end);

    interp->updateBias(ld_bias);
}

ObjectFile *
Process::getInterpreter()
{
    return objFile->getInterpreter();
}

Addr
Process::getBias()
{
    ObjectFile *interp = getInterpreter();

    return interp ? interp->bias() : objFile->bias();
}

Addr
Process::getStartPC()
{
    ObjectFile *interp = getInterpreter();

    return interp ? interp->entryPoint() : objFile->entryPoint();
}

std::string
Process::absPath(const std::string &file_name)
{
    if (file_name.empty() || startswith(file_name, "/") ||
        host_cwd.empty() || tgt_cwd.empty()) {
        return file_name;
    }

    std::string full = tgt_cwd;

    if (tgt_cwd[tgt_cwd.size() - 1] != '/')
        full += '/';

    return full + file_name;
}

Process *
ProcessParams::create()
{
    Process *process = nullptr;

    // If not specified, set the executable parameter equal to the
    // simulated system's zeroth command line parameter
    if (executable == "") {
        executable = cmd[0];
    }

    ObjectFile *obj_file = createObjectFile(executable);
    if (obj_file == nullptr) {
        fatal("Can't load object file %s", executable);
    }

#if THE_ISA == ALPHA_ISA
    if (obj_file->getArch() != ObjectFile::Alpha)
        fatal("Object file architecture does not match compiled ISA (Alpha).");

    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        process = new AlphaLinuxProcess(this, obj_file);
        break;

      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == SPARC_ISA
    if (obj_file->getArch() != ObjectFile::SPARC64 &&
        obj_file->getArch() != ObjectFile::SPARC32)
        fatal("Object file architecture does not match compiled ISA (SPARC).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        if (obj_file->getArch() == ObjectFile::SPARC64) {
            process = new Sparc64LinuxProcess(this, obj_file);
        } else {
            process = new Sparc32LinuxProcess(this, obj_file);
        }
        break;

      case ObjectFile::Solaris:
        process = new SparcSolarisProcess(this, obj_file);
        break;

      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == X86_ISA
    if (obj_file->getArch() != ObjectFile::X86_64 &&
        obj_file->getArch() != ObjectFile::I386)
        fatal("Object file architecture does not match compiled ISA (x86).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        if (obj_file->getArch() == ObjectFile::X86_64) {
            process = new X86_64LinuxProcess(this, obj_file);
        } else {
            process = new I386LinuxProcess(this, obj_file);
        }
        break;

      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == MIPS_ISA
    if (obj_file->getArch() != ObjectFile::Mips)
        fatal("Object file architecture does not match compiled ISA (MIPS).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        process = new MipsLinuxProcess(this, obj_file);
        break;

      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == ARM_ISA
    ObjectFile::Arch arch = obj_file->getArch();
    if (arch != ObjectFile::Arm && arch != ObjectFile::Thumb &&
        arch != ObjectFile::Arm64)
        fatal("Object file architecture does not match compiled ISA (ARM).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        if (arch == ObjectFile::Arm64) {
            process = new ArmLinuxProcess64(this, obj_file,
                                            obj_file->getArch());
        } else {
            process = new ArmLinuxProcess32(this, obj_file,
                                            obj_file->getArch());
        }
        break;
      case ObjectFile::FreeBSD:
        if (arch == ObjectFile::Arm64) {
            process = new ArmFreebsdProcess64(this, obj_file,
                                              obj_file->getArch());
        } else {
            process = new ArmFreebsdProcess32(this, obj_file,
                                              obj_file->getArch());
        }
        break;
      case ObjectFile::LinuxArmOABI:
        fatal("M5 does not support ARM OABI binaries. Please recompile with an"
              " EABI compiler.");
      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == POWER_ISA
    if (obj_file->getArch() != ObjectFile::Power)
        fatal("Object file architecture does not match compiled ISA (Power).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        process = new PowerLinuxProcess(this, obj_file);
        break;

      default:
        fatal("Unknown/unsupported operating system.");
    }
#elif THE_ISA == RISCV_ISA
    if (obj_file->getArch() != ObjectFile::Riscv)
        fatal("Object file architecture does not match compiled ISA (RISCV).");
    switch (obj_file->getOpSys()) {
      case ObjectFile::UnknownOpSys:
        warn("Unknown operating system; assuming Linux.");
        // fall through
      case ObjectFile::Linux:
        process = new RiscvLinuxProcess(this, obj_file);
        break;
      default:
        fatal("Unknown/unsupported operating system.");
    }
#else
#error "THE_ISA not set"
#endif

    if (process == nullptr)
        fatal("Unknown error creating process object.");
    return process;
}

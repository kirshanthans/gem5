/*
 * Copyright (c) 2013-2015 Advanced Micro Devices, Inc.
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
 * Author: Sooraj Puthoor, Tuan Ta
 */

#include "base/logging.hh"
#include "base/str.hh"
#include "config/the_isa.hh"

#if THE_ISA == X86_ISA
#include "arch/x86/insts/microldstop.hh"

#endif // X86_ISA
#include "mem/ruby/system/VIPERCoalescer.hh"

#include "cpu/testers/rubytest/RubyTester.hh"
#include "debug/GPUCoalescer.hh"
#include "debug/MemoryAccess.hh"
#include "debug/ProtocolTrace.hh"
#include "mem/packet.hh"
#include "mem/ruby/common/SubBlock.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/profiler/Profiler.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#include "mem/ruby/structures/CacheMemory.hh"
#include "mem/ruby/system/GPUCoalescer.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "params/VIPERCoalescer.hh"

using namespace std;

VIPERCoalescer *
VIPERCoalescerParams::create()
{
    return new VIPERCoalescer(this);
}

VIPERCoalescer::VIPERCoalescer(const Params *p)
    : GPUCoalescer(p),
      m_cache_inv_pkt(nullptr),
      m_num_pending_invs(0)
{
}

VIPERCoalescer::~VIPERCoalescer()
{
}

// Places an uncoalesced packet in uncoalescedTable. If the packet is a
// special type (MemFence, scoping, etc), it is issued immediately.
RequestStatus
VIPERCoalescer::makeRequest(PacketPtr pkt)
{
    // VIPER only supports following memory request types
    //    MemSyncReq & INV_L1 : TCP cache invalidation
    //    ReadReq             : cache read
    //    WriteReq            : cache write
    //    AtomicOp            : cache atomic
    //
    // VIPER does not expect MemSyncReq & Release since in GCN3, compute unit
    // does not specify an equivalent type of memory request.
    assert((pkt->cmd == MemCmd::MemSyncReq && pkt->req->isInvL1()) ||
            pkt->cmd == MemCmd::ReadReq ||
            pkt->cmd == MemCmd::WriteReq ||
            pkt->isAtomicOp());

    if (pkt->req->isInvL1() && m_cache_inv_pkt) {
        // In VIPER protocol, the coalescer is not able to handle two or
        // more cache invalidation requests at a time. Cache invalidation
        // requests must be serialized to ensure that all stale data in
        // TCP are invalidated correctly. If there's already a pending
        // cache invalidation request, we must retry this request later
        return RequestStatus_Aliased;
    }

    GPUCoalescer::makeRequest(pkt);

    if (pkt->req->isInvL1()) {
        // In VIPER protocol, a compute unit sends a MemSyncReq with INV_L1
        // flag to invalidate TCP. Upon receiving a request of this type,
        // VIPERCoalescer starts a cache walk to invalidate all valid entries
        // in TCP. The request is completed once all entries are invalidated.
        assert(!m_cache_inv_pkt);
        m_cache_inv_pkt = pkt;
        invTCP();
    }

    return RequestStatus_Issued;
}

void
VIPERCoalescer::issueRequest(CoalescedRequest* crequest)
{
    PacketPtr pkt = crequest->getFirstPkt();

    int proc_id = -1;
    if (pkt != NULL && pkt->req->hasContextId()) {
        proc_id = pkt->req->contextId();
    }

    // If valid, copy the pc to the ruby request
    Addr pc = 0;
    if (pkt->req->hasPC()) {
        pc = pkt->req->getPC();
    }

    Addr line_addr = makeLineAddress(pkt->getAddr());

    // Creating WriteMask that records written bytes
    // and atomic operations. This enables partial writes
    // and partial reads of those writes
    DataBlock dataBlock;
    dataBlock.clear();
    uint32_t blockSize = RubySystem::getBlockSizeBytes();
    std::vector<bool> accessMask(blockSize,false);
    std::vector< std::pair<int,AtomicOpFunctor*> > atomicOps;
    uint32_t tableSize = crequest->getPackets().size();
    for (int i = 0; i < tableSize; i++) {
        PacketPtr tmpPkt = crequest->getPackets()[i];
        uint32_t tmpOffset = (tmpPkt->getAddr()) - line_addr;
        uint32_t tmpSize = tmpPkt->getSize();
        if (tmpPkt->isAtomicOp()) {
            std::pair<int,AtomicOpFunctor *> tmpAtomicOp(tmpOffset,
                                                        tmpPkt->getAtomicOp());
            atomicOps.push_back(tmpAtomicOp);
        } else if (tmpPkt->isWrite()) {
            dataBlock.setData(tmpPkt->getPtr<uint8_t>(),
                              tmpOffset, tmpSize);
        }
        for (int j = 0; j < tmpSize; j++) {
            accessMask[tmpOffset + j] = true;
        }
    }
    std::shared_ptr<RubyRequest> msg;
    if (pkt->isAtomicOp()) {
        msg = std::make_shared<RubyRequest>(clockEdge(), pkt->getAddr(),
                              pkt->getPtr<uint8_t>(),
                              pkt->getSize(), pc, crequest->getRubyType(),
                              RubyAccessMode_Supervisor, pkt,
                              PrefetchBit_No, proc_id, 100,
                              blockSize, accessMask,
                              dataBlock, atomicOps, crequest->getSeqNum());
    } else {
        msg = std::make_shared<RubyRequest>(clockEdge(), pkt->getAddr(),
                              pkt->getPtr<uint8_t>(),
                              pkt->getSize(), pc, crequest->getRubyType(),
                              RubyAccessMode_Supervisor, pkt,
                              PrefetchBit_No, proc_id, 100,
                              blockSize, accessMask,
                              dataBlock, crequest->getSeqNum());
    }

//    if (pkt->cmd == MemCmd::WriteReq) {
//        makeWriteCompletePkts(crequest);
//    }

    DPRINTFR(ProtocolTrace, "%15s %3s %10s%20s %6s>%-6s %s %s\n",
             curTick(), m_version, "Coal", "Begin", "", "",
             printAddress(msg->getPhysicalAddress()),
             RubyRequestType_to_string(crequest->getRubyType()));

    fatal_if(crequest->getRubyType() == RubyRequestType_IFETCH,
             "there should not be any I-Fetch requests in the GPU Coalescer");

    // Send the message to the cache controller
    fatal_if(m_data_cache_hit_latency == 0,
             "should not have a latency of zero");

    if (!deadlockCheckEvent.scheduled()) {
        schedule(deadlockCheckEvent,
                 m_deadlock_threshold * clockPeriod() +
                 curTick());
    }

    assert(m_mandatory_q_ptr);
    m_mandatory_q_ptr->enqueue(msg, clockEdge(), m_data_cache_hit_latency);
}

void
VIPERCoalescer::invTCPCallback(Addr addr)
{
    assert(m_cache_inv_pkt && m_num_pending_invs > 0);

    m_num_pending_invs--;

    if (m_num_pending_invs == 0) {
        std::vector<PacketPtr> pkt_list { m_cache_inv_pkt };
        completeHitCallback(pkt_list);
        m_cache_inv_pkt = nullptr;
    }
}

/**
  * Invalidate TCP
  */
void
VIPERCoalescer::invTCP()
{
    int size = m_dataCache_ptr->getNumBlocks();
    DPRINTF(GPUCoalescer,
            "There are %d Invalidations outstanding before Cache Walk\n",
            m_num_pending_invs);
    // Walk the cache
    for (int i = 0; i < size; i++) {
        Addr addr = m_dataCache_ptr->getAddressAtIdx(i);
        // Evict Read-only data
        std::shared_ptr<RubyRequest> msg = std::make_shared<RubyRequest>(
            clockEdge(), addr, (uint8_t*) 0, 0, 0,
            RubyRequestType_REPLACEMENT, RubyAccessMode_Supervisor,
            nullptr);
        DPRINTF(GPUCoalescer, "Evicting addr 0x%x\n", addr);
        assert(m_mandatory_q_ptr != NULL);
        m_mandatory_q_ptr->enqueue(msg, clockEdge(), m_data_cache_hit_latency);
        m_num_pending_invs++;
    }
    DPRINTF(GPUCoalescer,
            "There are %d Invalidatons outstanding after Cache Walk\n",
            m_num_pending_invs);
}

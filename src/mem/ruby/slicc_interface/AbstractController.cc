/*
 * Copyright (c) 2017 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2011-2014 Mark D. Hill and David A. Wood
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
 */

#include "mem/ruby/slicc_interface/AbstractController.hh"

#include "debug/RubyQueue.hh"
#include "mem/protocol/MemoryMsg.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/system/GPUCoalescer.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/system/Sequencer.hh"
#include "sim/system.hh"

AbstractController::AbstractController(const Params *p)
    : MemObject(p), Consumer(this), cur_mem_outstanding(0),
      m_version(p->version), m_clusterID(p->cluster_id),
      m_masterId(p->system->getMasterId(this)), m_is_blocking(false),
      m_number_of_TBEs(p->number_of_TBEs),
      m_transitions_per_cycle(p->transitions_per_cycle),
      m_buffer_size(p->buffer_size), m_recycle_latency(p->recycle_latency),
      memoryPort(csprintf("%s.memory", name()), this, ""),
      addrRanges(p->addr_ranges.begin(), p->addr_ranges.end())
{
    if (m_version == 0) {
        // Combine the statistics from all controllers
        // of this particular type.
        Stats::registerDumpCallback(new StatsCallback(this));
    }
}

void
AbstractController::init()
{
    params()->ruby_system->registerAbstractController(this);
    m_delayHistogram.init(10);
    uint32_t size = Network::getNumberOfVirtualNetworks();
    for (uint32_t i = 0; i < size; i++) {
        m_delayVCHistogram.push_back(new Stats::Histogram());
        m_delayVCHistogram[i]->init(10);
    }

    if (getMemReqQueue()) {
        getMemReqQueue()->setConsumer(this);
    }
}

void
AbstractController::resetStats()
{
    m_delayHistogram.reset();
    uint32_t size = Network::getNumberOfVirtualNetworks();
    for (uint32_t i = 0; i < size; i++) {
        m_delayVCHistogram[i]->reset();
    }
}

void
AbstractController::regStats()
{
    MemObject::regStats();

    roundTripWrDelay
        .init(0, 1600000, 10000)
        .name(name() + ".roundtrip_wr_delay")
        .desc("Roundtrip write delay from this dir ctrlr")
        .flags(Stats::pdf | Stats::oneline)
        ;

    roundTripRdDelay
        .init(0, 1600000, 10000)
        .name(name() + ".roundtrip_rd_delay")
        .desc("Roundtrip read delay from this dir ctrlr")
        .flags(Stats::pdf | Stats::oneline)
        ;

    tokenFlowBlocked
        .init(1, 100, 10)
        .name(name() + ".token_flow_blocked")
        .desc("Inflight mem requests;" \
              " sampled every time token flow blocked a request")
        ;

    outstandingMemReqs
        .init(1, 100, 10)
        .name(name() + ".inflight_mem_reqs")
        .desc("Inflight mem requests; sampled everytime a request is issued")
        ;

    respSlotsBlocked
        .init(1, 100, 10)
        .name(name() + ".resp_slots_blocked")
        .desc("Inflight mem requests; sampled " \
              "every time response slots availability blocked a request")
        ;

    m_fully_busy_cycles
        .name(name() + ".fully_busy_cycles")
        .desc("cycles for which number of transistions == max transitions")
        .flags(Stats::nozero);

    resp_queue_stall_count
        .name(name() + ".resp_queue_stall_count")
        .desc("Number of times a memory request stalled on response queue")
        .flags(Stats::nozero);
}

void
AbstractController::profileMsgDelay(uint32_t virtualNetwork, Cycles delay)
{
    assert(virtualNetwork < m_delayVCHistogram.size());
    m_delayHistogram.sample(delay);
    m_delayVCHistogram[virtualNetwork]->sample(delay);
}

void
AbstractController::stallBuffer(MessageBuffer* buf, Addr addr)
{
    if (m_waiting_buffers.count(addr) == 0) {
        MsgVecType* msgVec = new MsgVecType;
        msgVec->resize(m_in_ports, NULL);
        m_waiting_buffers[addr] = msgVec;
    }
    DPRINTF(RubyQueue, "stalling %s port %d addr %#x\n", buf, m_cur_in_port,
            addr);
    assert(m_in_ports > m_cur_in_port);
    (*(m_waiting_buffers[addr]))[m_cur_in_port] = buf;
}

void
AbstractController::wakeUpBuffers(Addr addr)
{
    if (m_waiting_buffers.count(addr) > 0) {
        //
        // Wake up all possible lower rank (i.e. lower priority) buffers that could
        // be waiting on this message.
        //
        for (int in_port_rank = m_cur_in_port - 1;
             in_port_rank >= 0;
             in_port_rank--) {
            if ((*(m_waiting_buffers[addr]))[in_port_rank] != NULL) {
                (*(m_waiting_buffers[addr]))[in_port_rank]->
                    reanalyzeMessages(addr, clockEdge());
            }
        }
        delete m_waiting_buffers[addr];
        m_waiting_buffers.erase(addr);
    }
}

void
AbstractController::wakeUpAllBuffers(Addr addr)
{
    if (m_waiting_buffers.count(addr) > 0) {
        //
        // Wake up all possible lower rank (i.e. lower priority) buffers that could
        // be waiting on this message.
        //
        for (int in_port_rank = m_in_ports - 1;
             in_port_rank >= 0;
             in_port_rank--) {
            if ((*(m_waiting_buffers[addr]))[in_port_rank] != NULL) {
                (*(m_waiting_buffers[addr]))[in_port_rank]->
                    reanalyzeMessages(addr, clockEdge());
            }
        }
        delete m_waiting_buffers[addr];
        m_waiting_buffers.erase(addr);
    }
}

void
AbstractController::wakeUpAllBuffers()
{
    //
    // Wake up all possible buffers that could be waiting on any message.
    //

    std::vector<MsgVecType*> wokeUpMsgVecs;
    MsgBufType wokeUpMsgBufs;

    if (m_waiting_buffers.size() > 0) {
        for (WaitingBufType::iterator buf_iter = m_waiting_buffers.begin();
             buf_iter != m_waiting_buffers.end();
             ++buf_iter) {
             for (MsgVecType::iterator vec_iter = buf_iter->second->begin();
                  vec_iter != buf_iter->second->end();
                  ++vec_iter) {
                  //
                  // Make sure the MessageBuffer has not already be reanalyzed
                  //
                  if (*vec_iter != NULL &&
                      (wokeUpMsgBufs.count(*vec_iter) == 0)) {
                      (*vec_iter)->reanalyzeAllMessages(clockEdge());
                      wokeUpMsgBufs.insert(*vec_iter);
                  }
             }
             wokeUpMsgVecs.push_back(buf_iter->second);
        }

        for (std::vector<MsgVecType*>::iterator wb_iter = wokeUpMsgVecs.begin();
             wb_iter != wokeUpMsgVecs.end();
             ++wb_iter) {
             delete (*wb_iter);
        }

        m_waiting_buffers.clear();
    }
}

bool
AbstractController::serviceMemoryQueue()
{
    assert(getMemReqQueue());
    if (!getMemReqQueue()->isReady(clockEdge())) {
        return false;
    }

    int respSlots = cur_mem_outstanding + 1;
    if (!getMemRespQueue()->areNSlotsAvailable(respSlots, clockEdge())) {
        resp_queue_stall_count++;
        // Since we need to wait for the response queue to drain, check
        // if directory has popped something on the next cycle
        scheduleEvent(Cycles(1));
        respSlotsBlocked.sample(cur_mem_outstanding);
        return false;
    }

    const MemoryMsg *mem_msg = (const MemoryMsg*)getMemReqQueue()->peek();
    unsigned int req_size = RubySystem::getBlockSizeBytes();
    if (mem_msg->m_Len > 0) {
        req_size = mem_msg->m_Len;
    }

    RequestPtr req = new Request(mem_msg->m_addr, req_size, 0, m_masterId);
    PacketPtr pkt;
    if (mem_msg->getType() == MemoryRequestType_MEMORY_WB) {
        pkt = Packet::createWrite(req);
        uint8_t *newData = new uint8_t[req_size];
        pkt->dataDynamic(newData);
        int offset = getOffset(mem_msg->m_addr);
        memcpy(newData, mem_msg->m_DataBlk.getData(offset, req_size),
               req_size);
    } else if (mem_msg->getType() == MemoryRequestType_MEMORY_READ) {
        pkt = Packet::createRead(req);
        uint8_t *newData = new uint8_t[req_size];
        pkt->dataDynamic(newData);
    } else {
        panic("Unknown memory request type (%s) for addr %p",
              MemoryRequestType_to_string(mem_msg->getType()),
              mem_msg->m_addr);
    }

    SenderState *s = new SenderState(mem_msg->m_Sender);
    pkt->pushSenderState(s);

    if (RubySystem::getWarmupEnabled()) {
        cur_mem_outstanding++;
        // Use functional rather than timing accesses during warmup
        getMemReqQueue()->dequeue(clockEdge());
        memoryPort.sendFunctional(pkt);
        // Since the queue was popped the controller may be able
        // to make more progress. Make sure it wakes up
        scheduleEvent(Cycles(1));
        recvTimingResp(pkt);
    } else if (memoryPort.tryTiming(pkt)) {
        packetMap[pkt] = curTick();
        getMemReqQueue()->dequeue(clockEdge());
        memoryPort.sendTimingReq(pkt);
        // Since the queue was popped the controller may be able
        // to make more progress. Make sure it wakes up
        scheduleEvent(Cycles(1));
        cur_mem_outstanding++;
        outstandingMemReqs.sample(cur_mem_outstanding);
    } else {
        tokenFlowBlocked.sample(cur_mem_outstanding);
        delete pkt->req;
        delete pkt;
    }

    return true;
}

void
AbstractController::blockOnQueue(Addr addr, MessageBuffer* port)
{
    m_is_blocking = true;
    m_block_map[addr] = port;
}

bool
AbstractController::isBlocked(Addr addr) const
{
    return m_is_blocking && (m_block_map.find(addr) != m_block_map.end());
}

void
AbstractController::unblock(Addr addr)
{
    m_block_map.erase(addr);
    if (m_block_map.size() == 0) {
       m_is_blocking = false;
    }
}

bool
AbstractController::isBlocked(Addr addr)
{
    return (m_block_map.count(addr) > 0);
}

BaseMasterPort &
AbstractController::getMasterPort(const std::string &if_name,
                                  PortID idx)
{
    return memoryPort;
}

void
AbstractController::functionalMemoryRead(PacketPtr pkt)
{
    memoryPort.sendFunctional(pkt);
}

int
AbstractController::functionalMemoryWrite(PacketPtr pkt)
{
    int num_functional_writes = 0;

    // Check the buffer from the controller to the memory.
    if (memoryPort.checkFunctional(pkt)) {
        num_functional_writes++;
    }

    // Update memory itself.
    memoryPort.sendFunctional(pkt);
    return num_functional_writes + 1;
}

void
AbstractController::recvTimingResp(PacketPtr pkt)
{
    assert(getMemRespQueue());
    assert(pkt->isResponse());

    std::shared_ptr<MemoryMsg> msg = std::make_shared<MemoryMsg>(clockEdge());
    (*msg).m_addr = pkt->getAddr();
    (*msg).m_Sender = m_machineID;

    SenderState *s = dynamic_cast<SenderState *>(pkt->senderState);
    (*msg).m_OriginalRequestorMachId = s->id;
    delete s;

    if (pkt->isRead()) {
        roundTripRdDelay.sample(curTick() - packetMap[pkt]);
        (*msg).m_Type = MemoryRequestType_MEMORY_READ;
        (*msg).m_MessageSize = MessageSizeType_Response_Data;

        // Copy data from the packet
        (*msg).m_DataBlk.setData(pkt->getPtr<uint8_t>(), 0,
                                 RubySystem::getBlockSizeBytes());
    } else if (pkt->isWrite()) {
        roundTripWrDelay.sample(curTick() - packetMap[pkt]);
        (*msg).m_Type = MemoryRequestType_MEMORY_WB;
        (*msg).m_MessageSize = MessageSizeType_Writeback_Control;
    } else {
        panic("Incorrect packet type received from memory controller!");
    }

    cur_mem_outstanding--;
    getMemRespQueue()->enqueue(msg, clockEdge(), cyclesToTicks(Cycles(1)));
    packetMap.erase(pkt);
    delete pkt->req;
    delete pkt;
}

Tick
AbstractController::recvAtomic(PacketPtr pkt)
{
   return ticksToCycles(memoryPort.sendAtomic(pkt));
}

MachineID
AbstractController::mapAddressToMachine(Addr addr, MachineType mtype) const
{
    NodeID node = m_net_ptr->addressToNodeID(addr, mtype);
    MachineID mach = {mtype, node};
    return mach;
}

bool
AbstractController::MemoryPort::recvTimingResp(PacketPtr pkt)
{
    controller->recvTimingResp(pkt);
    return true;
}

AbstractController::MemoryPort::MemoryPort(const std::string &_name,
                                           AbstractController *_controller,
                                           const std::string &_label)
    : QueuedMasterPort(_name, _controller, reqQueue, snoopRespQueue),
      reqQueue(*_controller, *this, _label),
      snoopRespQueue(*_controller, *this, _label),
      controller(_controller)
{
}

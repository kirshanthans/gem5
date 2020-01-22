/*
 * Copyright (c) 2011-2015 Advanced Micro Devices, Inc.
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
 * Authors: John Kalamatianos,
 *          Anthony Gutierrez
 */

#ifndef __COMPUTE_UNIT_HH__
#define __COMPUTE_UNIT_HH__

#include <deque>
#include <map>
#include <unordered_set>
#include <vector>

#include "base/callback.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "config/the_gpu_isa.hh"
#include "enums/PrefetchType.hh"
#include "gpu-compute/exec_stage.hh"
#include "gpu-compute/fetch_stage.hh"
#include "gpu-compute/global_memory_pipeline.hh"
#include "gpu-compute/hsa_queue_entry.hh"
#include "gpu-compute/local_memory_pipeline.hh"
#include "gpu-compute/register_manager.hh"
#include "gpu-compute/scalar_memory_pipeline.hh"
#include "gpu-compute/schedule_stage.hh"
#include "gpu-compute/scoreboard_check_stage.hh"
#include "mem/mem_object.hh"
#include "mem/port.hh"
#include "mem/token_port.hh"

class HSAQueueEntry;
class LdsChunk;
class ScalarRegisterFile;
class Shader;
class VectorRegisterFile;

struct ComputeUnitParams;

enum EXEC_POLICY
{
    OLDEST = 0,
    RR
};

enum TLB_CACHE
{
    TLB_MISS_CACHE_MISS = 0,
    TLB_MISS_CACHE_HIT,
    TLB_HIT_CACHE_MISS,
    TLB_HIT_CACHE_HIT
};

class ComputeUnit : public MemObject
{
  public:


    // Execution resources
    //
    // The ordering of units is:
    // Vector ALUs
    // Scalar ALUs
    // GM Pipe
    // LM Pipe
    // Scalar Mem Pipe
    //
    // Note: the ordering of units is important and the code assumes the
    // above ordering. However, there may be more than one resource of
    // each type (e.g., 4 VALUs or 2 SALUs)

    int numVectorGlobalMemUnits;
    // Resource control for global memory to VRF data/address bus
    WaitClass glbMemToVrfBus;
    // Resource control for Vector Register File->Global Memory pipe buses
    WaitClass vrfToGlobalMemPipeBus;
    // Resource control for Vector Global Memory execution unit
    WaitClass vectorGlobalMemUnit;

    int numVectorSharedMemUnits;
    // Resource control for local memory to VRF data/address bus
    WaitClass locMemToVrfBus;
    // Resource control for Vector Register File->Local Memory pipe buses
    WaitClass vrfToLocalMemPipeBus;
    // Resource control for Vector Shared/Local Memory execution unit
    WaitClass vectorSharedMemUnit;

    int numScalarMemUnits;
    // Resource control for scalar memory to SRF data/address bus
    WaitClass scalarMemToSrfBus;
    // Resource control for Scalar Register File->Scalar Memory pipe buses
    WaitClass srfToScalarMemPipeBus;
    // Resource control for Scalar Memory execution unit
    WaitClass scalarMemUnit;

    // vector ALU execution resources
    int numVectorALUs;
    std::vector<WaitClass> vectorALUs;

    // scalar ALU execution resources
    int numScalarALUs;
    std::vector<WaitClass> scalarALUs;

    // Return total number of execution units on this CU
    int numExeUnits() const;
    // index into readyList of the first memory unit
    int firstMemUnit() const;
    // index into readyList of the last memory unit
    int lastMemUnit() const;
    // index into scalarALUs vector of SALU used by the wavefront
    int mapWaveToScalarAlu(Wavefront *w) const;
    // index into readyList of SALU used by wavefront
    int mapWaveToScalarAluGlobalIdx(Wavefront *w) const;
    // index into readyList of Global Memory unit used by wavefront
    int mapWaveToGlobalMem(Wavefront *w) const;
    // index into readyList of Local Memory unit used by wavefront
    int mapWaveToLocalMem(Wavefront *w) const;
    // index into readyList of Scalar Memory unit used by wavefront
    int mapWaveToScalarMem(Wavefront *w) const;

    int vrfToCoalescerBusWidth; // VRF->Coalescer data bus width in bytes
    int coalescerToVrfBusWidth; // Coalescer->VRF data bus width in bytes
    int numCyclesPerStoreTransfer;  // number of cycles per vector store
    int numCyclesPerLoadTransfer;  // number of cycles per vector load

    // Buffers used to communicate between various pipeline stages

    // At a high level, the following intra-/inter-stage communication occurs:
    // SCB to SCH: readyList provides per exec resource list of waves that
    //             passed dependency and readiness checks. If selected by
    //             scheduler, attempt to add wave to schList conditional on
    //             RF support.
    // SCH: schList holds waves that are gathering operands or waiting
    //      for execution resource availability. Once ready, waves are
    //      placed on the dispatchList as candidates for execution. A wave
    //      may spend multiple cycles in SCH stage, on the schList due to
    //      RF access conflicts or execution resource contention.
    // SCH to EX: dispatchList holds waves that are ready to be executed.
    //            LM/FLAT arbitration may remove an LM wave and place it
    //            back on the schList. RF model may also force a wave back
    //            to the schList if using the detailed model.

    // List of waves which are ready to be scheduled.
    // Each execution resource has a ready list. readyList is
    // used to communicate between scoreboardCheck stage and
    // schedule stage
    std::vector<std::vector<Wavefront*>> readyList;

    // List of waves which will be dispatched to
    // each execution resource. An EXREADY implies
    // dispatch list is non-empty and
    // execution unit has something to execute
    // this cycle. Currently, the dispatch list of
    // an execution resource can hold only one wave because
    // an execution resource can execute only one wave in a cycle.
    // dispatchList is used to communicate between schedule
    // and exec stage
    // TODO: convert std::pair to a class to increase readability
    std::vector<std::pair<Wavefront*, DISPATCH_STATUS>> dispatchList;
    // track presence of dynamic instructions in the Schedule pipeline
    // stage. This is used to check the readiness of the oldest,
    // non-dispatched instruction of every WF in the Scoreboard stage.
    std::unordered_set<uint64_t> pipeMap;

    RegisterManager registerManager;

    FetchStage fetchStage;
    ScoreboardCheckStage scoreboardCheckStage;
    ScheduleStage scheduleStage;
    ExecStage execStage;
    GlobalMemPipeline globalMemoryPipe;
    LocalMemPipeline localMemoryPipe;
    ScalarMemPipeline scalarMemoryPipe;

    EventFunctionWrapper tickEvent;

    typedef ComputeUnitParams Params;
    std::vector<std::vector<Wavefront*>> wfList;
    int cu_id;

    // array of vector register files, one per SIMD
    std::vector<VectorRegisterFile*> vrf;
    // array of scalar register files, one per SIMD
    std::vector<ScalarRegisterFile*> srf;

    // Width per VALU/SIMD unit: number of work items that can be executed
    // on the vector ALU simultaneously in a SIMD unit
    int simdWidth;
    // number of pipe stages for bypassing data to next dependent single
    // precision vector instruction inside the vector ALU pipeline
    int spBypassPipeLength;
    // number of pipe stages for bypassing data to next dependent double
    // precision vector instruction inside the vector ALU pipeline
    int dpBypassPipeLength;
    // number of pipe stages for scalar ALU
    int scalarPipeStages;
    // number of pipe stages for operand collection & distribution network
    int operandNetworkLength;
    // number of cycles per instruction issue period
    Cycles issuePeriod;

    // VRF to GM Bus latency
    Cycles vrf_gm_bus_latency;
    // SRF to Scalar Mem Bus latency
    Cycles srf_scm_bus_latency;
    // VRF to LM Bus latency
    Cycles vrf_lm_bus_latency;

    // tracks the last cycle a vector instruction was executed on a SIMD
    std::vector<uint64_t> lastExecCycle;

    // Track the amount of interleaving between wavefronts on each SIMD.
    // This stat is sampled using instExecPerSimd to compute the number of
    // instructions that have been executed on a SIMD between a WF executing
    // two successive instructions.
    Stats::VectorDistribution instInterleave;

    // tracks the number of dyn inst executed per SIMD
    std::vector<uint64_t> instExecPerSimd;

    // true if we allow a separate TLB per lane
    bool perLaneTLB;
    // if 0, TLB prefetching is off.
    int prefetchDepth;
    // if fixed-stride prefetching, this is the stride.
    int prefetchStride;

    std::vector<Addr> lastVaddrCU;
    std::vector<std::vector<Addr>> lastVaddrSimd;
    std::vector<std::vector<std::vector<Addr>>> lastVaddrWF;
    Enums::PrefetchType prefetchType;
    EXEC_POLICY exec_policy;

    bool debugSegFault;
    // Idle CU timeout in ticks
    Tick idleCUTimeout;
    int idleWfs;
    bool functionalTLB;
    bool localMemBarrier;

    /*
     * for Counting page accesses
     *
     * cuExitCallback inherits from Callback. When you register a callback
     * function as an exit callback, it will get added to an exit callback
     * queue, such that on simulation exit, all callbacks in the callback
     * queue will have their process() function called.
     */
    bool countPages;

    Shader *shader;
    uint32_t barrier_id;

    Tick req_tick_latency;
    Tick resp_tick_latency;

    /**
     * Number of WFs to schedule to each SIMD. This vector is populated
     * by hasDispResources(), and consumed by the subsequent call to
     * dispWorkgroup(), to schedule the specified number of WFs to the
     * SIMD units. Entry I provides the number of WFs to schedule to SIMD I.
     */
    std::vector<int> numWfsToSched;

    // number of currently reserved vector registers per SIMD unit
    std::vector<int> vectorRegsReserved;
    // number of currently reserved scalar registers per SIMD unit
    std::vector<int> scalarRegsReserved;
    // number of vector registers per SIMD unit
    int numVecRegsPerSimd;
    // number of available scalar registers per SIMD unit
    int numScalarRegsPerSimd;

    void updateReadyList(int unitId);

    // this hash map will keep track of page divergence
    // per memory instruction per wavefront. The hash map
    // is cleared in GPUDynInst::updateStats() in gpu_dyn_inst.cc.
    std::map<Addr, int> pagesTouched;

    void insertInPipeMap(Wavefront *w);
    void deleteFromPipeMap(Wavefront *w);

    ComputeUnit(const Params *p);
    ~ComputeUnit();

    // Timing Functions
    int oprNetPipeLength() const { return operandNetworkLength; }
    int simdUnitWidth() const { return simdWidth; }
    int spBypassLength() const { return spBypassPipeLength; }
    int dpBypassLength() const { return dpBypassPipeLength; }
    int scalarPipeLength() const { return scalarPipeStages; }
    int storeBusLength() const { return numCyclesPerStoreTransfer; }
    int loadBusLength() const { return numCyclesPerLoadTransfer; }
    int wfSize() const { return wavefrontSize; }

    void exec();
    void initiateFetch(Wavefront *wavefront);
    void fetch(PacketPtr pkt, Wavefront *wavefront);
    void fillKernelState(Wavefront *w, HSAQueueEntry *task);

    void startWavefront(Wavefront *w, int waveId, LdsChunk *ldsChunk,
                        HSAQueueEntry *task, bool fetchContext=false);

    void doInvalidate(Request *req, int kernId);
    void doFlush(GPUDynInstPtr gpuDynInst);

    void dispWorkgroup(HSAQueueEntry *task, bool startFromScheduler=false);
    bool hasDispResources(HSAQueueEntry *task);

    int cacheLineSize() const { return _cacheLineSize; }
    int getCacheLineBits() const { return cacheLineBits; }

    /* This function cycles through all the wavefronts in all the phases to see
     * if all of the wavefronts which should be associated with one barrier
     * (denoted with _barrier_id), are all at the same barrier in the program
     * (denoted by bcnt). When the number at the barrier matches bslots, then
     * return true.
     */
    int AllAtBarrier(uint32_t _barrier_id, uint32_t bcnt, uint32_t bslots);

    template<typename c0, typename c1>
    void doSmReturn(GPUDynInstPtr gpuDynInst);

    virtual void init();
    void sendRequest(GPUDynInstPtr gpuDynInst, int index, PacketPtr pkt);
    void sendScalarRequest(GPUDynInstPtr gpuDynInst, PacketPtr pkt);
    void injectGlobalMemFence(GPUDynInstPtr gpuDynInst,
                              bool kernelMemSync,
                              RequestPtr req=nullptr);
    void handleMemPacket(PacketPtr pkt, int memport_index);
    bool processTimingPacket(PacketPtr pkt);
    void processFetchReturn(PacketPtr pkt);
    void updatePageDivergenceDist(Addr addr);

    MasterID masterId() { return _masterId; }

    bool isDone() const;
    bool isVectorAluIdle(uint32_t simdId) const;

  protected:
    MasterID _masterId;

    LdsState &lds;

  public:
    Stats::Scalar vALUInsts;
    Stats::Formula vALUInstsPerWF;
    Stats::Scalar sALUInsts;
    Stats::Formula sALUInstsPerWF;
    Stats::Scalar instCyclesVALU;
    Stats::Scalar instCyclesSALU;
    Stats::Scalar threadCyclesVALU;
    Stats::Formula vALUUtilization;
    Stats::Scalar ldsNoFlatInsts;
    Stats::Formula ldsNoFlatInstsPerWF;
    Stats::Scalar flatVMemInsts;
    Stats::Formula flatVMemInstsPerWF;
    Stats::Scalar flatLDSInsts;
    Stats::Formula flatLDSInstsPerWF;
    Stats::Scalar vectorMemWrites;
    Stats::Formula vectorMemWritesPerWF;
    Stats::Scalar vectorMemReads;
    Stats::Formula vectorMemReadsPerWF;
    Stats::Scalar scalarMemWrites;
    Stats::Formula scalarMemWritesPerWF;
    Stats::Scalar scalarMemReads;
    Stats::Formula scalarMemReadsPerWF;

    Stats::Formula vectorMemReadsPerKiloInst;
    Stats::Formula vectorMemWritesPerKiloInst;
    Stats::Formula vectorMemInstsPerKiloInst;
    Stats::Formula scalarMemReadsPerKiloInst;
    Stats::Formula scalarMemWritesPerKiloInst;
    Stats::Formula scalarMemInstsPerKiloInst;

    // Cycles required to send register source (addr and data) from
    // register files to memory pipeline, per SIMD.
    Stats::Vector instCyclesVMemPerSimd;
    Stats::Vector instCyclesScMemPerSimd;
    Stats::Vector instCyclesLdsPerSimd;

    Stats::Scalar globalReads;
    Stats::Scalar globalWrites;
    Stats::Formula globalMemInsts;
    Stats::Scalar argReads;
    Stats::Scalar argWrites;
    Stats::Formula argMemInsts;
    Stats::Scalar spillReads;
    Stats::Scalar spillWrites;
    Stats::Formula spillMemInsts;
    Stats::Scalar groupReads;
    Stats::Scalar groupWrites;
    Stats::Formula groupMemInsts;
    Stats::Scalar privReads;
    Stats::Scalar privWrites;
    Stats::Formula privMemInsts;
    Stats::Scalar readonlyReads;
    Stats::Scalar readonlyWrites;
    Stats::Formula readonlyMemInsts;
    Stats::Scalar kernargReads;
    Stats::Scalar kernargWrites;
    Stats::Formula kernargMemInsts;

    int activeWaves;
    Stats::Distribution waveLevelParallelism;

    void updateInstStats(GPUDynInstPtr gpuDynInst);

    // the following stats compute the avg. TLB accesslatency per
    // uncoalesced request (only for data)
    Stats::Scalar tlbRequests;
    Stats::Scalar tlbCycles;
    Stats::Formula tlbLatency;
    // hitsPerTLBLevel[x] are the hits in Level x TLB. x = 0 is the page table.
    Stats::Vector hitsPerTLBLevel;

    Stats::Scalar ldsBankAccesses;
    Stats::Distribution ldsBankConflictDist;

    // over all memory instructions executed over all wavefronts
    // how many touched 0-4 pages, 4-8, ..., 60-64 pages
    Stats::Distribution pageDivergenceDist;
    // count of non-flat global memory vector instructions executed
    Stats::Scalar dynamicGMemInstrCnt;
    // count of flat global memory vector instructions executed
    Stats::Scalar dynamicFlatMemInstrCnt;
    Stats::Scalar dynamicLMemInstrCnt;

    Stats::Scalar wgBlockedDueLdsAllocation;
    // Number of instructions executed, i.e. if 64 (or 32 or 7) lanes are
    // active when the instruction is committed, this number is still
    // incremented by 1
    Stats::Scalar numInstrExecuted;
    // Number of cycles among successive instruction executions across all
    // wavefronts of the same CU
    Stats::Distribution execRateDist;
    // number of individual vector operations executed
    Stats::Scalar numVecOpsExecuted;
    // number of individual f16 vector operations executed
    Stats::Scalar numVecOpsExecutedF16;
    // number of individual f32 vector operations executed
    Stats::Scalar numVecOpsExecutedF32;
    // number of individual f64 vector operations executed
    Stats::Scalar numVecOpsExecutedF64;
    // number of individual FMA 16,32,64 vector operations executed
    Stats::Scalar numVecOpsExecutedFMA16;
    Stats::Scalar numVecOpsExecutedFMA32;
    Stats::Scalar numVecOpsExecutedFMA64;
    // number of individual MAC 16,32,64 vector operations executed
    Stats::Scalar numVecOpsExecutedMAC16;
    Stats::Scalar numVecOpsExecutedMAC32;
    Stats::Scalar numVecOpsExecutedMAC64;
    // number of individual MAD 16,32,64 vector operations executed
    Stats::Scalar numVecOpsExecutedMAD16;
    Stats::Scalar numVecOpsExecutedMAD32;
    Stats::Scalar numVecOpsExecutedMAD64;
    // total number of two op FP vector operations executed
    Stats::Scalar numVecOpsExecutedTwoOpFP;
    // Total cycles that something is running on the GPU
    Stats::Scalar totalCycles;
    Stats::Formula vpc; // vector ops per cycle
    Stats::Formula vpc_f16; // vector ops per cycle
    Stats::Formula vpc_f32; // vector ops per cycle
    Stats::Formula vpc_f64; // vector ops per cycle
    Stats::Formula ipc; // vector instructions per cycle
    Stats::Distribution controlFlowDivergenceDist;
    Stats::Distribution activeLanesPerGMemInstrDist;
    Stats::Distribution activeLanesPerLMemInstrDist;
    // number of vector ALU instructions received
    Stats::Formula numALUInstsExecuted;
    // number of times a WG can not start due to lack of free VGPRs in SIMDs
    Stats::Scalar numTimesWgBlockedDueVgprAlloc;
    // number of times a WG can not start due to lack of free SGPRs in SIMDs
    Stats::Scalar numTimesWgBlockedDueSgprAlloc;
    Stats::Scalar numCASOps;
    Stats::Scalar numFailedCASOps;
    Stats::Scalar completedWfs;
    Stats::Scalar completedWGs;

    // distrubtion in latency difference between first and last cache block
    // arrival ticks
    Stats::Distribution headTailLatency;

    void
    regStats();

    LdsState &
    getLds() const
    {
        return lds;
    }

    int32_t
    getRefCounter(const uint32_t dispatchId, const uint32_t wgId) const;

    bool
    sendToLds(GPUDynInstPtr gpuDynInst) __attribute__((warn_unused_result));

    typedef std::unordered_map<Addr, std::pair<int, int>> pageDataStruct;
    pageDataStruct pageAccesses;

    class CUExitCallback : public Callback
    {
      private:
        ComputeUnit *computeUnit;

      public:
        virtual ~CUExitCallback() { }

        CUExitCallback(ComputeUnit *_cu)
        {
            computeUnit = _cu;
        }

        virtual void
        process();
    };

    CUExitCallback *cuExitCallback;

    // Manager for the number of tokens available to this compute unit to
    // send global memory request packets to the coalescer this is only used
    // between global memory pipe and TCP coalescer.
    TokenManager *memPortTokens;

    /** Data access Port **/
    class DataPort : public TokenMasterPort
    {
      public:
        DataPort(const std::string &_name, ComputeUnit *_cu, PortID _index)
            : TokenMasterPort(_name, _cu), computeUnit(_cu),
              index(_index) { }

        bool snoopRangeSent;

        struct SenderState : public Packet::SenderState
        {
            GPUDynInstPtr _gpuDynInst;
            int port_index;
            Packet::SenderState *saved;

            SenderState(GPUDynInstPtr gpuDynInst, PortID _port_index,
                        Packet::SenderState *sender_state=nullptr)
                : _gpuDynInst(gpuDynInst),
                  port_index(_port_index),
                  saved(sender_state) { }
        };

        void processMemReqEvent(PacketPtr pkt);
        EventFunctionWrapper *createMemReqEvent(PacketPtr pkt);

        void processMemRespEvent(PacketPtr pkt);
        EventFunctionWrapper *createMemRespEvent(PacketPtr pkt);

        std::deque<std::pair<PacketPtr, GPUDynInstPtr>> retries;

      protected:
        ComputeUnit *computeUnit;
        int index;

        virtual bool recvTimingResp(PacketPtr pkt);
        virtual Tick recvAtomic(PacketPtr pkt) { return 0; }
        virtual void recvFunctional(PacketPtr pkt) { }
        virtual void recvRangeChange() { }
        virtual void recvReqRetry();

        virtual void
        getDeviceAddressRanges(AddrRangeList &resp, bool &snoop)
        {
            resp.clear();
            snoop = true;
        }

    };

    // Scalar data cache access port
    class ScalarDataPort : public MasterPort
    {
      public:
        ScalarDataPort(const std::string &_name, ComputeUnit *_cu,
                       PortID _index)
            : MasterPort(_name, _cu, _index), computeUnit(_cu), index(_index)
        {
            (void)index;
        }

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

        struct SenderState : public Packet::SenderState
        {
            SenderState(GPUDynInstPtr gpuDynInst,
                        Packet::SenderState *sender_state=nullptr)
                : _gpuDynInst(gpuDynInst), saved(sender_state)
            {
            }

            GPUDynInstPtr _gpuDynInst;
            Packet::SenderState *saved;
        };

        class MemReqEvent : public Event
        {
          private:
            ScalarDataPort *scalarDataPort;
            PacketPtr pkt;

          public:
            MemReqEvent(ScalarDataPort *_scalar_data_port, PacketPtr _pkt)
                : Event(), scalarDataPort(_scalar_data_port), pkt(_pkt)
            {
              setFlags(Event::AutoDelete);
            }

            void process();
            const char *description() const;
        };

        std::deque<PacketPtr> retries;

      private:
        ComputeUnit *computeUnit;
        PortID index;
    };

    // Instruction cache access port
    class SQCPort : public MasterPort
    {
      public:
        SQCPort(const std::string &_name, ComputeUnit *_cu, PortID _index)
            : MasterPort(_name, _cu), computeUnit(_cu),
              index(_index) { }

        bool snoopRangeSent;

        struct SenderState : public Packet::SenderState
        {
            Wavefront *wavefront;
            Packet::SenderState *saved;
            // kernel id to be used in handling I-Cache invalidate response
            int kernId;

            SenderState(Wavefront *_wavefront, Packet::SenderState
                    *sender_state=nullptr, int _kernId=-1)
                : wavefront(_wavefront), saved(sender_state),
                kernId(_kernId){ }
        };

        std::deque<std::pair<PacketPtr, Wavefront*>> retries;

      protected:
        ComputeUnit *computeUnit;
        int index;

        virtual bool recvTimingResp(PacketPtr pkt);
        virtual Tick recvAtomic(PacketPtr pkt) { return 0; }
        virtual void recvFunctional(PacketPtr pkt) { }
        virtual void recvRangeChange() { }
        virtual void recvReqRetry();

        virtual void
        getDeviceAddressRanges(AddrRangeList &resp, bool &snoop)
        {
            resp.clear();
            snoop = true;
        }
     };

    /** Data TLB port **/
    class DTLBPort : public MasterPort
    {
      public:
        DTLBPort(const std::string &_name, ComputeUnit *_cu, PortID _index)
            : MasterPort(_name, _cu), computeUnit(_cu),
              index(_index), stalled(false)
        { }

        bool isStalled() { return stalled; }
        void stallPort() { stalled = true; }
        void unstallPort() { stalled = false; }

        /**
         * here we queue all the translation requests that were
         * not successfully sent.
         */
        std::deque<PacketPtr> retries;

        /** SenderState is information carried along with the packet
         * throughout the TLB hierarchy
         */
        struct SenderState: public Packet::SenderState
        {
            // the memInst that this is associated with
            GPUDynInstPtr _gpuDynInst;

            // the lane in the memInst this is associated with, so we send
            // the memory request down the right port
            int portIndex;

            // constructor used for packets involved in timing accesses
            SenderState(GPUDynInstPtr gpuDynInst, PortID port_index)
                : _gpuDynInst(gpuDynInst), portIndex(port_index) { }

        };

      protected:
        ComputeUnit *computeUnit;
        int index;
        bool stalled;

        virtual bool recvTimingResp(PacketPtr pkt);
        virtual Tick recvAtomic(PacketPtr pkt) { return 0; }
        virtual void recvFunctional(PacketPtr pkt) { }
        virtual void recvRangeChange() { }
        virtual void recvReqRetry();
    };

    class ScalarDTLBPort : public MasterPort
    {
      public:
        ScalarDTLBPort(const std::string &_name, ComputeUnit *_cu)
            : MasterPort(_name, _cu), computeUnit(_cu), stalled(false)
        {
        }

        struct SenderState : public Packet::SenderState
        {
            SenderState(GPUDynInstPtr gpuDynInst) : _gpuDynInst(gpuDynInst) { }
            GPUDynInstPtr _gpuDynInst;
        };

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override { assert(false); }

        bool isStalled() const { return stalled; }
        void stallPort() { stalled = true; }
        void unstallPort() { stalled = false; }

        std::deque<PacketPtr> retries;

      private:
        ComputeUnit *computeUnit;
        bool stalled;
    };

    class ITLBPort : public MasterPort
    {
      public:
        ITLBPort(const std::string &_name, ComputeUnit *_cu)
            : MasterPort(_name, _cu), computeUnit(_cu), stalled(false) { }


        bool isStalled() { return stalled; }
        void stallPort() { stalled = true; }
        void unstallPort() { stalled = false; }

        /**
         * here we queue all the translation requests that were
         * not successfully sent.
         */
        std::deque<PacketPtr> retries;

        /** SenderState is information carried along with the packet
         * throughout the TLB hierarchy
         */
        struct SenderState: public Packet::SenderState
        {
            // The wavefront associated with this request
            Wavefront *wavefront;

            SenderState(Wavefront *_wavefront) : wavefront(_wavefront) { }
        };

      protected:
        ComputeUnit *computeUnit;
        bool stalled;

        virtual bool recvTimingResp(PacketPtr pkt);
        virtual Tick recvAtomic(PacketPtr pkt) { return 0; }
        virtual void recvFunctional(PacketPtr pkt) { }
        virtual void recvRangeChange() { }
        virtual void recvReqRetry();
    };

    /**
     * the port intended to communicate between the CU and its LDS
     */
    class LDSPort : public MasterPort
    {
      public:
        LDSPort(const std::string &_name, ComputeUnit *_cu, PortID _id)
        : MasterPort(_name, _cu, _id), computeUnit(_cu)
        {
        }

        bool isStalled() const { return stalled; }
        void stallPort() { stalled = true; }
        void unstallPort() { stalled = false; }

        /**
         * here we queue all the requests that were
         * not successfully sent.
         */
        std::queue<PacketPtr> retries;

        /**
         *  SenderState is information carried along with the packet, esp. the
         *  GPUDynInstPtr
         */
        class SenderState: public Packet::SenderState
        {
          protected:
            // The actual read/write/atomic request that goes with this command
            GPUDynInstPtr _gpuDynInst = nullptr;

          public:
            SenderState(GPUDynInstPtr gpuDynInst):
              _gpuDynInst(gpuDynInst)
            {
            }

            GPUDynInstPtr
            getMemInst() const
            {
              return _gpuDynInst;
            }
        };

        virtual bool
        sendTimingReq(PacketPtr pkt);

      protected:

        bool stalled = false; ///< whether or not it is stalled

        ComputeUnit *computeUnit;

        virtual bool
        recvTimingResp(PacketPtr pkt);

        virtual Tick
        recvAtomic(PacketPtr pkt) { return 0; }

        virtual void
        recvFunctional(PacketPtr pkt)
        {
        }

        virtual void
        recvRangeChange()
        {
        }

        virtual void
        recvReqRetry();
    };

    /** The port to access the Local Data Store
     *  Can be connected to a LDS object
     */
    LDSPort *ldsPort = nullptr;

    LDSPort *
    getLdsPort() const
    {
        return ldsPort;
    }

    TokenManager *
    getTokenManager()
    {
        return memPortTokens;
    }

    /** The memory port for SIMD data accesses.
     *  Can be connected to PhysMem for Ruby for timing simulations
     */
    std::vector<DataPort*> memPort;
    // port to the TLB hierarchy (i.e., the L1 TLB)
    std::vector<DTLBPort*> tlbPort;
    // port to the scalar data cache
    ScalarDataPort *scalarDataPort;
    // port to the scalar data TLB
    ScalarDTLBPort *scalarDTLBPort;
    // port to the SQC (i.e. the I-cache)
    SQCPort *sqcPort;
    // port to the SQC TLB (there's a separate TLB for each I-cache)
    ITLBPort *sqcTLBPort;

    virtual BaseMasterPort&
    getMasterPort(const std::string &if_name, PortID idx)
    {
        if (if_name == "memory_port") {
            memPort[idx] = new DataPort(csprintf("%s-port%d", name(), idx),
                                        this, idx);
            return *memPort[idx];
        } else if (if_name == "translation_port") {
            tlbPort[idx] = new DTLBPort(csprintf("%s-port%d", name(), idx),
                                        this, idx);
            return *tlbPort[idx];
        } else if (if_name == "scalar_port") {
            scalarDataPort = new ScalarDataPort(csprintf("%s-port%d", name(),
                                                idx), this, idx);
            return *scalarDataPort;
        } else if (if_name == "scalar_tlb_port") {
            scalarDTLBPort = new ScalarDTLBPort(csprintf("%s-port", name()),
                                                this);
            return *scalarDTLBPort;
        } else if (if_name == "sqc_port") {
            sqcPort = new SQCPort(csprintf("%s-port%d", name(), idx),
                                  this, idx);
            return *sqcPort;
        } else if (if_name == "sqc_tlb_port") {
            sqcTLBPort = new ITLBPort(csprintf("%s-port", name()), this);
            return *sqcTLBPort;
        } else if (if_name == "ldsPort") {
            if (ldsPort) {
                fatal("an LDS port was already allocated");
            }
            ldsPort = new LDSPort(csprintf("%s-port", name()), this, idx);
            return *ldsPort;
        } else {
            panic("incorrect port name");
        }
    }

    InstSeqNum getAndIncSeqNum() { return globalSeqNum++; }

  private:
    const int _cacheLineSize;
    int cacheLineBits;
    InstSeqNum globalSeqNum;
    int wavefrontSize;

    // hold the time of the arrival of the first cache block related to
    // a particular GPUDynInst. This is used to calculate the difference
    // between the first and last chace block arrival times.
    std::map<GPUDynInstPtr, Tick> headTailMap;
};

#endif // __COMPUTE_UNIT_HH__

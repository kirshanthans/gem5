/*
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
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
 * Authors: Tuan Ta
 */

#ifndef CPU_TESTERS_PROTOCOL_TESTER_PROTOCOLTESTER_HH_
#define CPU_TESTERS_PROTOCOL_TESTER_PROTOCOLTESTER_HH_

/*
 * The tester includes the main ProtocolTester that manages all ports to the
 * memory system.
 * Threads are mapped to certain data port(s)
 *
 * Threads inject memory requests through their data ports.
 * The tester receives and validates responses from the memory.
 *
 * Main components
 *    - AddressManager: generate DRF request streams &
 *                      validate data response against an internal log_table
 *    - Episode: a sequence of requests
 *    - Thread: either GPU wavefront or CPU thread
 */

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "base/types.hh"
#include "cpu/testers/gpu_ruby_test/AddressManager.hh"
#include "mem/mem_object.hh"
#include "mem/packet.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "params/ProtocolTester.hh"

class Thread;
class CpuThread;
class GpuWavefront;

class ProtocolTester : public MemObject
{
  public:
    class SeqPort : public MasterPort
    {
      public:
        SeqPort(const std::string &_name, ProtocolTester *_tester, PortID _id,
                PortID _index)
            : MasterPort(_name, _tester, _id),
              tester(_tester)
        {}

      protected:
        virtual bool recvTimingResp(PacketPtr pkt);
        virtual void recvReqRetry();

      private:
          ProtocolTester* tester;
    };

    struct SenderState : public Packet::SenderState
    {
        Thread* th;
        SenderState(Thread* _th)
        {
            assert(_th);
            th = _th;
        }

        ~SenderState()
        {}
    };

  public:
    typedef ProtocolTesterParams Params;
    ProtocolTester(const Params *p);
    ~ProtocolTester();

    typedef AddressManager::Location Location;
    typedef AddressManager::Value Value;

    void init();
    MasterID masterId() { return _masterId; };
    virtual BaseMasterPort &getMasterPort(const std::string &if_name,
                                          PortID idx = InvalidPortID);

    int getEpisodeLength() const { return episodeLength; }
    // return pointer to the address manager
    AddressManager* getAddressManager() const { return addrManager; }
    // return true if the tester should stop issuing new episodes
    bool checkExit();
    // verify if a location to be picked for LD/ST will satisfy
    // data race free requirement
    bool checkDRF(Location atomic_loc, Location loc, bool isStore) const;
    // return the next episode id and increment it
    int getNextEpisodeID() { return nextEpisodeId++; }
    // get action sequence number
    int getActionSeqNum() { return actionCount++; }
    // retry all failed request pkts
    void retry();

    // dump error log into a file and exit the simulation
    void dumpErrorLog(std::stringstream& ss);

  private:
    MasterID _masterId;

    // list of parameters taken from python scripts
    int numCpuPorts;
    int numVectorPorts;
    int numSqcPorts;
    int numScalarPorts;
    int numCusPerSqc;
    int numCusPerScalar;
    int numWfsPerCu;
    int numWisPerWf;
    // parameters controlling the address range that the tester can access
    int numAtomicLocs;
    int numNormalLocsPerAtomic;
    // the number of actions in an episode (episodeLength +- random number)
    int episodeLength;
    // the maximum number of episodes to be completed by this tester
    int maxNumEpisodes;
    // are we debuggin the tester
    bool debugTester;

    // all available master ports connected to Ruby
    std::vector<MasterPort*> cpuPorts;      // cpu data ports
    std::vector<MasterPort*> cuVectorPorts; // ports to GPU vector cache
    std::vector<MasterPort*> cuSqcPorts;    // ports to GPU instruction cache
    std::vector<MasterPort*> cuScalarPorts; // ports to GPU scalar cache
    // all CPU and GPU threads
    std::vector<CpuThread*> cpuThreads;
    std::vector<GpuWavefront*> wfs;

    // address manager that (1) generates DRF sequences of requests,
    //                      (2) manages an internal log table and
    //                      (3) validate response data
    AddressManager* addrManager;

    // number of CPUs and CUs
    int numCpus;
    int numCus;
    // unique id of the next episode
    int nextEpisodeId;

    // global action count. Overflow is fine. It's used to uniquely identify
    // per-wave & per-instruction memory requests in the coalescer
    int actionCount;

    // if an exit signal was already sent
    bool sentExitSignal;

    OutputStream* logFile;
};

#endif /* CPU_TESTERS_PROTOCOL_TESTER_PROTOCOLTESTER_HH_ */

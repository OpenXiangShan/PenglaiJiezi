/*
 * Copyright (c) 2023 Todo
 *
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

#ifndef __CPU_O3_FETCH_TOP_HH__
#define __CPU_O3_FETCH_TOP_HH__

#include "cpu/o3/fetch.hh"
#include "cpu/o3/fetch_stage.hh"

namespace gem5
{
struct BaseO3CPUParams;

namespace o3
{
class CPU;

class FetchTop
{
  public:
    FetchTop(CPU *_cpu, const BaseO3CPUParams &params);
    std::function<void (void)> tick;
    std::function<void (void)>  startupStage;
    std::function<void (ThreadID tid)>  clearStates;
    std::function<RequestPort& (void)>  getInstPort;
    std::function<void (ThreadID tid)>  deactivateThread;
    std::function<void (std::list<ThreadID> *at_ptr)>  setActiveThreads;
    std::function<void (TimeBuffer<TimeStruct> *time_buffer)>  setTimeBuffer;
    std::function<void (TimeBuffer<FetchStruct> *fq_ptr)>  setFetchQueue;
    std::function<void (void)>  regProbePoints;
    std::function<void (void)>  wakeFromQuiesce;
    std::function<void (void)>  drainSanityCheck;
    std::function<bool (void)>  isDrained;
    std::function<void (ThreadID tid)>  drainStall;
    std::function<void (void)>  drainResume;
    std::function<void (void)>  takeOverFrom;
    std::function<void (void)>  flushFetchBuffer;

  public:
    InstDecoder *decoder[MaxThreads];

  private:
    std::shared_ptr<FetchStage> pFetchStage = nullptr;
    std::shared_ptr<Fetch> pFetch = nullptr;
};
}
}

#endif
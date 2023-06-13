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

#include "cpu/o3/dyn_inst.hh"
#include "debug/Fetch.hh"
#include "fetch_top.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{
FetchTop::FetchTop(CPU *_cpu, const BaseO3CPUParams &params)
{
    if (params.enable_decoupledFrontend) {
        std::cout << "use decoupled frontend " << std::endl;
        pFetchStage = std::make_shared<FetchStage>(_cpu, params);
        FetchStage& fetch = *pFetchStage;
        for (auto ii=0; ii<MaxThreads; ii++) {
          decoder[ii] = fetch.decoder[ii];
        }
        tick = [&](void) {fetch.tick();};
        startupStage = [&](void) {fetch.startupStage();};
        clearStates =  [&](ThreadID tid) {fetch.clearStates(tid);};
        getInstPort = [&](void)->RequestPort& {return fetch.getInstPort();};
        deactivateThread = [&](ThreadID tid) {fetch.deactivateThread(tid);};
        setActiveThreads = [&](std::list<ThreadID> *at_ptr) {
            fetch.setActiveThreads(at_ptr);};
        setTimeBuffer = [&](TimeBuffer<TimeStruct> *time_buffer) {
            fetch.setTimeBuffer(time_buffer);};
        setFetchQueue = [&](TimeBuffer<FetchStruct> *fq_ptr) {
            fetch.setFetchQueue(fq_ptr);};
        regProbePoints = [&](void) {fetch.regProbePoints();};
        wakeFromQuiesce = [&](void) {fetch.wakeFromQuiesce();};
        drainSanityCheck = [&](void) {fetch.drainSanityCheck();};
        isDrained = [&](void)->bool {return fetch.isDrained();};
        drainStall = [&](ThreadID tid) {fetch.drainStall(tid);};
        drainResume = [&](void) {fetch.drainResume();};
        takeOverFrom = [&](void) {fetch.takeOverFrom();};
        flushFetchBuffer = [&](void) {fetch.flushFetchBuffer();};
    } else {
        std::cout << "use GEM5 frontend " << std::endl;
        pFetch = std::make_shared<Fetch>(_cpu, params);
        Fetch& fetch = *pFetch;
        tick = [&](void) {fetch.tick();};
        startupStage = [&](void) {fetch.startupStage();};
        clearStates =  [&](ThreadID tid) {fetch.clearStates(tid);};
        getInstPort = [&](void)->RequestPort& {return fetch.getInstPort();};
        deactivateThread = [&](ThreadID tid) {fetch.deactivateThread(tid);};
        setActiveThreads = [&](std::list<ThreadID> *at_ptr) {
            fetch.setActiveThreads(at_ptr);};
        setTimeBuffer = [&](TimeBuffer<TimeStruct> *time_buffer) {
            fetch.setTimeBuffer(time_buffer);};
        setFetchQueue = [&](TimeBuffer<FetchStruct> *fq_ptr) {
            fetch.setFetchQueue(fq_ptr);};
        regProbePoints = [&](void) {fetch.regProbePoints();};
        wakeFromQuiesce = [&](void) {fetch.wakeFromQuiesce();};
        drainSanityCheck = [&](void) {fetch.drainSanityCheck();};
        isDrained = [&](void)->bool {return fetch.isDrained();};
        drainStall = [&](ThreadID tid) {fetch.drainStall(tid);};
        drainResume = [&](void) {fetch.drainResume();};
        takeOverFrom = [&](void) {fetch.takeOverFrom();};
        flushFetchBuffer = [&](void) {fetch.flushFetchBuffer();};
    }
}
}
}

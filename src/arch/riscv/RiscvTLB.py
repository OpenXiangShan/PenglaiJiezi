# -*- mode:python -*-

# Copyright (c) 2007 MIPS Technologies, Inc.
# Copyright (c) 2020 Barkhausen Institut
# Copyright (c) 2021 Huawei International
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.params import *
from m5.proxy import *

from m5.objects.BaseTLB import BaseTLB
from m5.objects.ClockedObject import ClockedObject

class RiscvPagetableWalker(ClockedObject):
    type = 'RiscvPagetableWalker'
    cxx_class = 'gem5::RiscvISA::Walker'
    cxx_header = 'arch/riscv/pagetable_walker.hh'

    port = RequestPort("Port for the hardware table walker")
    system = Param.System(Parent.any, "system object")
    num_squash_per_cycle = Param.Unsigned(4,
            "Number of outstanding walks that can be squashed per cycle")
    #notice :only partial testing of open ptwsquash was carried out
    #open it may have some bugs
    ptwSquash = Param.Bool(False,
    "when squash xs will continue ptw until ptw finish")
    # Grab the pma_checker from the MMU
    pma_checker = Param.PMAChecker(Parent.any, "PMA Checker")
    pmp = Param.PMP(Parent.any, "PMP")
    #OpenNextline = Param.Bool(False,"open nextline pre")
    OpenNextline = Param.Bool(True, "open nextline pre")

class RiscvTLB(BaseTLB):
    type = 'RiscvTLB'
    cxx_class = 'gem5::RiscvISA::TLB'
    cxx_header = 'arch/riscv/tlb.hh'

    #size = Param.Int(2048, "TLB size")
    #size = Param.Int(64, "TLB size")
    #size = Param.Int(256, "TLB size")
    is_L1tlb = Param.Bool(True,"the tlb is l1tlb")
    #the tlb has private l2tlb
    is_stage2 = Param.Bool(False,"the tlb is private l2tlb")
    #is_stage2 = Param.Bool(True,"the tlb is private l2tlb")
    is_the_sharedL2 = Param.Bool(False,"the tlb is shared l2tlb")
    size = Param.Int(64, "TLB size")
    #size = Param.Int(36, "TLB size")
    #l2tlb_l1_size = Param.Int(8, "l2TLB_l1 size")
    #l2tlb_l2_size = Param.Int(32, "l2TLB_l2 size")
    #l2tlb_l3_size = Param.Int(256, "l2TLB_l3 size")
    #l2tlb_sp_size = Param.Int(16, "l2TLB_sp size")
    l2tlb_l1_size = Param.Int(16, "l2TLB_l1 size")
    l2tlb_l2_size = Param.Int(64, "l2TLB_l2 size")
    l2tlb_l3_size = Param.Int(512, "l2TLB_l3 size")
    l2tlb_sp_size = Param.Int(16, "l2TLB_sp size")
    walker = Param.RiscvPagetableWalker(\
            RiscvPagetableWalker(), "page table walker")
    # Grab the pma_checker from the MMU
    pma_checker = Param.PMAChecker(Parent.any, "PMA Checker")
    pmp  = Param.PMP(Parent.any, "Physical Memory Protection Unit")
    isOpenNextline = Param.Bool(True, "open auto adjustment nextline")

class RiscvTLBL2(RiscvTLB):
    is_L1tlb = False
    is_the_sharedL2 = True


/*
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Copyright (c) 2006 INRIA (Institut National de Recherche en
 * Informatique et en Automatique  / French National Research Institute
 * for Computer Science and Applied Mathematics)
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
 *
 * Author: André Seznec, Pau Cabre, Javier Bueno
 *
 */

#include "cpu/pred/dppred_blk/new_sc_blk.hh"

namespace gem5 {

namespace branch_prediction {

New_SC_BLK::New_SC_BLK(const New_SC_BLKParams &p)
    : SimObject(p),
    logBias(p.logBias),
    logSizeUp(p.logSizeUp),
    logSizeUps(logSizeUp / 2),
    bwnb(p.bwnb),
    logBwnb(p.logBwnb),
    bwm(p.bwm),
    numEntriesFirstLocalHistories(p.numEntriesFirstLocalHistories),
    lnb(p.lnb),
    logLnb(p.logLnb),
    lm(p.lm),
    numEntriesSecondLocalHistories(p.numEntriesSecondLocalHistories),
    snb(p.snb),
    logSnb(p.logSnb),
    sm(p.sm),
    numEntriesThirdLocalHistories(p.numEntriesThirdLocalHistories),
    tnb(p.tnb),
    logTnb(p.logTnb),
    tm(p.tm),
    inb(p.inb),
    logInb(p.logInb),
    im(p.im),
    gnb(p.gnb),
    logGnb(p.logGnb),
    gm(p.gm),
    chooserConfWidth(p.chooserConfWidth),
    updateThresholdWidth(p.updateThresholdWidth),
    pUpdateThresholdWidth(p.pUpdateThresholdWidth),
    extraWeightsWidth(p.extraWeightsWidth),
    scCountersWidth(p.scCountersWidth),
    lWeightInitValue(p.lWeightInitValue),
    bwWeightInitValue(p.bwWeightInitValue),
    iWeightInitValue(p.iWeightInitValue),
    initialUpdateThresholdValue(p.initialUpdateThresholdValue),
    stats(this)
{
    for (unsigned br = 0; br < MaxNumBr; ++br){
        updateThreshold[br] = 35 << 3;
    }

    initGEHLTable(lnb, lm, lgehl, logLnb, wl, -1);
    initGEHLTable(snb, sm, sgehl, logSnb, ws, -1);
    initGEHLTable(tnb, tm, tgehl, logTnb, wt, -1);
    initGEHLTable(bwnb, bwm, bwgehl, logBwnb, wbw, -1);
    initGEHLTable(inb, im, igehl, logInb, wi, -1);
    initGEHLTable(gnb, gm, ggehl, logGnb, wg, -1);

    for (unsigned br = 0; br < MaxNumBr; ++br){
        pUpdateThreshold[br].resize(1 << logSizeUp);
        for (unsigned i = 0; i < (1 << logSizeUp); ++i){
            pUpdateThreshold[br][i] = initialUpdateThresholdValue;
        }
    }


    bias.resize(1 << logBias);
    biasSK.resize(1 << logBias);
    biasBank.resize(1 << logBias);
    wb.resize(1 << logSizeUps);
    for (unsigned i = 0; i < (1 << logSizeUps); ++i){
        for (unsigned br = 0; br < MaxNumBr; ++br){
            wb[i].ctr[br] = -1;
        }
    }
    scHistory = makeThreadHistory();
    initBias();

}

New_SC_BLK::BranchInfo*
New_SC_BLK::makeBranchInfo()
{
    return new BranchInfo(numEntriesFirstLocalHistories,
                          numEntriesSecondLocalHistories,
                          numEntriesThirdLocalHistories);
}

void
New_SC_BLK::initBias()
{
    for (int j = 0; j < (1 << logBias); j++) {
        for (unsigned br = 0; br < MaxNumBr; ++br){
            switch (j & 3) {
            case 0:
                bias[j].ctr[br] = 0;
                biasSK[j].ctr[br] = -1;
                biasBank[j].ctr[br] = 0;
                break;
            case 1:
                bias[j].ctr[br] = -1;
                biasSK[j].ctr[br] = 0;
                biasBank[j].ctr[br] = -1;
                break;
            case 2:
                bias[j].ctr[br] = 0;
                biasSK[j].ctr[br] = -1;
                biasBank[j].ctr[br] = 0;
                break;
            case 3:
                bias[j].ctr[br] = -1;
                biasSK[j].ctr[br] = 0;
                biasBank[j].ctr[br] = -1;
                break;
            }
        }
    }
}

void
New_SC_BLK::initGEHLTable(unsigned numLenghts,
    std::vector<int> lengths, std::vector<SCEntry> * & table,
    unsigned logNumEntries, std::vector<SCEntry> & w, int8_t wInitValue)
{
    assert(lengths.size() == numLenghts);
    if (numLenghts == 0) {
        return;
    }
    table = new std::vector<SCEntry> [numLenghts];
    for (int i = 0; i < numLenghts; ++i) {
        table[i].resize(1 << logNumEntries);
        for (int j = 0; j < ((1 << logNumEntries)); ++j) {
            if (! (j & 1)) {
                for (unsigned br = 0; br < MaxNumBr; ++br){
                    table[i][j].ctr[br] = -1;
                }
            }else{
                for (unsigned br = 0; br < MaxNumBr; ++br){
                    table[i][j].ctr[br] = 0;
                }
            }
        }
    }

    w.resize(1 << logSizeUps);
    for (int i = 0; i < (1 << logSizeUps); ++i) {
        for (unsigned br = 0; br < MaxNumBr; ++br){
            w[i].ctr[br] = wInitValue;
        }
    }
}

unsigned
New_SC_BLK::getIndBias(Addr branch_pc, BranchInfo* bi,
                            bool bias, unsigned br) const
{
    return (((((branch_pc ^(branch_pc >>2))<<1) ^
            (bi->lowConf[br] & bias)) <<1)
            +  bi->predBeforeSC[br]) & ((1<<logBias) -1);
}

unsigned
New_SC_BLK::getIndBiasSK(Addr branch_pc, BranchInfo* bi, unsigned br) const
{
    return (((((branch_pc ^ (branch_pc >> (logBias-2)))<<1) ^
           (bi->highConf[br]))<<1) + bi->predBeforeSC[br]) & ((1<<logBias) -1);
}

unsigned
New_SC_BLK::getIndUpd(Addr branch_pc) const
{
    return ((branch_pc ^ (branch_pc >>2)) & ((1 << (logSizeUp)) - 1));
}

unsigned
New_SC_BLK::getIndUpds(Addr branch_pc) const
{
    return ((branch_pc ^ (branch_pc >>2)) & ((1 << (logSizeUps)) - 1));
}

int64_t
New_SC_BLK::gIndex(Addr branch_pc, int64_t bhist, int logs, int nbr,
                             int i)
{
    return (((int64_t) branch_pc) ^ bhist ^ (bhist >> (8 - i)) ^
            (bhist >> (16 - 2 * i)) ^ (bhist >> (24 - 3 * i)) ^
            (bhist >> (32 - 3 * i)) ^ (bhist >> (40 - 4 * i))) &
           ((1 << (logs - gIndexLogsSubstr(nbr, i))) - 1);
}

int
New_SC_BLK::gPredict(Addr branch_pc, int64_t hist,
        std::vector<int> & length, std::vector<SCEntry> * tab, int nbr,
        int logs, std::vector<SCEntry> & w, unsigned br, unsigned bank)
{
    int percsum = 0;
    for (int i = 0; i < nbr; i++) {
        int64_t bhist = hist & ((int64_t) ((1 << length[i]) - 1));
        int64_t index = gIndex(branch_pc, bhist, logs, nbr, i);
        int8_t ctr = tab[i][index].ctr[bank];
        percsum += (2 * ctr + 1);
    }
    if (nbr > 0){
        percsum = (1 + (w[getIndUpds(branch_pc)].ctr[bank] >= 0)) * percsum;
    }
    return percsum;
}

void
New_SC_BLK::gUpdate(Addr branch_pc, bool taken, int64_t hist,
                   std::vector<int> & length, std::vector<SCEntry> * tab,
                   int nbr, int logs, std::vector<SCEntry> & w,
                   BranchInfo* bi, unsigned br, unsigned bank)
{
    int percsum = 0;
    for (int i = 0; i < nbr; i++) {
        int64_t bhist = hist & ((int64_t) ((1 << length[i]) - 1));
        int64_t index = gIndex(branch_pc, bhist, logs, nbr, i);
        percsum += (2 * tab[i][index].ctr[bank] + 1);
        ctrUpdate(tab[i][index].ctr[bank], taken, scCountersWidth);
    }
    if (nbr > 0){
        int xsum = bi->lsum[br] -
                ((w[getIndUpds(branch_pc)].ctr[bank] >= 0)) * percsum;
        if ((xsum + percsum >= 0) != (xsum >= 0)) {
            ctrUpdate(w[getIndUpds(branch_pc)].ctr[bank],
                  ((percsum >= 0) == taken),
                  extraWeightsWidth);
        }
    }
}

void
New_SC_BLK::gPredictions(ThreadID tid, Addr branch_pc,
                            BranchInfo* bi,
                            int & lsum, unsigned br,
                            unsigned bank, int& thres)
{   //accumulate 2 global history table result
    lsum += gPredict(
        branch_pc, scHistory->globalHist, gm,
                        ggehl, gnb, logGnb, wg, br, bank);

    //accumulate 2 global backward branch history table result
    lsum += gPredict(
        branch_pc, scHistory->bwHist, bwm,
                        bwgehl, bwnb, logBwnb, wbw, br, bank);

    //accumulate 3 local history table result
    lsum += gPredict(
        branch_pc, scHistory->getLocalHistory(1, branch_pc), lm,
        lgehl, lnb, logLnb, wl, br, bank);

    lsum += gPredict(
        branch_pc, scHistory->getLocalHistory(2, branch_pc), sm,
        sgehl, snb, logSnb, ws, br, bank);

    lsum += gPredict(
        branch_pc, scHistory->getLocalHistory(3, branch_pc), tm,
        tgehl, tnb, logTnb, wt, br, bank);

    //accumulate 1 loop counter table result
    lsum += gPredict(
        branch_pc, scHistory->imliCount, im,
                            igehl, inb, logInb, wi, br, bank);
    //lookup threshold
    thres = (updateThreshold[br]>>3) +
            pUpdateThreshold[bank][getIndUpd(branch_pc)];
}


void
New_SC_BLK::scPredict(ThreadID tid, Addr branch_pc, bool* cond_branch,
                     bool* always_taken, BranchInfo* &bi,
                     bool* pred_taken, bool* bias_bit,
                     int8_t* conf_ctr, unsigned conf_bits,
                     int* hitBank, int* altBank,
                     Addr* corrTarget, bool *confidence)
{
    int lsum[MaxNumBr];
    int8_t ctr[MaxNumBr];
    int thres[MaxNumBr];
    bool scPred[MaxNumBr];
    bool useScPred[MaxNumBr];
    bool bank_remap;
    unsigned bank;
    if (((branch_pc >> 1) & 1) == 1){
        bank_remap = true;
    }else{
        bank_remap = false;
    }

    bi = new BranchInfo(numEntriesFirstLocalHistories,
                        numEntriesSecondLocalHistories,
                        numEntriesThirdLocalHistories);
    bi->globalHist = scHistory->globalHist;
    bi->bwHist = scHistory->bwHist;
    bi->imliCount = scHistory->imliCount;
    *(bi->localHistories) = *(scHistory->localHistories);
    *(bi->localHistories+1) = *(scHistory->localHistories+1);
    *(bi->localHistories+2) = *(scHistory->localHistories+2);
    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (bank_remap){
            bank = !br;
        }else{
            bank = br;
        }
        if (cond_branch[br]) {
            bi->predBeforeSC[br] = pred_taken[br];
            // first calc/update the confidences from the TAGE prediction
            bi->lowConf[br] = (abs(2 * conf_ctr[br] + 1) == 1);
            bi->medConf[br] = (abs(2 * conf_ctr[br] + 1) == 5 ||
                                            abs(2 * conf_ctr[br] + 1) == 3);
            bi->highConf[br] = (abs(2 * conf_ctr[br] + 1) >=
                                (1<<conf_bits) - 1);

            lsum[br] = 0;

            ctr[br] = bias[getIndBias(branch_pc, bi,
                            bias_bit[br], br)].ctr[bank];
            lsum[br] += (2 * ctr[br] + 1);
            if (hasThreeBias){
                ctr[br] = biasSK[getIndBiasSK(branch_pc, bi, br)].ctr[bank];
                lsum[br] += (2 * ctr[br] + 1);
                ctr[br] = biasBank[getIndBiasBank(branch_pc, bi,
                                hitBank[br], altBank[br], br)].ctr[bank];
                lsum[br] += (2 * ctr[br] + 1);
            }

            lsum[br] = (1 + (wb[getIndUpds(branch_pc)].ctr[bank] >= 0))
                            * lsum[br];

            gPredictions(tid, branch_pc, bi, lsum[br], br, bank, thres[br]);

            // These will be needed at update time
            bi->lsum[br] = lsum[br];
            bi->thres[br] = thres[br];

            scPred[br] = (lsum[br] >= 0);
            useScPred[br] = false;
            bi->scPred[br] = pred_taken[br];
            if (pred_taken[br] != scPred[br] && !always_taken[br]) {
                //Choser uses TAGE confidence and |LSUM|
                if (bi->highConf[br]) {
                    if (abs (lsum[br]) > thres[br]/2) {
                        useScPred[br] = true;
                    }
                }

                if (bi->medConf[br]) {
                    if (abs (lsum[br]) > (thres[br]/4)) {
                        useScPred[br] = true;
                    }
                }

                if (bi->lowConf[br]) {
                    if (abs (lsum[br]) > (thres[br]/8)) {
                        useScPred[br] = true;
                    }
                }

                if (useScPred[br]) {
                    pred_taken[br] = scPred[br];
                    bi->scPred[br] = scPred[br];
                    if (abs (lsum[br]) > thres[br]/2){
                        //confidence[br] = true;
                    }
                }
            }
            bi->usedScPred[br] = useScPred[br];
        }
    }
}

void
New_SC_BLK::scHistoryUpdate(Addr branch_pc,
                        bool taken, Addr corrTarget)
{
    //update global history
    scHistory->globalHist = (scHistory->globalHist << 1) + taken;

    //update loop counter history
    int imliCountWidth = inb > 0 ? im[0] : 8;
    if (corrTarget < branch_pc) {
        //This branch corresponds to a loop
        if (!taken) {
            //exit of the "loop"
            scHistory->imliCount = 0;
        } else {
            if (scHistory->imliCount < ((1 << imliCountWidth) - 1)) {
                scHistory->imliCount++;
            }
        }
    }

    //update backward history
    scHistory->bwHist = (scHistory->bwHist << 1) +
                            (taken & (corrTarget < branch_pc));
    //update local history
    scHistory->updateLocalHistory(1, branch_pc, taken);
    scHistory->updateLocalHistory(2, branch_pc, taken);
    scHistory->updateLocalHistory(3, branch_pc, taken);
}

void
New_SC_BLK::specUpdateHist(Addr branch_pc, bool* cond_branch,
                        bool* pred_taken, Addr* corrTarget)
{
    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (cond_branch[br]) {
            scHistoryUpdate(branch_pc, pred_taken[br], corrTarget[br]);
            if (pred_taken[br]){
                break;
            }
        }
    }
}

void
New_SC_BLK::condBranchUpdate(ThreadID tid, Addr branch_pc,
                    bool* cond_branch, bool* taken, BranchInfo *bi,
                    bool* bias_bit, int* hitBank, int* altBank)
{
    bool scPred[MaxNumBr];
    int xsum[MaxNumBr];
    bool bank_remap;
    unsigned bank;
    if (((branch_pc >> 1) & 1) == 1){
        bank_remap = true;
    }else{
        bank_remap = false;
    }

    for (unsigned br = 0; br < MaxNumBr; ++br){
        if (bank_remap){
            bank = !br;
        }else{
            bank = br;
        }
        if (cond_branch[br]){
            scPred[br] = (bi->lsum[br] >= 0);
            if ((scPred[br] != taken[br]) ||
                ((abs(bi->lsum[br]) < bi->thres[br]))) {
                if (bi->predBeforeSC[br] != scPred[br]){
                    ctrUpdate(updateThreshold[br], (scPred[br] != taken[br]),
                                                    updateThresholdWidth);
                    ctrUpdate(pUpdateThreshold[bank][getIndUpd(branch_pc)],
                            (scPred[br] != taken[br]), pUpdateThresholdWidth);
                }

                unsigned indUpds = getIndUpds(branch_pc);
                unsigned indBias = getIndBias(branch_pc, bi, bias_bit[br], br);
                unsigned indBiasSK = getIndBiasSK(branch_pc, bi, br);
                unsigned indBiasBank = getIndBiasBank(branch_pc, bi,
                                                hitBank[br], altBank[br], br);
                if (hasThreeBias){
                    xsum[br] = bi->lsum[br] -
                        ((wb[indUpds].ctr[bank] >= 0) *
                          ((2 * bias[indBias].ctr[bank] + 1) +
                          (2 * biasSK[indBiasSK].ctr[bank] + 1) +
                          (2 * biasBank[indBiasBank].ctr[bank] + 1)));

                    if ((xsum[br] + ((2 * bias[indBias].ctr[bank] + 1) +
                        (2 * biasSK[indBiasSK].ctr[bank] + 1) +
                        (2 * biasBank[indBiasBank].ctr[bank] + 1)) >= 0) !=
                        (xsum[br] >= 0)){
                            ctrUpdate(wb[indUpds].ctr[bank],
                                (((2 * bias[indBias].ctr[bank] + 1) +
                                (2 * biasSK[indBiasSK].ctr[bank] + 1) +
                                (2 * biasBank[indBiasBank].ctr[bank] +
                                1) >= 0) ==
                                taken[br]), extraWeightsWidth);
                    }
                }else{
                    xsum[br] = bi->lsum[br] -
                        ((wb[indUpds].ctr[bank] >= 0) *
                          ((2 * bias[indBias].ctr[bank] + 1)));

                    if ((xsum[br] + ((2 * bias[indBias].ctr[bank] + 1)) >=
                            0) !=(xsum[br] >= 0)){
                            ctrUpdate(wb[indUpds].ctr[bank],
                                (((2 * bias[indBias].ctr[bank] + 1) >= 0) ==
                                taken[br]), extraWeightsWidth);
                    }
                }
                //update 3 base table
                ctrUpdate(bias[indBias].ctr[bank],
                                    taken[br], scCountersWidth);
                if (hasThreeBias){
                    ctrUpdate(biasSK[indBiasSK].ctr[bank],
                                    taken[br], scCountersWidth);
                    ctrUpdate(biasBank[indBiasBank].ctr[bank],
                                    taken[br], scCountersWidth);
                }
                //update global history table
                gUpdates(tid, branch_pc, taken[br], bi, br, bank);
            }
            updateStats(taken[br], bi, br);
        }
    }
}

void
New_SC_BLK::gUpdates(ThreadID tid, Addr pc, bool taken,
        BranchInfo* bi, unsigned br, unsigned bank)
{
    gUpdate(pc, taken, bi->globalHist, gm, ggehl,
            gnb, logGnb, wg, bi, br, bank);
    gUpdate(pc, taken, bi->bwHist, bwm, bwgehl,
            bwnb, logBwnb, wbw, bi, br, bank);
    gUpdate(pc, taken, bi->getLocalHistory(1, pc),
            lm, lgehl, lnb, logLnb, wl, bi, br, bank);
    gUpdate(pc, taken, bi->getLocalHistory(2, pc),
            sm, sgehl, snb, logSnb, ws, bi, br, bank);
    gUpdate(pc, taken, bi->getLocalHistory(3, pc),
            tm, tgehl, tnb, logTnb, wt, bi, br, bank);
    gUpdate(pc, taken, bi->imliCount, im,
            igehl, inb, logInb, wi, bi, br, bank);
}

//if flush，need to recover global history
void
New_SC_BLK::squash(ThreadID tid, bool taken, Addr branch_pc,
                   BranchInfo *bi, bool isCondMisp,
                   unsigned numBrBefore, Addr* corrTarget)
{
    bool taken_recover[MaxNumBr];
    unsigned num_recover_br;
    if (isCondMisp){
        if (numBrBefore == 0){
            taken_recover[0] = taken;
            taken_recover[1] = false;
            num_recover_br = 1;
        }else if (numBrBefore == 1){
            taken_recover[0] = false;
            taken_recover[1] = taken;
            num_recover_br = 2;
        }else{
            assert(false);
        }

        //recover all history and update with the right dir result
        scHistory->globalHist = bi->globalHist;
        scHistory->bwHist = bi->bwHist;
        scHistory->imliCount = bi->imliCount;
        *(scHistory->localHistories) = *(bi->localHistories);
        *(scHistory->localHistories+1) = *(bi->localHistories+1);
        *(scHistory->localHistories+2) = *(bi->localHistories+2);
        for (unsigned br = 0; br < num_recover_br; ++br){
            scHistoryUpdate(branch_pc, taken_recover[br], corrTarget[br]);
        }
    }else{
        if (numBrBefore == 0){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 0;
        }else if (numBrBefore == 1){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 1;
        }else if (numBrBefore == 2){
            taken_recover[0] = false;
            taken_recover[1] = false;
            num_recover_br = 2;
        }else{
            assert(false);
        }
        //recover all history and update with the right dir result
        scHistory->globalHist = bi->globalHist;
        scHistory->bwHist = bi->bwHist;
        scHistory->imliCount = bi->imliCount;
        *(scHistory->localHistories) = *(bi->localHistories);
        *(scHistory->localHistories+1) = *(bi->localHistories+1);
        *(scHistory->localHistories+2) = *(bi->localHistories+2);
        for (unsigned br = 0; br < num_recover_br; ++br){
            scHistoryUpdate(branch_pc, taken_recover[br], corrTarget[br]);
        }
    }
}


void
New_SC_BLK::free_mem(BranchInfo * &bi){
    /** free memory*/
    delete bi;
}

void
New_SC_BLK::updateStats(bool taken, BranchInfo *bi, unsigned br)
{
    stats.num_update_cond_br++;
    if (bi->usedScPred[br]){
        if (taken == bi->scPred[br] && taken != bi->predBeforeSC[br]) {
            stats.sc_correct_tage_wrong++;
        } else if (taken != bi->scPred[br] && taken == bi->predBeforeSC[br]){
            stats.sc_wrong_tage_correct++;
        }
        stats.sc_used++;
        if (taken == bi->scPred[br]){
            stats.condCorrect++;
        }else{
            stats.condWrong++;
        }
    }else{
        stats.tage_used++;
        if (taken == bi->predBeforeSC[br]){
            stats.condCorrect++;
        }else{
            stats.condWrong++;
        }
    }
}

New_SC_BLK::SCThreadHistory *
New_SC_BLK::makeThreadHistory()
{
    SCThreadHistory *sh = new SCThreadHistory();
    sh->setNumOrdinalHistories(3); //local history tables
    //init local history tables depth
    sh->initLocalHistory(1, numEntriesFirstLocalHistories, 2);
    sh->initLocalHistory(2, numEntriesSecondLocalHistories, 5);
    sh->initLocalHistory(3, numEntriesThirdLocalHistories, 3);
    return sh;
}

int New_SC_BLK::gIndexLogsSubstr(int nbr, int i)
{
    return 0;
}

unsigned
New_SC_BLK::getIndBiasBank(Addr branch_pc,
        BranchInfo* bi, int hitBank, int altBank, unsigned br)
{
    return (bi->predBeforeSC[br] + (((hitBank+1)/4)<<4) +
            (bi->highConf[br]<<1) +
            (bi->lowConf[br] <<2) +
            ((altBank!=0)<<3)) & ((1<<logBias) -1);
}

New_SC_BLK::New_SC_BLKStats::New_SC_BLKStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(sc_correct_tage_wrong, statistics::units::Count::get(),
               "sc is correct and tage is wrong"),
      ADD_STAT(sc_wrong_tage_correct, statistics::units::Count::get(),
               "sc is wrong and tage is correct"),
      ADD_STAT(sc_used, statistics::units::Count::get(),
               "use sc predictor"),
      ADD_STAT(tage_used, statistics::units::Count::get(),
               "use tage predictor"),
      ADD_STAT(num_update_cond_br, statistics::units::Count::get(),
               "num update cond br"),
      ADD_STAT(condCorrect, statistics::units::Count::get(),
               "num condCorrectr"),
      ADD_STAT(condWrong, statistics::units::Count::get(),
               "num condWrong")
{
}

}  // namespace branch_prediction
}  // namespace gem5

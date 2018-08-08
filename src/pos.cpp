// Copyright (c) 2014-2018 The Blackcoin developers
// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/* 
 * Stake cache by Qtum
 * Copyright (c) 2016-2018 The Qtum developers
 */

#include "pos.h"
#include "chain.h"
#include "chainparams.h"
#include "clientversion.h"
#include "coins.h"
#include "hash.h"
#include "uint256.h"
#include "primitives/transaction.h"
#include <stdio.h>
#include "util.h"
#include "config.h"
#include "policy/policy.h"

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel) {
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return ss.GetHash();
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx) {
    const Consensus::Params& params = Params().GetConsensus();
    if (params.IsProtocolV2(nTimeBlock))
        return (nTimeBlock == nTimeTx) && ((nTimeTx & params.nStakeTimestampMask) == 0);
    else
        return (nTimeBlock == nTimeTx);
}

// Simplified version of CheckCoinStakeTimestamp() to check header-only timestamp
bool CheckStakeBlockTimestamp(int64_t nTimeBlock) {
    return CheckCoinStakeTimestamp(nTimeBlock, nTimeBlock);
}

// BlackCoin kernel protocol v3
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits,
        unsigned int nBlockFromTime, Amount prevOutAmount,
        const COutPoint &prevout, unsigned int nTime) {

    if (nTime < nBlockFromTime)  // Transaction timestamp violation
        return error("%s: nTime violation", __func__);

    // Base target
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return error("%s: SetCompact failed", __func__);

    // Weighted target
    int64_t nValueIn = prevOutAmount.GetSatoshis();
    if (nValueIn == 0)
        return error("%s: nValueIn == 0", __func__);
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    uint256 nStakeModifier = pindexPrev->nStakeModifier;

    // Calculate hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << nBlockFromTime << prevout.GetHash() << prevout.GetN() << nTime;
    uint256 hashProofOfStake = ss.GetHash();

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return false;

    if (gArgs.IsArgSet("-debug")) {
        LogPrintf("%s: check modifier=%s nBlockFromTime=%u nPrevout=%u nTime=%u hashProof=%s\n", __func__, 
            nStakeModifier.GetHex().c_str(),
            nBlockFromTime, prevout.GetN(), nTime,
            hashProofOfStake.ToString());
    }

    return true;
}

bool IsConfirmedInNPrevBlocks(const CDiskTxPos& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth) {
    for (const CBlockIndex* pindex = pindexFrom; pindex && pindexFrom->nHeight - pindex->nHeight < nMaxDepth; pindex = pindex->pprev) {
            if (pindex->nDataPos == txindex.nPos && pindex->nFile == txindex.nFile) {
                nActualDepth = pindexFrom->nHeight - pindex->nHeight;
                return true;
            }
        }

    return false;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, uint32_t nTimeBlock, CCoinsViewCache& view, CValidationState &state) {
    if (!tx.IsCoinStake())
            return error("CheckProofOfStake(): called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target (nBits)
    const CTxIn& txin = tx.vin[0];

    Coin coinPrev;

    if (!view.GetCoin(txin.prevout, coinPrev))
        return state.DoS(100, error("CheckProofOfStake(): Stake prevout does not exist %s", txin.prevout.GetHash().ToString()));

    if (pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nStakeMinConfirmations)
        return state.DoS(100, error("CheckProofOfStake(): Stake prevout is not mature, expecting %i and only matured to %i", Params().GetConsensus().nStakeMinConfirmations, pindexPrev->nHeight + 1 - coinPrev.nHeight));

    // Verify script
    CScript kernelPubKey = coinPrev.out.scriptPubKey;
    ScriptError serror = SCRIPT_ERR_OK;
    if (!VerifyScript(txin.scriptSig, kernelPubKey, SCRIPT_VERIFY_NONE, TransactionSignatureChecker(&tx, 0, coinPrev.out.nValue), &serror))
        return state.DoS(100, error("CheckProofOfStake(): VerifyScript failed on coinstake %s", tx.GetHash().ToString()));

    // Check kernel
    if (!CheckStakeKernelHash(pindexPrev, nBits, coinPrev.nTime, coinPrev.out.nValue, txin.prevout, nTimeBlock))
        return state.DoS(1, error("CheckProofOfStake(): CheckStakeKernelHash failed on coinstake %s", tx.GetHash().ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, uint32_t* pBlockTime) {
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, pBlockTime, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTime, const COutPoint& prevout, uint32_t* pBlockTime, const std::map<COutPoint, CStakeCache>& cache) {
    auto it = cache.find(prevout);

    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);

    if (it == cache.end()) {
        Coin coinPrev;
        if (!view.GetCoin(prevout, coinPrev))
            return false;

        if(pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nStakeMinConfirmations)
            return false;

        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if (!blockFrom)
            return false;

        if(coinPrev.IsSpent())
            return false;

        return CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime, coinPrev.out.nValue, prevout, nTime);
    } else {
        //found in cache
        const CStakeCache& stake = it->second;

        if (pBlockTime)
            *pBlockTime = stake.blockFromTime;
        return CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake.amount, prevout, nTime);
    }

}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view) {
    if (cache.find(prevout) != cache.end()) {
        // already in cache
        return;
    }

    Coin coinPrev;
    if (!view.GetCoin(prevout, coinPrev))
        return;

    if (pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nStakeMinConfirmations)
        return;

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if (!blockFrom)
        return;

    CStakeCache c(blockFrom->nTime, coinPrev.out.nValue);
    cache.insert({prevout, c});
}


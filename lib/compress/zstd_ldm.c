/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */

#include "zstd_ldm.h"

#include "zstd_fast.h"          /* ZSTD_fillHashTable() */
#include "zstd_double_fast.h"   /* ZSTD_fillDoubleHashTable() */

#define LDM_BUCKET_SIZE_LOG 3
#define LDM_MIN_MATCH_LENGTH 64
#define LDM_HASH_RLOG 7
#define LDM_HASH_CHAR_OFFSET 10

size_t ZSTD_ldm_initializeParameters(ldmParams_t* params, U32 enableLdm)
{
    ZSTD_STATIC_ASSERT(LDM_BUCKET_SIZE_LOG <= ZSTD_LDM_BUCKETSIZELOG_MAX);
    params->enableLdm = enableLdm>0;
    params->hashLog = 0;
    params->bucketSizeLog = LDM_BUCKET_SIZE_LOG;
    params->minMatchLength = LDM_MIN_MATCH_LENGTH;
    params->hashEveryLog = ZSTD_LDM_HASHEVERYLOG_NOTSET;
    return 0;
}

void ZSTD_ldm_adjustParameters(ldmParams_t* params,
                               ZSTD_compressionParameters const* cParams)
{
    U32 const windowLog = cParams->windowLog;
    if (cParams->strategy >= ZSTD_btopt) {
      /* Get out of the way of the optimal parser */
      U32 const minMatch = MAX(cParams->targetLength, params->minMatchLength);
      assert(minMatch >= ZSTD_LDM_MINMATCH_MIN);
      assert(minMatch <= ZSTD_LDM_MINMATCH_MAX);
      params->minMatchLength = minMatch;
    }
    if (params->hashLog == 0) {
        params->hashLog = MAX(ZSTD_HASHLOG_MIN, windowLog - LDM_HASH_RLOG);
        assert(params->hashLog <= ZSTD_HASHLOG_MAX);
    }
    if (params->hashEveryLog == ZSTD_LDM_HASHEVERYLOG_NOTSET) {
        params->hashEveryLog =
                windowLog < params->hashLog ? 0 : windowLog - params->hashLog;
    }
    params->bucketSizeLog = MIN(params->bucketSizeLog, params->hashLog);
}

size_t ZSTD_ldm_getTableSize(ldmParams_t params)
{
    size_t const ldmHSize = ((size_t)1) << params.hashLog;
    size_t const ldmBucketSizeLog = MIN(params.bucketSizeLog, params.hashLog);
    size_t const ldmBucketSize =
        ((size_t)1) << (params.hashLog - ldmBucketSizeLog);
    size_t const totalSize = ldmBucketSize + ldmHSize * sizeof(ldmEntry_t);
    return params.enableLdm ? totalSize : 0;
}

size_t ZSTD_ldm_getMaxNbSeq(ldmParams_t params, size_t maxChunkSize)
{
    return params.enableLdm ? (maxChunkSize / params.minMatchLength) : 0;
}

/** ZSTD_ldm_getSmallHash() :
 *  numBits should be <= 32
 *  If numBits==0, returns 0.
 *  @return : the most significant numBits of value. */
static U32 ZSTD_ldm_getSmallHash(U64 value, U32 numBits)
{
    assert(numBits <= 32);
    return numBits == 0 ? 0 : (U32)(value >> (64 - numBits));
}

/** ZSTD_ldm_getChecksum() :
 *  numBitsToDiscard should be <= 32
 *  @return : the next most significant 32 bits after numBitsToDiscard */
static U32 ZSTD_ldm_getChecksum(U64 hash, U32 numBitsToDiscard)
{
    assert(numBitsToDiscard <= 32);
    return (hash >> (64 - 32 - numBitsToDiscard)) & 0xFFFFFFFF;
}

/** ZSTD_ldm_getTag() ;
 *  Given the hash, returns the most significant numTagBits bits
 *  after (32 + hbits) bits.
 *
 *  If there are not enough bits remaining, return the last
 *  numTagBits bits. */
static U32 ZSTD_ldm_getTag(U64 hash, U32 hbits, U32 numTagBits)
{
    assert(numTagBits < 32 && hbits <= 32);
    if (32 - hbits < numTagBits) {
        return hash & (((U32)1 << numTagBits) - 1);
    } else {
        return (hash >> (32 - hbits - numTagBits)) & (((U32)1 << numTagBits) - 1);
    }
}

/** ZSTD_ldm_getBucket() :
 *  Returns a pointer to the start of the bucket associated with hash. */
static ldmEntry_t* ZSTD_ldm_getBucket(
        ldmState_t* ldmState, size_t hash, ldmParams_t const ldmParams)
{
    return ldmState->hashTable + (hash << ldmParams.bucketSizeLog);
}

/** ZSTD_ldm_insertEntry() :
 *  Insert the entry with corresponding hash into the hash table */
static void ZSTD_ldm_insertEntry(ldmState_t* ldmState,
                                 size_t const hash, const ldmEntry_t entry,
                                 ldmParams_t const ldmParams)
{
    BYTE* const bucketOffsets = ldmState->bucketOffsets;
    *(ZSTD_ldm_getBucket(ldmState, hash, ldmParams) + bucketOffsets[hash]) = entry;
    bucketOffsets[hash]++;
    bucketOffsets[hash] &= ((U32)1 << ldmParams.bucketSizeLog) - 1;
}

/** ZSTD_ldm_makeEntryAndInsertByTag() :
 *
 *  Gets the small hash, checksum, and tag from the rollingHash.
 *
 *  If the tag matches (1 << ldmParams.hashEveryLog)-1, then
 *  creates an ldmEntry from the offset, and inserts it into the hash table.
 *
 *  hBits is the length of the small hash, which is the most significant hBits
 *  of rollingHash. The checksum is the next 32 most significant bits, followed
 *  by ldmParams.hashEveryLog bits that make up the tag. */
static void ZSTD_ldm_makeEntryAndInsertByTag(ldmState_t* ldmState,
                                             U64 const rollingHash,
                                             U32 const hBits,
                                             U32 const offset,
                                             ldmParams_t const ldmParams)
{
    U32 const tag = ZSTD_ldm_getTag(rollingHash, hBits, ldmParams.hashEveryLog);
    U32 const tagMask = ((U32)1 << ldmParams.hashEveryLog) - 1;
    if (tag == tagMask) {
        U32 const hash = ZSTD_ldm_getSmallHash(rollingHash, hBits);
        U32 const checksum = ZSTD_ldm_getChecksum(rollingHash, hBits);
        ldmEntry_t entry;
        entry.offset = offset;
        entry.checksum = checksum;
        ZSTD_ldm_insertEntry(ldmState, hash, entry, ldmParams);
    }
}

/** ZSTD_ldm_getRollingHash() :
 *  Get a 64-bit hash using the first len bytes from buf.
 *
 *  Giving bytes s = s_1, s_2, ... s_k, the hash is defined to be
 *  H(s) = s_1*(a^(k-1)) + s_2*(a^(k-2)) + ... + s_k*(a^0)
 *
 *  where the constant a is defined to be prime8bytes.
 *
 *  The implementation adds an offset to each byte, so
 *  H(s) = (s_1 + HASH_CHAR_OFFSET)*(a^(k-1)) + ... */
static U64 ZSTD_ldm_getRollingHash(const BYTE* buf, U32 len)
{
    U64 ret = 0;
    U32 i;
    for (i = 0; i < len; i++) {
        ret *= prime8bytes;
        ret += buf[i] + LDM_HASH_CHAR_OFFSET;
    }
    return ret;
}

/** ZSTD_ldm_ipow() :
 *  Return base^exp. */
static U64 ZSTD_ldm_ipow(U64 base, U64 exp)
{
    U64 ret = 1;
    while (exp) {
        if (exp & 1) { ret *= base; }
        exp >>= 1;
        base *= base;
    }
    return ret;
}

U64 ZSTD_ldm_getHashPower(U32 minMatchLength) {
    assert(minMatchLength >= ZSTD_LDM_MINMATCH_MIN);
    return ZSTD_ldm_ipow(prime8bytes, minMatchLength - 1);
}

/** ZSTD_ldm_updateHash() :
 *  Updates hash by removing toRemove and adding toAdd. */
static U64 ZSTD_ldm_updateHash(U64 hash, BYTE toRemove, BYTE toAdd, U64 hashPower)
{
    hash -= ((toRemove + LDM_HASH_CHAR_OFFSET) * hashPower);
    hash *= prime8bytes;
    hash += toAdd + LDM_HASH_CHAR_OFFSET;
    return hash;
}

/** ZSTD_ldm_countBackwardsMatch() :
 *  Returns the number of bytes that match backwards before pIn and pMatch.
 *
 *  We count only bytes where pMatch >= pBase and pIn >= pAnchor. */
static size_t ZSTD_ldm_countBackwardsMatch(
            const BYTE* pIn, const BYTE* pAnchor,
            const BYTE* pMatch, const BYTE* pBase)
{
    size_t matchLength = 0;
    while (pIn > pAnchor && pMatch > pBase && pIn[-1] == pMatch[-1]) {
        pIn--;
        pMatch--;
        matchLength++;
    }
    return matchLength;
}

/** ZSTD_ldm_fillFastTables() :
 *
 *  Fills the relevant tables for the ZSTD_fast and ZSTD_dfast strategies.
 *  This is similar to ZSTD_loadDictionaryContent.
 *
 *  The tables for the other strategies are filled within their
 *  block compressors. */
static size_t ZSTD_ldm_fillFastTables(ZSTD_matchState_t* ms,
                                      ZSTD_compressionParameters const* cParams,
                                      void const* end)
{
    const BYTE* const iend = (const BYTE*)end;

    switch(cParams->strategy)
    {
    case ZSTD_fast:
        ZSTD_fillHashTable(ms, cParams, iend);
        ms->nextToUpdate = (U32)(iend - ms->base);
        break;

    case ZSTD_dfast:
        ZSTD_fillDoubleHashTable(ms, cParams, iend);
        ms->nextToUpdate = (U32)(iend - ms->base);
        break;

    case ZSTD_greedy:
    case ZSTD_lazy:
    case ZSTD_lazy2:
    case ZSTD_btlazy2:
    case ZSTD_btopt:
    case ZSTD_btultra:
        break;
    default:
        assert(0);  /* not possible : not a valid strategy id */
    }

    return 0;
}

/** ZSTD_ldm_fillLdmHashTable() :
 *
 *  Fills hashTable from (lastHashed + 1) to iend (non-inclusive).
 *  lastHash is the rolling hash that corresponds to lastHashed.
 *
 *  Returns the rolling hash corresponding to position iend-1. */
static U64 ZSTD_ldm_fillLdmHashTable(ldmState_t* state,
                                     U64 lastHash, const BYTE* lastHashed,
                                     const BYTE* iend, const BYTE* base,
                                     U32 hBits, ldmParams_t const ldmParams)
{
    U64 rollingHash = lastHash;
    const BYTE* cur = lastHashed + 1;

    while (cur < iend) {
        rollingHash = ZSTD_ldm_updateHash(rollingHash, cur[-1],
                                          cur[ldmParams.minMatchLength-1],
                                          state->hashPower);
        ZSTD_ldm_makeEntryAndInsertByTag(state,
                                         rollingHash, hBits,
                                         (U32)(cur - base), ldmParams);
        ++cur;
    }
    return rollingHash;
}


/** ZSTD_ldm_limitTableUpdate() :
 *
 *  Sets cctx->nextToUpdate to a position corresponding closer to anchor
 *  if it is far way
 *  (after a long match, only update tables a limited amount). */
static void ZSTD_ldm_limitTableUpdate(ZSTD_matchState_t* ms, const BYTE* anchor)
{
    U32 const current = (U32)(anchor - ms->base);
    if (current > ms->nextToUpdate + 1024) {
        ms->nextToUpdate =
            current - MIN(512, current - ms->nextToUpdate - 1024);
    }
}

size_t ZSTD_ldm_generateSequences(
        ldmState_t* ldmState, rawSeq* sequences, ZSTD_matchState_t const* ms,
        ldmParams_t const* params, void const* src, size_t srcSize,
        int const extDict)
{
    rawSeq const* const sequencesStart = sequences;
    /* LDM parameters */
    U32 const minMatchLength = params->minMatchLength;
    U64 const hashPower = ldmState->hashPower;
    U32 const hBits = params->hashLog - params->bucketSizeLog;
    U32 const ldmBucketSize = 1U << params->bucketSizeLog;
    U32 const hashEveryLog = params->hashEveryLog;
    U32 const ldmTagMask = (1U << params->hashEveryLog) - 1;
    /* Prefix and extDict parameters */
    U32 const dictLimit = ms->dictLimit;
    U32 const lowestIndex = extDict ? ms->lowLimit : dictLimit;
    BYTE const* const base = ms->base;
    BYTE const* const dictBase = extDict ? ms->dictBase : NULL;
    BYTE const* const dictStart = extDict ? dictBase + lowestIndex : NULL;
    BYTE const* const dictEnd = extDict ? dictBase + dictLimit : NULL;
    BYTE const* const lowPrefixPtr = base + dictLimit;
    /* Input bounds */
    BYTE const* const istart = (BYTE const*)src;
    BYTE const* const iend = istart + srcSize;
    BYTE const* const ilimit = iend - MAX(minMatchLength, HASH_READ_SIZE);
    /* Input positions */
    BYTE const* anchor = istart;
    BYTE const* ip = istart;
    /* Rolling hash */
    BYTE const* lastHashed = NULL;
    U64 rollingHash = 0;

    while (ip <= ilimit) {
        size_t mLength;
        U32 const current = (U32)(ip - base);
        size_t forwardMatchLength = 0, backwardMatchLength = 0;
        ldmEntry_t* bestEntry = NULL;
        if (ip != istart) {
            rollingHash = ZSTD_ldm_updateHash(rollingHash, lastHashed[0],
                                              lastHashed[minMatchLength],
                                              hashPower);
        } else {
            rollingHash = ZSTD_ldm_getRollingHash(ip, minMatchLength);
        }
        lastHashed = ip;

        /* Do not insert and do not look for a match */
        if (ZSTD_ldm_getTag(rollingHash, hBits, hashEveryLog) != ldmTagMask) {
           ip++;
           continue;
        }

        /* Get the best entry and compute the match lengths */
        {
            ldmEntry_t* const bucket =
                ZSTD_ldm_getBucket(ldmState,
                                   ZSTD_ldm_getSmallHash(rollingHash, hBits),
                                   *params);
            ldmEntry_t* cur;
            size_t bestMatchLength = 0;
            U32 const checksum = ZSTD_ldm_getChecksum(rollingHash, hBits);

            for (cur = bucket; cur < bucket + ldmBucketSize; ++cur) {
                size_t curForwardMatchLength, curBackwardMatchLength,
                       curTotalMatchLength;
                if (cur->checksum != checksum || cur->offset <= lowestIndex) {
                    continue;
                }
                if (extDict) {
                    BYTE const* const curMatchBase =
                        cur->offset < dictLimit ? dictBase : base;
                    BYTE const* const pMatch = curMatchBase + cur->offset;
                    BYTE const* const matchEnd =
                        cur->offset < dictLimit ? dictEnd : iend;
                    BYTE const* const lowMatchPtr =
                        cur->offset < dictLimit ? dictStart : lowPrefixPtr;

                    curForwardMatchLength = ZSTD_count_2segments(
                                                ip, pMatch, iend,
                                                matchEnd, lowPrefixPtr);
                    if (curForwardMatchLength < minMatchLength) {
                        continue;
                    }
                    curBackwardMatchLength =
                        ZSTD_ldm_countBackwardsMatch(ip, anchor, pMatch,
                                                     lowMatchPtr);
                    curTotalMatchLength = curForwardMatchLength +
                                          curBackwardMatchLength;
                } else { /* !extDict */
                    BYTE const* const pMatch = base + cur->offset;
                    curForwardMatchLength = ZSTD_count(ip, pMatch, iend);
                    if (curForwardMatchLength < minMatchLength) {
                        continue;
                    }
                    curBackwardMatchLength =
                        ZSTD_ldm_countBackwardsMatch(ip, anchor, pMatch,
                                                     lowPrefixPtr);
                    curTotalMatchLength = curForwardMatchLength +
                                          curBackwardMatchLength;
                }

                if (curTotalMatchLength > bestMatchLength) {
                    bestMatchLength = curTotalMatchLength;
                    forwardMatchLength = curForwardMatchLength;
                    backwardMatchLength = curBackwardMatchLength;
                    bestEntry = cur;
                }
            }
        }

        /* No match found -- continue searching */
        if (bestEntry == NULL) {
            ZSTD_ldm_makeEntryAndInsertByTag(ldmState, rollingHash,
                                             hBits, current,
                                             *params);
            ip++;
            continue;
        }

        /* Match found */
        mLength = forwardMatchLength + backwardMatchLength;
        ip -= backwardMatchLength;

        {
            /* Store the sequence:
             * ip = current - backwardMatchLength
             * The match is at (bestEntry->offset - backwardMatchLength)
             */
            U32 const matchIndex = bestEntry->offset;
            U32 const offset = current - matchIndex;

            sequences->litLength = (U32)(ip - anchor);
            sequences->matchLength = (U32)mLength;
            sequences->offset = offset;
            ++sequences;
        }

        /* Insert the current entry into the hash table */
        ZSTD_ldm_makeEntryAndInsertByTag(ldmState, rollingHash, hBits,
                                         (U32)(lastHashed - base),
                                         *params);

        assert(ip + backwardMatchLength == lastHashed);

        /* Fill the hash table from lastHashed+1 to ip+mLength*/
        /* Heuristic: don't need to fill the entire table at end of block */
        if (ip + mLength <= ilimit) {
            rollingHash = ZSTD_ldm_fillLdmHashTable(
                              ldmState, rollingHash, lastHashed,
                              ip + mLength, base, hBits, *params);
            lastHashed = ip + mLength - 1;
        }
        ip += mLength;
        anchor = ip;
    }
    /* Return the number of sequences generated */
    return sequences - sequencesStart;
}

#if 0
/**
 * If the sequence length is longer than remaining then the sequence is split
 * between this block and the next.
 *
 * Returns the current sequence to handle, or if the rest of the block should
 * be literals, it returns a sequence with offset == 0.
 */
static rawSeq maybeSplitSequence(rawSeq* sequences, size_t* nbSeq,
                                 size_t const seq, size_t const remaining,
                                 U32 const minMatch)
{
    rawSeq sequence = sequences[seq];
    assert(sequence.offset > 0);
    /* Handle partial sequences */
    if (remaining <= sequence.litLength) {
        /* Split the literals that we have out of the sequence.
         * They will become the last literals of this block.
         * The next block starts off with the remaining literals.
         */
        sequences[seq].litLength -= remaining;
        *nbSeq = seq;
        sequence.offset = 0;
    } else if (remaining < sequence.litLength + sequence.matchLength) {
        /* Split the match up into two sequences. One in this block, and one
         * in the next with no literals. If either match would be shorter
         * than searchLength we omit it.
         */
        U32 const matchPrefix = remaining - sequence.litLength;
        U32 const matchSuffix = sequence.matchLength - matchPrefix;

        assert(remaining > sequence.litLength);
        assert(matchPrefix < sequence.matchLength);
        assert(matchPrefix + matchSuffix == sequence.matchLength);
        /* Update the current sequence */
        sequence.matchLength = matchPrefix;
        /* Update the next sequence when long enough, otherwise omit it. */
        if (matchSuffix >= minMatch) {
            sequences[seq].litLength = 0;
            sequences[seq].matchLength = matchSuffix;
            *nbSeq = seq;
        } else {
            sequences[seq + 1].litLength += matchSuffix;
            *nbSeq = seq + 1;
        }
        if (sequence.matchLength < minMatch) {
            /* Skip the current sequence if it is too short */
            sequence.offset = 0;
        }
    }
    return sequence;
}
#endif

size_t ZSTD_ldm_blockCompress(rawSeq const* sequences, size_t nbSeq,
    ZSTD_matchState_t* ms, seqStore_t* seqStore, U32 rep[ZSTD_REP_NUM],
    ZSTD_compressionParameters const* cParams, void const* src, size_t srcSize,
    int const extDict)
{
    ZSTD_blockCompressor const blockCompressor =
        ZSTD_selectBlockCompressor(cParams->strategy, extDict);
    BYTE const* const base = ms->base;
    /* Input bounds */
    BYTE const* const istart = (BYTE const*)src;
    BYTE const* const iend = istart + srcSize;
    /* Input positions */
    BYTE const* ip = istart;
    size_t seq;
    /* Loop through each sequence and apply the block compressor to the lits */
    for (seq = 0; seq < nbSeq; ++seq) {
        rawSeq const sequence = sequences[seq];
        int i;

        if (sequence.offset == 0)
            break;

        assert(ip + sequence.litLength + sequence.matchLength <= iend);

        /* Fill tables for block compressor */
        ZSTD_ldm_limitTableUpdate(ms, ip);
        ZSTD_ldm_fillFastTables(ms, cParams, ip);
        /* Run the block compressor */
        {
            size_t const newLitLength =
                blockCompressor(ms, seqStore, rep, cParams, ip,
                                sequence.litLength);
            ip += sequence.litLength;
            ms->nextToUpdate = (U32)(ip - base);
            /* Update the repcodes */
            for (i = ZSTD_REP_NUM - 1; i > 0; i--)
                rep[i] = rep[i-1];
            rep[0] = sequence.offset;
            /* Store the sequence */
            ZSTD_storeSeq(seqStore, newLitLength, ip - newLitLength,
                          sequence.offset + ZSTD_REP_MOVE,
                          sequence.matchLength - MINMATCH);
            ip += sequence.matchLength;
        }
    }
    ZSTD_ldm_limitTableUpdate(ms, ip);
    ZSTD_ldm_fillFastTables(ms, cParams, ip);
    {
        size_t const lastLiterals = blockCompressor(ms, seqStore, rep, cParams,
                                                    ip, iend - ip);
        ms->nextToUpdate = (U32)(iend - base);
        return lastLiterals;
    }
}

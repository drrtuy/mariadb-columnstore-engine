/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2016-2020 MariaDB Corporation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

//
// $Id: batchprimitiveprocessor.cpp 2136 2013-07-24 21:04:30Z pleblanc $
// C++ Implementation: batchprimitiveprocessor
//
// Description:
//
//
// Author: Patrick LeBlanc <pleblanc@calpont.com>, (C) 2008
//
// Copyright: See COPYING file that comes with this distribution
//
//

#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <cstring>
// #define NDEBUG
#include <cassert>
#include <string>
#include <sstream>
#include <set>
#include "rowgroup.h"
#include "serviceexemgr.h"
#include <cstdlib>
using namespace std;

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
using namespace boost;

#include "bpp.h"
#include "primitiveserver.h"
#include "errorcodes.h"
#include "exceptclasses.h"
#include "pp_logger.h"
#include "funcexpwrapper.h"
#include "fixedallocator.h"
#include "blockcacheclient.h"
#include "MonitorProcMem.h"
#include "threadnaming.h"
#include "vlarray.h"
#include "widedecimalutils.h"

#define MAX64 0x7fffffffffffffffLL
#define MIN64 0x8000000000000000LL

using namespace messageqcpp;
using namespace joiner;
using namespace std::tr1;
using namespace rowgroup;
using namespace funcexp;
using namespace logging;
using namespace utils;
using namespace joblist;

namespace primitiveprocessor
{
#ifdef PRIMPROC_STOPWATCH
#include "poormanprofiler.inc"
#endif

// these are config parms defined in primitiveserver.cpp, initialized by PrimProc main().
extern uint32_t blocksReadAhead;
extern uint32_t dictBufferSize;
extern uint32_t defaultBufferSize;
extern int fCacheCount;
extern uint32_t connectionsPerUM;
extern int noVB;

// copied from https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
uint nextPowOf2(uint x)
{
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;
  return x;
}

BatchPrimitiveProcessor::BatchPrimitiveProcessor()
 : ot(BPS_ELEMENT_TYPE)
 , txnID(0)
 , sessionID(0)
 , stepID(0)
 , uniqueID(0)
 , count(1)
 , baseRid(0)
 , ridCount(0)
 , needStrValues(false)
 , wideColumnsWidths(0)
 , filterCount(0)
 , projectCount(0)
 , sendRidsAtDelivery(false)
 , ridMap(0)
 , gotAbsRids(false)
 , gotValues(false)
 , hasScan(false)
 , validCPData(false)
 , minVal(MAX64)
 , maxVal(MIN64)
 , cpDataFromDictScan(false)
 , lbidForCP(0)
 , hasWideColumnOut(false)
 , busyLoaderCount(0)
 , physIO(0)
 , cachedIO(0)
 , touchedBlocks(0)
 , LBIDTrace(false)
 , fBusy(false)
 , doJoin(false)
 , hasFilterStep(false)
 , filtOnString(false)
 , prefetchThreshold(0)
 , mJOINHasSkewedKeyColumn(false)
 , mSmallSideRGPtr(nullptr)
 , mSmallSideKeyColumnsPtr(nullptr)
 , hasDictStep(false)
 , sockIndex(0)
 , endOfJoinerRan(false)
 , processorThreads(0)
 , ptMask(0)
 , firstInstance(false)
 , valuesLBID(0)
 , initiatedByEM_(false)
 , weight_(0)
{
  pp.setLogicalBlockMode(true);
  pp.setBlockPtr((int*)blockData);
  pp.setBlockPtrAux((int*)blockDataAux);
  pthread_mutex_init(&objLock, nullptr);
}

BatchPrimitiveProcessor::BatchPrimitiveProcessor(ByteStream& b, double prefetch,
                                                 boost::shared_ptr<BPPSendThread> bppst,
                                                 uint _processorThreads)
 : ot(BPS_ELEMENT_TYPE)
 , txnID(0)
 , sessionID(0)
 , stepID(0)
 , uniqueID(0)
 , count(1)
 , baseRid(0)
 , ridCount(0)
 , needStrValues(false)
 , wideColumnsWidths(0)
 , filterCount(0)
 , projectCount(0)
 , sendRidsAtDelivery(false)
 , ridMap(0)
 , gotAbsRids(false)
 , gotValues(false)
 , hasScan(false)
 , validCPData(false)
 , minVal(MAX64)
 , maxVal(MIN64)
 , cpDataFromDictScan(false)
 , lbidForCP(0)
 , hasWideColumnOut(false)
 , busyLoaderCount(0)
 , physIO(0)
 , cachedIO(0)
 , touchedBlocks(0)
 , LBIDTrace(false)
 , fBusy(false)
 , doJoin(false)
 , hasFilterStep(false)
 , filtOnString(false)
 , prefetchThreshold(prefetch)
 , mJOINHasSkewedKeyColumn(false)
 , mSmallSideRGPtr(nullptr)
 , mSmallSideKeyColumnsPtr(nullptr)
 , hasDictStep(false)
 , sockIndex(0)
 , endOfJoinerRan(false)
 , processorThreads(_processorThreads)
 // processorThreads(32),
 // ptMask(processorThreads - 1),
 , firstInstance(true)
 , valuesLBID(0)
 , initiatedByEM_(false)
 , weight_(0)
{
  // promote processorThreads to next power of 2.  also need to change the name to bucketCount or similar
  processorThreads = nextPowOf2(processorThreads);
  ptMask = processorThreads - 1;

  pp.setLogicalBlockMode(true);
  pp.setBlockPtr((int*)blockData);
  pp.setBlockPtrAux((int*)blockDataAux);
  sendThread = bppst;
  pthread_mutex_init(&objLock, nullptr);
  initBPP(b);
}

BatchPrimitiveProcessor::~BatchPrimitiveProcessor()
{
  // FIXME: just do a sync fetch
  counterLock.lock();  // need to make sure the loader has exited

  while (busyLoaderCount > 0)
  {
    counterLock.unlock();
    usleep(100000);
    counterLock.lock();
  }

  counterLock.unlock();
  pthread_mutex_destroy(&objLock);
}

/**
 * InitBPP Parses the creation messages from BatchPrimitiveProcessor-JL::createBPP()
 * Refer to that fcn for message format info.
 */

void BatchPrimitiveProcessor::initBPP(ByteStream& bs)
{
  uint32_t i;
  uint8_t tmp8;
  uint16_t tmp16;
  Command::CommandType type;

  bs.advance(sizeof(ISMPacketHeader));  // skip the header
  bs >> tmp8;
  ot = static_cast<BPSOutputType>(tmp8);
  bs >> txnID;
  bs >> sessionID;
  bs >> stepID;
  bs >> uniqueID;
  bs >> versionInfo;

  bs >> tmp16;
  needStrValues = tmp16 & NEED_STR_VALUES;
  gotAbsRids = tmp16 & GOT_ABS_RIDS;
  gotValues = tmp16 & GOT_VALUES;
  LBIDTrace = tmp16 & LBID_TRACE;
  sendRidsAtDelivery = tmp16 & SEND_RIDS_AT_DELIVERY;
  doJoin = tmp16 & HAS_JOINER;
  hasRowGroup = tmp16 & HAS_ROWGROUP;
  getTupleJoinRowGroupData = tmp16 & JOIN_ROWGROUP_DATA;
  bool hasWideColumnsIn = tmp16 & HAS_WIDE_COLUMNS;

  // This used to signify that there was input row data from previous jobsteps, and
  // it never quite worked right. No need to fix it or update it; all BPP's have started
  // with a scan for years.  Took it out.
  assert(!hasRowGroup);

  if (hasWideColumnsIn)
    bs >> wideColumnsWidths;

  bs >> bop;
  bs >> forHJ;

  if (ot == ROW_GROUP)
  {
    bs >> outputRG;
    bs >> tmp8;

    if (tmp8)
    {
      fe1.reset(new FuncExpWrapper());
      bs >> *fe1;
      bs >> fe1Input;
    }

    bs >> tmp8;

    if (tmp8)
    {
      fe2.reset(new FuncExpWrapper());
      bs >> *fe2;
      bs >> fe2Output;
    }
  }

  if (doJoin)
  {
    pthread_mutex_lock(&objLock);
    bs >> maxPmJoinResultCount;
    maxPmJoinResultCount = std::max(maxPmJoinResultCount, (uint32_t)1);

    if (ot == ROW_GROUP)
    {
      bs >> joinerCount;
      joinTypes.reset(new JoinType[joinerCount]);

      tJoiners.reset(new std::shared_ptr<boost::shared_ptr<TJoiner>[]>[joinerCount]);
      for (uint j = 0; j < joinerCount; ++j)
        tJoiners[j].reset(new boost::shared_ptr<TJoiner>[processorThreads]);

      tlJoiners.reset(new std::shared_ptr<boost::shared_ptr<TLJoiner>[]>[joinerCount]);
      for (uint j = 0; j < joinerCount; ++j)
        tlJoiners[j].reset(new boost::shared_ptr<TLJoiner>[processorThreads]);

      addToJoinerLocks.reset(new boost::scoped_array<boost::mutex>[joinerCount]);
      for (uint j = 0; j < joinerCount; ++j)
        addToJoinerLocks[j].reset(new boost::mutex[processorThreads]);

      smallSideDataLocks.reset(new boost::mutex[joinerCount]);
      tJoinerSizes.reset(new std::atomic<uint32_t>[joinerCount]);
      largeSideKeyColumns.reset(new uint32_t[joinerCount]);
      tlLargeSideKeyColumns.reset(new vector<uint32_t>[joinerCount]);
      tlSmallSideKeyColumns.reset(new std::vector<uint32_t>);
      typelessJoin.reset(new bool[joinerCount]);
      tlSmallSideKeyLengths.reset(new uint32_t[joinerCount]);

      auto alloc = exemgr::globServiceExeMgr->getRm().getAllocator<utils::PoolAllocatorBufType>();
      for (uint j = 0; j < joinerCount; ++j)
      {
        storedKeyAllocators.emplace_back(PoolAllocator(alloc, PoolAllocator::DEFAULT_WINDOW_SIZE, false,
        true));
      }

      joinNullValues.reset(new uint64_t[joinerCount]);
      doMatchNulls.reset(new bool[joinerCount]);
      joinFEFilters.reset(new scoped_ptr<FuncExpWrapper>[joinerCount]);
      hasJoinFEFilters = false;
      hasSmallOuterJoin = false;
      bool smallSideRGRecvd = false;
      auto* resourceManager = &exemgr::globServiceExeMgr->getRm();

      for (i = 0; i < joinerCount; i++)
      {
        doMatchNulls[i] = false;
        uint32_t tmp32;
        bs >> tmp32;
        tJoinerSizes[i] = tmp32;
        bs >> joinTypes[i];
        bs >> tmp8;
        typelessJoin[i] = (bool)tmp8;

        if (joinTypes[i] & WITHFCNEXP)
        {
          hasJoinFEFilters = true;
          joinFEFilters[i].reset(new FuncExpWrapper());
          bs >> *joinFEFilters[i];
        }

        if (joinTypes[i] & SMALLOUTER)
          hasSmallOuterJoin = true;

        if (!typelessJoin[i])
        {
          bs >> joinNullValues[i];
          bs >> largeSideKeyColumns[i];
          for (uint j = 0; j < processorThreads; ++j)
          tJoiners[i][j].reset(new TJoiner(10, TupleJoiner::hasher(), std::equal_to<uint64_t>(),
                                            utils::STLPoolAllocator<TJoiner::value_type>(resourceManager)));
        }
        else
        {
          deserializeVector<uint32_t>(bs, tlLargeSideKeyColumns[i]);
          bs >> tlSmallSideKeyLengths[i];
          bs >> tmp8;
          mJOINHasSkewedKeyColumn = (bool)tmp8;
          // Deser smallSideRG if key data types are different, e.g. INT vs wide-DECIMAL.
          if (mJOINHasSkewedKeyColumn && !smallSideRGRecvd)
          {
            smallSideRGs.emplace_back(rowgroup::RowGroup(bs));
            // LargeSide key columns number equals to SmallSide key columns number.
            deserializeVector<uint32_t>(bs, *tlSmallSideKeyColumns);
            mSmallSideRGPtr = &smallSideRGs[0];
            mSmallSideKeyColumnsPtr = &(*tlSmallSideKeyColumns);
            smallSideRGRecvd = true;
          }

          for (uint j = 0; j < processorThreads; ++j)
          {
            auto tlHasher = TupleJoiner::TypelessDataHasher(&outputRG, &tlLargeSideKeyColumns[i],
                                                            mSmallSideKeyColumnsPtr, mSmallSideRGPtr);
            auto tlComparator = TupleJoiner::TypelessDataComparator(&outputRG, &tlLargeSideKeyColumns[i],
                                                                    mSmallSideKeyColumnsPtr, mSmallSideRGPtr);
            tlJoiners[i][j].reset(new TLJoiner(10, tlHasher, tlComparator,
                                               utils::STLPoolAllocator<TLJoiner::value_type>(resourceManager)));
          }
        }
      }

      if (hasJoinFEFilters)
      {
        joinFERG.reset(new RowGroup());
        bs >> *joinFERG;
      }

      if (getTupleJoinRowGroupData)
      {
        deserializeVector(bs, smallSideRGs);
        idbassert(smallSideRGs.size() == joinerCount);
        smallSideRowLengths.reset(new uint32_t[joinerCount]);
        smallSideRowData.reset(new RGData[joinerCount]);
        smallNullRowData.reset(new RGData[joinerCount]);
        smallNullPointers.reset(new Row::Pointer[joinerCount]);
        ssrdPos.reset(new uint64_t[joinerCount]);

        for (i = 0; i < joinerCount; i++)
        {
          smallSideRowLengths[i] = smallSideRGs[i].getRowSize();
          smallSideRowData[i] = RGData(smallSideRGs[i], tJoinerSizes[i]);
          smallSideRGs[i].setData(&smallSideRowData[i]);
          smallSideRGs[i].resetRowGroup(0);
          ssrdPos[i] = smallSideRGs[i].getEmptySize();

          if (joinTypes[i] & (LARGEOUTER | SEMI | ANTI))
          {
            Row smallRow;
            smallSideRGs[i].initRow(&smallRow);
            smallNullRowData[i] = RGData(smallSideRGs[i], 1);
            smallSideRGs[i].setData(&smallNullRowData[i]);
            smallSideRGs[i].getRow(0, &smallRow);
            smallRow.initToNull();
            smallNullPointers[i] = smallRow.getPointer();
            smallSideRGs[i].setData(&smallSideRowData[i]);
          }
        }

        bs >> largeSideRG;
        bs >> joinedRG;
      }
    }

    pthread_mutex_unlock(&objLock);
  }

  bs >> filterCount;
  filterSteps.resize(filterCount);
  hasScan = false;
  hasPassThru = false;

  for (i = 0; i < filterCount; ++i)
  {
    filterSteps[i] = SCommand(Command::makeCommand(bs, &type, filterSteps));

    if (type == Command::COLUMN_COMMAND)
    {
      ColumnCommand* col = (ColumnCommand*)filterSteps[i].get();

      if (col->isScan())
        hasScan = true;

      if (bop == BOP_OR)
        col->setScan(true);
    }
    else if (type == Command::FILTER_COMMAND)
    {
      hasFilterStep = true;

      if (dynamic_cast<StrFilterCmd*>(filterSteps[i].get()) != nullptr)
        filtOnString = true;
    }
    else if (type == Command::DICT_STEP || type == Command::RID_TO_STRING)
      hasDictStep = true;
  }

  bs >> projectCount;
  projectSteps.resize(projectCount);

  for (i = 0; i < projectCount; ++i)
  {
    projectSteps[i] = SCommand(Command::makeCommand(bs, &type, projectSteps));

    if (type == Command::PASS_THRU)
      hasPassThru = true;
    else if (type == Command::DICT_STEP || type == Command::RID_TO_STRING)
      hasDictStep = true;
  }

  if (ot == ROW_GROUP)
  {
    bs >> tmp8;

    if (tmp8 > 0)
    {
      bs >> fAggregateRG;
      fAggregator.reset(new RowAggregation);
      bs >> *(fAggregator.get());

      // If there's UDAF involved, set up for PM processing
      for (const auto& pcol : fAggregator->getAggFunctions())
      {
        auto* rowUDAF = dynamic_cast<RowUDAFFunctionCol*>(pcol.get());

        if (rowUDAF)
        {
          // On the PM, the aux column is not sent, but rather is output col + 1.
          rowUDAF->fAuxColumnIndex = rowUDAF->fOutputColumnIndex + 1;
          // Set the PM flag in case the UDAF cares.
          rowUDAF->fUDAFContext.setContextFlags(rowUDAF->fUDAFContext.getContextFlags() |
                                                mcsv1sdk::CONTEXT_IS_PM);
        }
      }
    }
  }

  initProcessor();
}

/**
 * resetBPP Parses the run messages from BatchPrimitiveProcessor-JL::runBPP()
 * Refer to that fcn for message format info.
 */
void BatchPrimitiveProcessor::resetBPP(ByteStream& bs, const SP_UM_MUTEX& w, const SP_UM_IOSOCK& s)
{
  uint32_t i;
  vector<uint64_t> preloads;

  pthread_mutex_lock(&objLock);

  writelock = w;
  sock = s;
  newConnection = true;

  // skip the header, sessionID, stepID, uniqueID, and priority
  bs.advance(sizeof(ISMPacketHeader) + 16);
  bs >> weight_;
  bs >> dbRoot;
  bs >> count;
  uint8_t u8 = 0;
  bs >> u8;
  initiatedByEM_ = u8;

  bs >> ridCount;

  if (gotAbsRids)
  {
    assert(0);
    memcpy(absRids.get(), bs.buf(), ridCount << 3);
    bs.advance(ridCount << 3);
    /* TODO: this loop isn't always necessary or sensible */
    ridMap = 0;
    baseRid = absRids[0] & 0xffffffffffffe000ULL;

    for (uint32_t j = 0; j < ridCount; j++)
    {
      relRids[j] = absRids[j] - baseRid;
      ridMap |= 1 << (relRids[j] >> 9);
    }
  }
  else
  {
    bs >> ridMap;
    bs >> baseRid;
    memcpy(relRids, bs.buf(), ridCount << 1);
    bs.advance(ridCount << 1);
  }

  if (gotValues)
  {
    memcpy(values, bs.buf(), ridCount << 3);
    bs.advance(ridCount << 3);
  }

  for (i = 0; i < filterCount; ++i)
  {
    filterSteps[i]->resetCommand(bs);
  }

  for (i = 0; i < projectCount; ++i)
  {
    projectSteps[i]->resetCommand(bs);
  }

  idbassert(bs.empty());

  /* init vars not part of the BS */
  currentBlockOffset = 0;
  memset(relLBID.get(), 0, sizeof(uint64_t) * (projectCount + 2));
  memset(asyncLoaded.get(), 0, sizeof(bool) * (projectCount + 2));

  buildVSSCache(count);
  pthread_mutex_unlock(&objLock);
}

// This version of addToJoiner() is multithreaded.  Values are first
// hashed into thread-local vectors corresponding to the shared hash
// tables.  Once the incoming values are organized locally, it grabs
// the lock for each shared table and inserts them there.
void BatchPrimitiveProcessor::addToJoiner(ByteStream& bs)
{
  /*  to get wall-time of hash table construction
      idbassert(processorThreads != 0);
      if (firstCallTime.is_not_a_date_time())
          firstCallTime = boost::posix_time::microsec_clock::universal_time();
  */

  uint32_t count, i, joinerNum, tlIndex, startPos, bucket;
#pragma pack(push, 1)
  struct JoinerElements
  {
    uint64_t key;
    uint32_t value;
  }* arr;
#pragma pack(pop)

  /* skip the header */
  bs.advance(sizeof(ISMPacketHeader) + 3 * sizeof(uint32_t));

  bs >> count;
  bs >> startPos;

  if (ot == ROW_GROUP)
  {
    bs >> joinerNum;
    idbassert(joinerNum < joinerCount);
    arr = (JoinerElements*)bs.buf();

    std::atomic<uint32_t>& tJoinerSize = tJoinerSizes[joinerNum];

    // XXXPAT: enormous if stmts are evil.  TODO: move each block into
    // properly-named functions for clarity.
    if (typelessJoin[joinerNum])
    {
      utils::VLArray<vector<pair<TypelessData, uint32_t> > > tmpBuckets(processorThreads);
      uint8_t nullFlag;
      PoolAllocator& storedKeyAllocator = storedKeyAllocators[joinerNum];
      // this first loop hashes incoming values into vectors that parallel the hash tables.
      uint nullCount = 0;
      for (i = 0; i < count; ++i)
      {
        bs >> nullFlag;
        if (nullFlag == 0)
        {
          TypelessData tlSmallSideKey(bs, storedKeyAllocator);
          if (mJOINHasSkewedKeyColumn)
            tlSmallSideKey.setSmallSideWithSkewedData();
          else
            tlSmallSideKey.setSmallSide();
          bs >> tlIndex;
          // The bucket number corresponds with the index used later inserting TL keys into permanent JOIN
          // hash map.
          auto ha = tlSmallSideKey.hash(outputRG, tlLargeSideKeyColumns[joinerNum], mSmallSideKeyColumnsPtr,
                                        mSmallSideRGPtr);

          bucket = ha & ptMask;
          tmpBuckets[bucket].push_back(make_pair(tlSmallSideKey, tlIndex));
        }
        else
          ++nullCount;
      }
      tJoinerSize -= nullCount;

      bool done = false, didSomeWork;
      // uint loopCounter = 0, noWorkCounter = 0;
      // this loop moves the elements from each vector into its corresponding hash table.
      while (!done)
      {
        //++loopCounter;
        done = true;
        didSomeWork = false;
        for (i = 0; i < processorThreads; ++i)
        {
          if (!tmpBuckets[i].empty())
          {
            bool gotIt = addToJoinerLocks[joinerNum][i].try_lock();
            if (!gotIt)
            {
              done = false;  // didn't get it, don't block, try the next bucket
              continue;
            }
            for (auto& element : tmpBuckets[i])
              tlJoiners[joinerNum][i]->insert(element);
            addToJoinerLocks[joinerNum][i].unlock();
            tmpBuckets[i].clear();
            didSomeWork = true;
          }
        }
        // if this iteration did no useful work, everything we need is locked; wait briefly
        // and try again.
        if (!done && !didSomeWork)
        {
          ::usleep(500 * processorThreads);
          //++noWorkCounter;
        }
      }
      // cout << "TL join insert.  Took " << loopCounter << " loops" << endl;
    }
    else
    {
      std::shared_ptr<boost::shared_ptr<TJoiner>[]> tJoiner = tJoiners[joinerNum];
      uint64_t nullValue = joinNullValues[joinerNum];
      bool& l_doMatchNulls = doMatchNulls[joinerNum];
      joblist::JoinType joinType = joinTypes[joinerNum];
      utils::VLArray<vector<pair<uint64_t, uint32_t> > > tmpBuckets(processorThreads);

      if (joinType & MATCHNULLS)
      {
        // this first loop hashes incoming values into vectors that parallel the hash tables.
        for (i = 0; i < count; ++i)
        {
          /* A minor optimization: the matchnull logic should only be used with
           * the jointype specifies it and there's a null value in the small side */
          if (!l_doMatchNulls && arr[i].key == nullValue)
            l_doMatchNulls = true;
          bucket = bucketPicker((char*)&arr[i].key, 8, bpSeed) & ptMask;
          tmpBuckets[bucket].push_back(make_pair(arr[i].key, arr[i].value));
        }

        bool done = false, didSomeWork;
        // uint loopCounter = 0, noWorkCounter = 0;
        // this loop moves the elements from each vector into its corresponding hash table.
        while (!done)
        {
          //++loopCounter;
          done = true;
          didSomeWork = false;
          for (i = 0; i < processorThreads; ++i)
          {
            if (!tmpBuckets[i].empty())
            {
              bool gotIt = addToJoinerLocks[joinerNum][i].try_lock();
              if (!gotIt)
              {
                done = false;  // didn't get it, don't block, try the next bucket
                continue;
              }
              for (auto& element : tmpBuckets[i])
                tJoiners[joinerNum][i]->insert(element);
              addToJoinerLocks[joinerNum][i].unlock();
              tmpBuckets[i].clear();
              didSomeWork = true;
            }
          }
          // if this iteration did no useful work, everything we need is locked; wait briefly
          // and try again.
          if (!done && !didSomeWork)
          {
            ::usleep(500 * processorThreads);
            //++noWorkCounter;
          }
        }

        // cout << "T numeric join insert.  Took " << loopCounter << " loops" << endl;
      }
      else
      {
        // this first loop hashes incoming values into vectors that parallel the hash tables.
        for (i = 0; i < count; ++i)
        {
          bucket = bucketPicker((char*)&arr[i].key, 8, bpSeed) & ptMask;
          tmpBuckets[bucket].push_back(make_pair(arr[i].key, arr[i].value));
        }

        bool done = false;
        bool didSomeWork;
        // uint loopCounter = 0, noWorkCounter = 0;
        // this loop moves the elements from each vector into its corresponding hash table.
        while (!done)
        {
          //++loopCounter;
          done = true;
          didSomeWork = false;
          for (i = 0; i < processorThreads; ++i)
          {
            if (!tmpBuckets[i].empty())
            {
              bool gotIt = addToJoinerLocks[joinerNum][i].try_lock();
              if (!gotIt)
              {
                done = false;  // didn't get it, don't block, try the next bucket
                continue;
              }
              for (auto& element : tmpBuckets[i])
                tJoiners[joinerNum][i]->insert(element);
              addToJoinerLocks[joinerNum][i].unlock();
              tmpBuckets[i].clear();
              didSomeWork = true;
            }
          }
          // if this iteration did no useful work, everything we need is locked; wait briefly
          // and try again.
          if (!done && !didSomeWork)
          {
            ::usleep(500 * processorThreads);
            //++noWorkCounter;
          }
        }
        // cout << "T numeric join insert 2.  Took " << loopCounter << " loops," <<
        //    " unproductive iterations = " << noWorkCounter << endl;
      }
    }

    if (!typelessJoin[joinerNum])
      bs.advance(count * sizeof(JoinerElements));

    if (getTupleJoinRowGroupData)
    {
      RowGroup& smallSide = smallSideRGs[joinerNum];
      RGData offTheWire;

      // TODO: write an RGData fcn to let it interpret data within a ByteStream to avoid
      // the extra copying.
      offTheWire.deserialize(bs);
      boost::mutex::scoped_lock lk(smallSideDataLocks[joinerNum]);
      smallSide.setData(&smallSideRowData[joinerNum]);
      smallSide.append(offTheWire, startPos);

      /*  This prints the row data
                          smallSideRGs[joinerNum].initRow(&r);
                          for (i = 0; i < (tJoinerSizes[joinerNum] * smallSideRowLengths[joinerNum]);
         i+=r.getSize()) { r.setData(&smallSideRowData[joinerNum][i +
         smallSideRGs[joinerNum].getEmptySize()]); cout << " got row: " << r.toString() << endl;
                          }
      */
    }
  }

  idbassert(bs.length() == 0);
}

int BatchPrimitiveProcessor::endOfJoiner()
{
  /* Wait for all joiner elements to be added */
  uint32_t i;
  size_t currentSize;

  if (endOfJoinerRan)
    return 0;

  // minor hack / optimization.  The instances not inserting the table data don't
  // need to check that the table is complete.
  if (!firstInstance)
  {
    endOfJoinerRan = true;
    pthread_mutex_unlock(&objLock);
    return 0;
  }

  for (i = 0; i < joinerCount; i++)
  {
    if (!typelessJoin[i])
    {
      currentSize = 0;
      for (uint j = 0; j < processorThreads; ++j)
        if (!tJoiners[i] || !tJoiners[i][j])
        {
          return -1;
        }
        else
          currentSize += tJoiners[i][j]->size();
      if (currentSize != tJoinerSizes[i])
      {
        return -1;
      }
    }
    else
    {
      currentSize = 0;
      for (uint j = 0; j < processorThreads; ++j)
      {
        if (!tlJoiners[i] || !tlJoiners[i][j])
        {
          return -1;
        }
        else
          currentSize += tlJoiners[i][j]->size();
      }
      if (currentSize != tJoinerSizes[i])
      {
        return -1;
      }
    }
  }

  endOfJoinerRan = true;

  pthread_mutex_unlock(&objLock);
  return 0;
}

void BatchPrimitiveProcessor::initProcessor()
{
  uint32_t i, j;

  if (gotAbsRids || needStrValues || hasRowGroup)
    absRids.reset(new uint64_t[LOGICAL_BLOCK_RIDS]);

  if (needStrValues)
    strValues.reset(new utils::NullString[LOGICAL_BLOCK_RIDS]);

  outMsgSize = defaultBufferSize;
  outputMsg.reset(new (std::align_val_t(MAXCOLUMNWIDTH)) uint8_t[outMsgSize]);

  if (ot == ROW_GROUP)
  {
    // calculate the projection -> outputRG mapping
    projectionMap.reset(new int[projectCount]);
    bool* reserved = (bool*)alloca(outputRG.getColumnCount() * sizeof(bool));

    for (i = 0; i < outputRG.getColumnCount(); i++)
      reserved[i] = false;

    for (i = 0; i < projectCount; i++)
    {
      for (j = 0; j < outputRG.getColumnCount(); j++)
        if (projectSteps[i]->getTupleKey() == outputRG.getKeys()[j] && !reserved[j])
        {
          projectionMap[i] = j;
          reserved[j] = true;
          break;
        }

      if (j == outputRG.getColumnCount())
        projectionMap[i] = -1;
    }

    if (doJoin)
    {
      outputRG.initRow(&oldRow);
      outputRG.initRow(&newRow);

      tSmallSideMatches.reset(new MatchedData[joinerCount]);
      keyColumnProj.reset(new bool[projectCount]);

      for (i = 0; i < projectCount; i++)
      {
        keyColumnProj[i] = false;

        for (j = 0; j < joinerCount; j++)
        {
          if (!typelessJoin[j])
          {
            if (projectionMap[i] == (int)largeSideKeyColumns[j])
            {
              keyColumnProj[i] = true;
              break;
            }
          }
          else
          {
            for (uint32_t k = 0; k < tlLargeSideKeyColumns[j].size(); k++)
            {
              if (projectionMap[i] == (int)tlLargeSideKeyColumns[j][k])
              {
                keyColumnProj[i] = true;
                break;
              }
            }
          }
        }
      }

      if (hasJoinFEFilters)
      {
        joinFERG->initRow(&joinFERow, true);
        joinFERowData.reset(new uint8_t[joinFERow.getSize()]);
        joinFERow.setData(rowgroup::Row::Pointer(joinFERowData.get()));
        joinFEMappings.reset(new std::shared_ptr<int[]>[joinerCount + 1]);

        for (i = 0; i < joinerCount; i++)
          joinFEMappings[i] = makeMapping(smallSideRGs[i], *joinFERG);

        joinFEMappings[joinerCount] = makeMapping(largeSideRG, *joinFERG);
      }
    }

    /*
    Calculate the FE1 -> projection mapping
    Calculate the projection step -> FE1 input mapping
    */
    if (fe1)
    {
      fe1ToProjection = makeMapping(fe1Input, outputRG);
      projectForFE1.reset(new int[projectCount]);
      bool* reserved = (bool*)alloca(fe1Input.getColumnCount() * sizeof(bool));

      for (i = 0; i < fe1Input.getColumnCount(); i++)
        reserved[i] = false;

      for (i = 0; i < projectCount; i++)
      {
        projectForFE1[i] = -1;

        for (j = 0; j < fe1Input.getColumnCount(); j++)
        {
          if (projectSteps[i]->getTupleKey() == fe1Input.getKeys()[j] && !reserved[j])
          {
            reserved[j] = true;
            projectForFE1[i] = j;
            break;
          }
        }
      }

      fe1Input.initRow(&fe1In);
      outputRG.initRow(&fe1Out);
    }

    if (fe2)
    {
      fe2Input = (doJoin ? &joinedRG : &outputRG);
      fe2Mapping = makeMapping(*fe2Input, fe2Output);
      fe2Input->initRow(&fe2In);
      fe2Output.initRow(&fe2Out);
    }

    if (getTupleJoinRowGroupData)
    {
      gjrgPlaceHolders.reset(new uint32_t[joinerCount]);
      outputRG.initRow(&largeRow);
      joinedRG.initRow(&joinedRow);
      joinedRG.initRow(&baseJRow, true);
      smallRows.reset(new Row[joinerCount]);

      for (i = 0; i < joinerCount; i++)
        smallSideRGs[i].initRow(&smallRows[i], true);

      baseJRowMem.reset(new uint8_t[baseJRow.getSize()]);
      baseJRow.setData(rowgroup::Row::Pointer(baseJRowMem.get()));
      gjrgMappings.reset(new std::shared_ptr<int[]>[joinerCount + 1]);

      for (i = 0; i < joinerCount; i++)
        gjrgMappings[i] = makeMapping(smallSideRGs[i], joinedRG);

      gjrgMappings[joinerCount] = makeMapping(outputRG, joinedRG);
    }
  }

  // @bug 1051
  if (hasFilterStep)
  {
    for (uint64_t i = 0; i < 2; i++)
    {
      fFiltRidCount[i] = 0;
      fFiltCmdRids[i].reset(new uint16_t[LOGICAL_BLOCK_RIDS]);
      fFiltCmdValues[i].reset(new int64_t[LOGICAL_BLOCK_RIDS]);
      if (wideColumnsWidths | datatypes::MAXDECIMALWIDTH)
        fFiltCmdBinaryValues[i].reset(new int128_t[LOGICAL_BLOCK_RIDS]);

      if (filtOnString)
        fFiltStrValues[i].reset(new utils::NullString[LOGICAL_BLOCK_RIDS]);
    }
  }

  /* init the Commands */
  if (filterCount > 0)
  {
    for (i = 0; i < (uint32_t)filterCount - 1; ++i)
    {
      filterSteps[i]->setBatchPrimitiveProcessor(this);

      if (filterSteps[i + 1]->getCommandType() == Command::DICT_STEP)
        filterSteps[i]->prep(OT_BOTH, true);
      else if (filterSteps[i]->filterFeeder() != Command::NOT_FEEDER)
        filterSteps[i]->prep(OT_BOTH, false);
      else
        filterSteps[i]->prep(OT_RID, false);
    }

    filterSteps[i]->setBatchPrimitiveProcessor(this);
    filterSteps[i]->prep(OT_BOTH, false);
  }

  for (i = 0; i < projectCount; ++i)
  {
    projectSteps[i]->setBatchPrimitiveProcessor(this);

    if (noVB)
      projectSteps[i]->prep(OT_BOTH, false);
    else
      projectSteps[i]->prep(OT_DATAVALUE, false);

    if (0 < filterCount)
    {
      // if there is an rtscommand with a passThru, the passThru must make its own absRids
      // unless there is only one project step, then the last filter step can make absRids
      RTSCommand* rts = dynamic_cast<RTSCommand*>(projectSteps[i].get());

      if (rts && rts->isPassThru())
      {
        if (1 == projectCount)
          filterSteps[filterCount - 1]->setMakeAbsRids(true);
        else
          rts->setAbsNull();
      }
    }
  }

  if (fAggregator.get() != nullptr)
  {
    fAggRowGroupData.reinit(fAggregateRG);
    fAggregateRG.setData(&fAggRowGroupData);

    if (doJoin)
    {
      fAggregator->setInputOutput(fe2 ? fe2Output : joinedRG, &fAggregateRG);
      fAggregator->setJoinRowGroups(&smallSideRGs, &largeSideRG);
    }
    else
      fAggregator->setInputOutput(fe2 ? fe2Output : outputRG, &fAggregateRG);
  }

  if (LIKELY(!hasWideColumnOut))
  {
    minVal = MAX64;
    maxVal = MIN64;
  }
  else
  {
    max128Val = datatypes::Decimal::minInt128;
    min128Val = datatypes::Decimal::maxInt128;
  }

  // @bug 1269, initialize data used by execute() for async loading blocks
  // +2 for:
  //    1. the scan filter step with no predicate, if any
  //    2. AUX column
  relLBID.reset(new uint64_t[projectCount + 2]);
  asyncLoaded.reset(new bool[projectCount + 2]);
}

// This version does a join on projected rows
// In order to prevent super size result sets in the case of near cartesian joins on three or more joins,
// the startRid start at 0) is used to begin the rid loop and if we cut off processing early because of
// the size of the result set, we return the next rid to start with. If we finish ridCount rids, return 0-
uint32_t BatchPrimitiveProcessor::executeTupleJoin(uint32_t startRid, RowGroup& largeSideRowGroup)
{
  uint32_t newRowCount = 0, i, j;
  vector<uint32_t> matches;
  uint64_t largeKey;
  uint64_t resultCount = 0;
  uint32_t newStartRid = startRid;
  largeSideRowGroup.getRow(startRid, &oldRow);
  outputRG.getRow(0, &newRow);

  // ridCount gets modified based on the number of Rids actually processed during this call.
  // origRidCount is the number of rids for this thread after filter, which are the total
  // number of rids to be processed from all calls to this function during this thread.
  for (i = startRid; i < origRidCount && !sendThread->aborted(); i++, oldRow.nextRow())
  {
    /* Decide whether this large-side row belongs in the output.  The breaks
     * in the loop mean that it doesn't.
     *
     * In English the logic is:
     * 		Reject the row if there's no match and it's not an anti or an outer join
     *      or if there is a match and it's an anti join with no filter.
     * 		If there's an antijoin with a filter nothing can be eliminated at this stage.
     * 		If there's an antijoin where the join op should match NULL values, and there
     * 		  are NULL values to match against, but there is no filter, all rows can be eliminated.
     */

    for (j = 0; j < joinerCount; j++)
    {
      bool found;
      if (UNLIKELY(joinTypes[j] & ANTI))
      {
        if (joinTypes[j] & WITHFCNEXP)
          continue;
        else if (doMatchNulls[j])
          break;
      }

      if (LIKELY(!typelessJoin[j]))
      {
        bool isNull;
        uint32_t colIndex = largeSideKeyColumns[j];

        if (oldRow.isUnsigned(colIndex))
          largeKey = oldRow.getUintField(colIndex);
        else
          largeKey = oldRow.getIntField(colIndex);
        uint bucket = bucketPicker((char*)&largeKey, 8, bpSeed) & ptMask;

        bool joinerIsEmpty = tJoiners[j][bucket]->empty();

        found = (tJoiners[j][bucket]->find(largeKey) != tJoiners[j][bucket]->end());
        isNull = oldRow.isNullValue(colIndex);
        /* These conditions define when the row is NOT in the result set:
         *    - if the key is not in the small side, and the join isn't a large-outer or anti join
         *    - if the key is NULL, and the join isn't anti- or large-outer
         *    - if it's an anti-join and the key is either in the small side or it's NULL
         */

        if (((!found || isNull) && !(joinTypes[j] & (LARGEOUTER | ANTI))) ||
            ((joinTypes[j] & ANTI) && !joinerIsEmpty &&
             ((isNull && (joinTypes[j] & MATCHNULLS)) || (found && !isNull))))
        {
          break;
        }
      }
      else
      {
        // the null values are not sent by UM in typeless case.  null -> !found
        TypelessData tlLargeKey(&oldRow);
        uint bucket = oldRow.hashTypeless(tlLargeSideKeyColumns[j], mSmallSideKeyColumnsPtr,
                                          mSmallSideRGPtr ? &mSmallSideRGPtr->getColWidths() : nullptr) &
                      ptMask;
        found = tlJoiners[j][bucket]->find(tlLargeKey) != tlJoiners[j][bucket]->end();

        if ((!found && !(joinTypes[j] & (LARGEOUTER | ANTI))) || (joinTypes[j] & ANTI))
        {
          // Separated the ANTI join logic for readability.
          if (joinTypes[j] & ANTI)
          {
            if (found)
              break;
            else if (joinTypes[j] & MATCHNULLS)
            {
              bool hasNull = false;

              for (unsigned int column : tlLargeSideKeyColumns[j])
                if (oldRow.isNullValue(column))
                {
                  hasNull = true;
                  break;
                }

              if (hasNull)  // keys with nulls match everything
                break;
              else
                continue;  // non-null keys not in the small side

              // are in the result
            }
            else  // signifies a not-exists query
              continue;
          }

          break;
        }
      }
    }

    if (j == joinerCount)
    {
      uint32_t matchCount;
      for (j = 0; j < joinerCount; j++)
      {
        /* The result is already known if...
         *   -- anti-join with no fcnexp
         *   -- semi-join with no fcnexp and not scalar
         *
         * The ANTI join case isn't just a shortcut.  getJoinResults() will produce results
         * for a different case and generate the wrong result.  Need to fix that, later.
         */
        if ((joinTypes[j] & (SEMI | ANTI)) && !(joinTypes[j] & WITHFCNEXP) && !(joinTypes[j] & SCALAR))
        {
          tSmallSideMatches[j][newRowCount].push_back(-1);
          continue;
        }

        getJoinResults(oldRow, j, tSmallSideMatches[j][newRowCount]);
        matchCount = tSmallSideMatches[j][newRowCount].size();

        if (joinTypes[j] & WITHFCNEXP)
        {
          vector<uint32_t> newMatches;
          applyMapping(joinFEMappings[joinerCount], oldRow, &joinFERow);

          for (uint32_t k = 0; k < matchCount; k++)
          {
            if (tSmallSideMatches[j][newRowCount][k] == (uint32_t)-1)
              smallRows[j].setPointer(smallNullPointers[j]);
            else
            {
              smallSideRGs[j].getRow(tSmallSideMatches[j][newRowCount][k], &smallRows[j]);
            }

            applyMapping(joinFEMappings[j], smallRows[j], &joinFERow);

            if (joinFEFilters[j]->evaluate(&joinFERow))
            {
              /* The first match includes it in a SEMI join result and excludes it from an ANTI join
               * result.  If it's SEMI & SCALAR however, it needs to continue.
               */
              newMatches.push_back(tSmallSideMatches[j][newRowCount][k]);

              if ((joinTypes[j] & ANTI) || ((joinTypes[j] & (SEMI | SCALAR)) == SEMI))
                break;
            }
          }

          tSmallSideMatches[j][newRowCount].swap(newMatches);
          matchCount = tSmallSideMatches[j][newRowCount].size();
        }

        if (matchCount == 0 && (joinTypes[j] & LARGEOUTER))
        {
          tSmallSideMatches[j][newRowCount].push_back(-1);
          matchCount = 1;
        }

        /* Scalar check */
        if ((joinTypes[j] & SCALAR) && matchCount > 1)
          throw scalar_exception();

        /* Reverse the result for anti-join */
        if (joinTypes[j] & ANTI)
        {
          if (matchCount == 0)
          {
            tSmallSideMatches[j][newRowCount].push_back(-1);
            matchCount = 1;
          }
          else
          {
            tSmallSideMatches[j][newRowCount].clear();
            matchCount = 0;
          }
        }

        /* For all join types, no matches here means it's not in the result */
        if (matchCount == 0)
          break;

        /* Pair non-scalar semi-joins with a NULL row */
        if ((joinTypes[j] & SEMI) && !(joinTypes[j] & SCALAR))
        {
          tSmallSideMatches[j][newRowCount].clear();
          tSmallSideMatches[j][newRowCount].push_back(-1);
          matchCount = 1;
        }

        resultCount += matchCount;
      }

      /* Finally, copy the row into the output */
      if (j == joinerCount)
      {
        // We need to update 8 and 16 bytes in values and wide128Values buffers
        // otherwise unrelated values will be observed in the JOIN-ed output RGData.
        if (i != newRowCount)
        {
          values[newRowCount] = values[i];
          if (mJOINHasSkewedKeyColumn)
            wide128Values[newRowCount] = wide128Values[i];
          relRids[newRowCount] = relRids[i];
          copyRow(oldRow, &newRow);
        }

        newRowCount++;
        newRow.nextRow();
      }
    }
    // If we've accumulated more than `maxPmJoinResultCount` of `resultCounts`, cut off processing.
    // The caller will restart to continue where we left off.
    if (resultCount >= maxPmJoinResultCount)
    {
      // New start rid is a next row for large side.
      newStartRid = i + 1;
      break;
    }
  }

  if (resultCount < maxPmJoinResultCount)
    newStartRid = 0;

  ridCount = newRowCount;
  outputRG.setRowCount(ridCount);

  return newStartRid;
}

#ifdef PRIMPROC_STOPWATCH
void BatchPrimitiveProcessor::execute(StopWatch* stopwatch, messageqcpp::SBS& bs)
#else
void BatchPrimitiveProcessor::execute(messageqcpp::SBS& bs)
#endif
{
  uint8_t sendCount = 0;
  //    bool smoreRGs = false;
  //    uint32_t sStartRid = 0;
  uint32_t i, j;

  try
  {
#ifdef PRIMPROC_STOPWATCH
    stopwatch->start("BatchPrimitiveProcessor::execute first part");
#endif
    utils::setThreadName("BPPFilt&Pr");

    // if only one scan step which has no predicate, async load all columns
    if (filterCount == 1 && hasScan)
    {
      ColumnCommand* col = dynamic_cast<ColumnCommand*>(filterSteps[0].get());

      if ((col != nullptr) && (col->getFilterCount() == 0) && (col->getLBID() != 0))
      {
        // stored in last pos in relLBID[] and asyncLoaded[]
        uint64_t p = projectCount;
        asyncLoaded[p] = asyncLoaded[p] && (relLBID[p] % blocksReadAhead != 0);
        relLBID[p] += col->getWidth();

        if (!asyncLoaded[p] && col->willPrefetch())
        {
          loadBlockAsync(col->getLBID(), versionInfo, txnID, col->getCompType(), &cachedIO, &physIO,
                         LBIDTrace, sessionID, &counterLock, &busyLoaderCount, sendThread, &vssCache);
          asyncLoaded[p] = true;
        }

        if (col->hasAuxCol())
        {
          asyncLoaded[p + 1] = asyncLoaded[p + 1] && (relLBID[p + 1] % blocksReadAhead != 0);
          relLBID[p + 1] += 1;

          if (!asyncLoaded[p + 1])
          {
            loadBlockAsync(col->getLBIDAux(), versionInfo, txnID, 2, &cachedIO, &physIO, LBIDTrace, sessionID,
                           &counterLock, &busyLoaderCount, sendThread, &vssCache);
            asyncLoaded[p + 1] = true;
          }
        }

        asyncLoadProjectColumns();
      }
    }

#ifdef PRIMPROC_STOPWATCH
    stopwatch->stop("BatchPrimitiveProcessor::execute first part");
    stopwatch->start("BatchPrimitiveProcessor::execute second part");
#endif

    // filters use relrids and values for intermediate results.
    if (bop == BOP_AND)
    {
      for (j = 0; j < filterCount; ++j)
      {
#ifdef PRIMPROC_STOPWATCH
        stopwatch->start("- filterSteps[j]->execute()");
        filterSteps[j]->execute();
        stopwatch->stop("- filterSteps[j]->execute()");
#else
        filterSteps[j]->execute();
#endif
      }
    }
    else  // BOP_OR
    {
      /* XXXPAT: This is a hacky impl of OR logic.  Each filter is configured to
      be a scan operation on init.  This code runs each independently and
      unions their output ridlists using accumulator.  At the end it turns
      accumulator into a final ridlist for subsequent steps.

      If there's a join or a passthru command in the projection list, the
      values array has to contain values from the last filter step.  In that
      case, the last filter step isn't part of the "OR" filter processing.
      JLF has added it to prep those operations, not to be a filter.

      7/7/09 update: the multiple-table join required relocating the join op.  It's
      no longer necessary to add the loader columncommand to the filter array.
      */

      bool accumulator[LOGICAL_BLOCK_RIDS];
      // 		uint32_t realFilterCount = ((forHJ || hasPassThru) ? filterCount - 1 : filterCount);
      uint32_t realFilterCount = filterCount;

      for (i = 0; i < LOGICAL_BLOCK_RIDS; i++)
        accumulator[i] = false;

      if (!hasScan)  // there are input rids
        for (i = 0; i < ridCount; i++)
          accumulator[relRids[i]] = true;

      ridCount = 0;

      for (i = 0; i < realFilterCount; ++i)
      {
        filterSteps[i]->execute();

        if (!filterSteps[i]->filterFeeder())
        {
          for (j = 0; j < ridCount; j++)
            accumulator[relRids[j]] = true;

          ridCount = 0;
        }
      }

      for (ridMap = 0, i = 0; i < LOGICAL_BLOCK_RIDS; ++i)
      {
        if (accumulator[i])
        {
          relRids[ridCount] = i;
          ridMap |= 1 << (relRids[ridCount] >> 9);
          ++ridCount;
        }
      }
    }

#ifdef PRIMPROC_STOPWATCH
    stopwatch->stop("BatchPrimitiveProcessor::execute second part");
    stopwatch->start("BatchPrimitiveProcessor::execute third part");
#endif

    if (projectCount > 0 || ot == ROW_GROUP)
    {
#ifdef PRIMPROC_STOPWATCH
      stopwatch->start("- writeProjectionPreamble");
      writeProjectionPreamble();
      stopwatch->stop("- writeProjectionPreamble");
#else
      writeProjectionPreamble(bs);
#endif
    }

    // async load blocks for project phase, if not alread loaded
    if (ridCount > 0)
    {
#ifdef PRIMPROC_STOPWATCH
      stopwatch->start("- asyncLoadProjectColumns");
      asyncLoadProjectColumns();
      stopwatch->stop("- asyncLoadProjectColumns");
#else
      asyncLoadProjectColumns();
#endif
    }

#ifdef PRIMPROC_STOPWATCH
    stopwatch->stop("BatchPrimitiveProcessor::execute third part");
    stopwatch->start("BatchPrimitiveProcessor::execute fourth part");
#endif

    // projection commands read relrids and write output directly to a rowgroup
    // or the serialized bytestream
    if (ot != ROW_GROUP)
    {
      for (j = 0; j < projectCount; ++j)
      {
        projectSteps[j]->project(bs);
      }
    }
    else
    {
      /* Function & Expression group 1 processing
          - project for FE1
          - execute FE1 row by row
          - if return value = true, map input row into the projection RG, adjust ridlist
      */
#ifdef PRIMPROC_STOPWATCH
      stopwatch->start("- if(ot != ROW_GROUP) else");
#endif
      outputRG.resetRowGroup(baseRid);

      utils::setThreadName("BPPFE1_1");

      if (fe1)
      {
        uint32_t newRidCount = 0;
        fe1Input.resetRowGroup(baseRid);
        fe1Input.setRowCount(ridCount);
        fe1Input.getRow(0, &fe1In);
        outputRG.getRow(0, &fe1Out);

        for (j = 0; j < projectCount; j++)
          if (projectForFE1[j] != -1)
            projectSteps[j]->projectIntoRowGroup(fe1Input, projectForFE1[j]);

        for (j = 0; j < ridCount; j++, fe1In.nextRow())
          if (fe1->evaluate(&fe1In))
          {
            applyMapping(fe1ToProjection, fe1In, &fe1Out);
            relRids[newRidCount] = relRids[j];
            values[newRidCount++] = values[j];
            fe1Out.nextRow();
          }

        ridCount = newRidCount;
      }

      outputRG.setRowCount(ridCount);

      if (sendRidsAtDelivery)
      {
        Row r;
        outputRG.initRow(&r);
        outputRG.getRow(0, &r);

        for (j = 0; j < ridCount; ++j)
        {
          r.setRid(relRids[j]);
          r.nextRow();
        }
      }

      /* 7/7/09 PL: I Changed the projection alg to reduce block touches when there's
      a join.  The key columns get projected first, the join is executed to further
      reduce the ridlist, then the rest of the columns get projected */

      if (!doJoin)
      {
        for (j = 0; j < projectCount; ++j)
        {
          if (projectionMap[j] != -1)
          {
#ifdef PRIMPROC_STOPWATCH
            stopwatch->start("-- projectIntoRowGroup");
            projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
            stopwatch->stop("-- projectIntoRowGroup");
#else
            projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
#endif
          }
        }
        if (fe2)
        {
          utils::setThreadName("BPPFE2_1");

          /* functionize this -> processFE2() */
          fe2Output.resetRowGroup(baseRid);
          fe2Output.getRow(0, &fe2Out);
          fe2Input->getRow(0, &fe2In);

          // cerr << "input row: " << fe2In.toString() << endl;
          for (j = 0; j < outputRG.getRowCount(); j++, fe2In.nextRow())
          {
            if (fe2->evaluate(&fe2In))
            {
              applyMapping(fe2Mapping, fe2In, &fe2Out);
              // cerr << "   passed. output row: " << fe2Out.toString() << endl;
              fe2Out.setRid(fe2In.getRelRid());
              fe2Output.incRowCount();
              fe2Out.nextRow();
            }
          }

          if (!fAggregator)
          {
            *bs << (uint8_t)1;  // the "count this msg" var
            fe2Output.setDBRoot(dbRoot);
            fe2Output.serializeRGData(*bs);
            //*serialized << fe2Output.getDataSize();
            // serialized->append(fe2Output.getData(), fe2Output.getDataSize());
          }
        }

        if (fAggregator)
        {
          utils::setThreadName("BPPAgg_1");

          *bs << (uint8_t)1;  // the "count this msg" var

          // see TupleBPS::setFcnExpGroup2() and where it gets called.
          // it sets fe2 there, on the other side of communication.
          RowGroup& toAggregate = (fe2 ? fe2Output : outputRG);
          // toAggregate.convertToInlineDataInPlace();

          if (fe2)
            fe2Output.setDBRoot(dbRoot);
          else
            outputRG.setDBRoot(dbRoot);

          fAggregator->addRowGroup(&toAggregate);

          if ((currentBlockOffset + 1) == count)  // @bug4507, 8k
          {
            fAggregator->loadResult(*bs);  // @bug4507, 8k
          }  // @bug4507, 8k
          else if (utils::MonitorProcMem::isMemAvailable())  // @bug4507, 8k
          {
            fAggregator->loadEmptySet(*bs);  // @bug4507, 8k
          }  // @bug4507, 8k
          else  // @bug4507, 8k
          {
            fAggregator->loadResult(*bs);  // @bug4507, 8k
            fAggregator->aggReset();       // @bug4507, 8k
          }  // @bug4507, 8k
        }

        if (!fAggregator && !fe2)
        {
          *bs << (uint8_t)1;  // the "count this msg" var
          outputRG.setDBRoot(dbRoot);
          // cerr << "serializing " << outputRG.toString() << endl;
          outputRG.serializeRGData(*bs);

          //*serialized << outputRG.getDataSize();
          // serialized->append(outputRG.getData(), outputRG.getDataSize());
        }

#ifdef PRIMPROC_STOPWATCH
        stopwatch->stop("- if(ot != ROW_GROUP) else");
#endif
      }
      else  // Is doJoin
      {
        uint32_t startRid = 0;
        ByteStream preamble = *bs;
        origRidCount = ridCount;  // ridCount can get modified by executeTupleJoin(). We need to keep track of
                                  // the original val.
        /* project the key columns.  If there's the filter IN the join, project everything.
           Also need to project 'long' strings b/c executeTupleJoin may copy entire rows
           using copyRow(), which will try to interpret the uninit'd string ptr.
           Valgrind will legitimately complain about copying uninit'd values for the
           other types but that is technically safe. */
        for (j = 0; j < projectCount; j++)
        {
          if (keyColumnProj[j] ||
              (projectionMap[j] != -1 && (hasJoinFEFilters || oldRow.isLongString(projectionMap[j]))))
          {
#ifdef PRIMPROC_STOPWATCH
            stopwatch->start("-- projectIntoRowGroup");
            projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
            stopwatch->stop("-- projectIntoRowGroup");
#else
            projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
#endif
          }
        }

        // Duplicate projected `RGData` to `large side` row group.
        // We create a `large side` row group from `output` row group,
        // to save an original data, because 'output` row group is used
        // to store matched rows from small side.
        RGData largeSideRGData = outputRG.duplicate();
        RowGroup largeSideRowGroup = outputRG;
        largeSideRowGroup.setData(&largeSideRGData);

        do  // while (startRid > 0)
        {
          utils::setThreadName("BPPJoin_1");

#ifdef PRIMPROC_STOPWATCH
          stopwatch->start("-- executeTupleJoin()");
          startRid = executeTupleJoin(startRid, largeSideRowGroup);
          stopwatch->stop("-- executeTupleJoin()");
#else
          startRid = executeTupleJoin(startRid, largeSideRowGroup);
#endif

          /* project the non-key columns */
          for (j = 0; j < projectCount; ++j)
          {
            if (projectionMap[j] != -1 && !keyColumnProj[j] && !hasJoinFEFilters &&
                !oldRow.isLongString(projectionMap[j]))
            {
#ifdef PRIMPROC_STOPWATCH
              stopwatch->start("-- projectIntoRowGroup");
              projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
              stopwatch->stop("-- projectIntoRowGroup");
#else
              projectSteps[j]->projectIntoRowGroup(outputRG, projectionMap[j]);
#endif
            }
          }
          /* The RowGroup is fully joined at this point.
           *            Add additional RowGroup processing here.
           *            TODO:  Try to clean up all of the switching */

          if (fe2 || fAggregator)
          {
            bool moreRGs = true;
            initGJRG();

            while (moreRGs && !sendThread->aborted())
            {
              /*
               *                        generate 1 rowgroup (8192 rows max) of joined rows
               *                        if there's an FE2, run it
               *                                -pack results into a new rowgroup
               *                                -if there are < 8192 rows in the new RG, continue
               *                        if there's an agg, run it
               *                        send the result
               */
              resetGJRG();
              moreRGs = generateJoinedRowGroup(baseJRow);
              //                            smoreRGs = moreRGs;
              sendCount = (uint8_t)(!moreRGs && !startRid);
              //                            *serialized << (uint8_t)(!moreRGs && !startRid);   // the "count
              //                            this msg" var
              *bs << sendCount;
              if (fe2)
              {
                utils::setThreadName("BPPFE2_2");

                /* functionize this -> processFE2()*/
                fe2Output.resetRowGroup(baseRid);
                fe2Output.setDBRoot(dbRoot);
                fe2Output.getRow(0, &fe2Out);
                fe2Input->getRow(0, &fe2In);

                for (j = 0; j < joinedRG.getRowCount(); j++, fe2In.nextRow())
                {
                  if (fe2->evaluate(&fe2In))
                  {
                    applyMapping(fe2Mapping, fe2In, &fe2Out);
                    fe2Out.setRid(fe2In.getRelRid());
                    fe2Output.incRowCount();
                    fe2Out.nextRow();
                  }
                }
              }

              RowGroup& nextRG = (fe2 ? fe2Output : joinedRG);
              nextRG.setDBRoot(dbRoot);

              if (fAggregator)
              {
                utils::setThreadName("BPPAgg_2");

                fAggregator->addRowGroup(&nextRG);

                if ((currentBlockOffset + 1) == count && moreRGs == false && startRid == 0)  // @bug4507, 8k
                {
                  fAggregator->loadResult(*bs);  // @bug4507, 8k
                }  // @bug4507, 8k
                else if (utils::MonitorProcMem::isMemAvailable())  // @bug4507, 8k
                {
                  fAggregator->loadEmptySet(*bs);  // @bug4507, 8k
                }  // @bug4507, 8k
                else  // @bug4507, 8k
                {
                  fAggregator->loadResult(*bs);  // @bug4507, 8k
                  fAggregator->aggReset();       // @bug4507, 8k
                }  // @bug4507, 8k
              }
              else
              {
                // cerr <<" * serialzing " << nextRG.toString() << endl;
                nextRG.serializeRGData(*bs);
              }

              /* send the msg & reinit the BS */
              if (moreRGs)
              {
                sendResponse(bs);
                bs.reset(new ByteStream());
                *bs = preamble;
              }
            }

            if (hasSmallOuterJoin)
            {
              // Should we happen to finish sending data rows right on the boundary of when moreRGs flips off,
              // then we need to start a new buffer. I.e., it needs the count this message byte pushed.
              if (bs->length() == preamble.length())
                *bs << (uint8_t)(startRid > 0 ? 0 : 1);  // the "count this msg" var

              *bs << ridCount;

              for (i = 0; i < joinerCount; i++)
              {
                for (j = 0; j < ridCount; ++j)
                {
                  serializeInlineVector<uint32_t>(*bs, tSmallSideMatches[i][j]);
                  tSmallSideMatches[i][j].clear();
                }
              }
            }
            else
            {
              // We have no more use for this allocation
              for (i = 0; i < joinerCount; i++)
                for (j = 0; j < ridCount; ++j)
                  tSmallSideMatches[i][j].clear();
            }
          }
          else
          {
            *bs << (uint8_t)(startRid > 0 ? 0 : 1);  // the "count this msg" var
            outputRG.setDBRoot(dbRoot);
            // cerr << "serializing " << outputRG.toString() << endl;
            outputRG.serializeRGData(*bs);

            //*serialized << outputRG.getDataSize();
            // serialized->append(outputRG.getData(), outputRG.getDataSize());
            for (i = 0; i < joinerCount; i++)
            {
              for (j = 0; j < ridCount; ++j)
              {
                serializeInlineVector<uint32_t>(*bs, tSmallSideMatches[i][j]);
                tSmallSideMatches[i][j].clear();
              }
            }
          }
          if (startRid > 0)
          {
            sendResponse(bs);
            bs.reset(new ByteStream());
            *bs = preamble;
          }
        } while (startRid > 0);
      }
#ifdef PRIMPROC_STOPWATCH
      stopwatch->stop("- if(ot != ROW_GROUP) else");
#endif
    }
    ridCount = origRidCount;  // May not be needed, but just to be safe.
    //        std::cout << "end of send. startRid=" << sStartRid << " moreRG=" << smoreRGs << " sendCount=" <<
    //        sendCount << std::endl;
    if (projectCount > 0 || ot == ROW_GROUP)
    {
      *bs << cachedIO;
      cachedIO = 0;
      *bs << physIO;
      physIO = 0;
      *bs << touchedBlocks;
      touchedBlocks = 0;
      // 		cout << "sent physIO=" << physIO << " cachedIO=" << cachedIO <<
      // 			" touchedBlocks=" << touchedBlocks << endl;
    }
    utils::setThreadName("BPPExecuteEnd");

#ifdef PRIMPROC_STOPWATCH
    stopwatch->stop("BatchPrimitiveProcessor::execute fourth part");
#endif
  }
  catch (logging::QueryDataExcept& qex)
  {
    writeErrorMsg(bs, qex.what(), qex.errorCode());
  }
  catch (logging::DictionaryBufferOverflow& db)
  {
    writeErrorMsg(bs, db.what(), db.errorCode());
  }
  catch (scalar_exception& se)
  {
    writeErrorMsg(bs, IDBErrorInfo::instance()->errorMsg(ERR_MORE_THAN_1_ROW), ERR_MORE_THAN_1_ROW, false);
  }
  catch (NeedToRestartJob& n)
  {
#ifndef __FreeBSD__
    pthread_mutex_unlock(&objLock);
#endif
    throw n;  // need to pass this through to BPPSeeder
  }
  catch (IDBExcept& iex)
  {
    writeErrorMsg(bs, iex.what(), iex.errorCode(), true, false);
  }
  catch (const std::exception& ex)
  {
    writeErrorMsg(bs, ex.what(), logging::batchPrimitiveProcessorErr);
  }
  catch (...)
  {
    string msg("BatchPrimitiveProcessor caught an unknown exception");
    writeErrorMsg(bs, msg, logging::batchPrimitiveProcessorErr);
  }
}

void BatchPrimitiveProcessor::writeErrorMsg(messageqcpp::SBS& bs, const string& error, uint16_t errCode,
                                            bool logIt, bool critical)
{
  ISMPacketHeader ism;
  PrimitiveHeader ph;

  // we don't need every field of these headers.  Init'ing them anyway
  // makes memory checkers happy.
  void* ismp = static_cast<void*>(&ism);
  void* php = static_cast<void*>(&ph);
  memset(ismp, 0, sizeof(ISMPacketHeader));
  memset(php, 0, sizeof(PrimitiveHeader));
  ph.SessionID = sessionID;
  ph.StepID = stepID;
  ph.UniqueID = uniqueID;
  ism.Status = errCode;

  bs.reset(new ByteStream());
  bs->append((uint8_t*)&ism, sizeof(ism));
  bs->append((uint8_t*)&ph, sizeof(ph));
  *bs << error;

  if (logIt)
  {
    Logger log;
    log.logMessage(error, critical);
  }
}

void BatchPrimitiveProcessor::writeProjectionPreamble(SBS& bs)
{
  ISMPacketHeader ism;
  PrimitiveHeader ph;

  // we don't need every field of these headers.  Init'ing them anyway
  // makes memory checkers happy.
  void* ismp = static_cast<void*>(&ism);
  void* php = static_cast<void*>(&ph);
  memset(ismp, 0, sizeof(ISMPacketHeader));
  memset(php, 0, sizeof(PrimitiveHeader));
  ph.SessionID = sessionID;
  ph.StepID = stepID;
  ph.UniqueID = uniqueID;

  bs.reset(new ByteStream());
  bs->append((uint8_t*)&ism, sizeof(ism));
  bs->append((uint8_t*)&ph, sizeof(ph));

  /* add-ons */
  if (hasScan)
  {
    if (validCPData)
    {
      *bs << (uint8_t)1;
      *bs << lbidForCP;
      *bs << ((uint8_t)cpDataFromDictScan);
      if (UNLIKELY(hasWideColumnOut))
      {
        // PSA width
        *bs << (uint8_t)wideColumnWidthOut;
        *bs << min128Val;
        *bs << max128Val;
      }
      else
      {
        *bs << (uint8_t)utils::MAXLEGACYWIDTH;  // width of min/max value
        *bs << (uint64_t)minVal;
        *bs << (uint64_t)maxVal;
      }
    }
    else
    {
      *bs << (uint8_t)0;
      *bs << lbidForCP;
    }
  }

  // 	ridsOut += ridCount;
  /* results */

  if (ot != ROW_GROUP)
  {
    *bs << ridCount;

    if (sendRidsAtDelivery)
    {
      *bs << baseRid;
      bs->append((uint8_t*)relRids, ridCount << 1);
    }
  }
}

void BatchPrimitiveProcessor::serializeElementTypes(messageqcpp::SBS& bs)
{
  *bs << baseRid;
  *bs << ridCount;
  bs->append((uint8_t*)relRids, ridCount << 1);
  bs->append((uint8_t*)values, ridCount << 3);
}

void BatchPrimitiveProcessor::serializeStrings(messageqcpp::SBS& bs)
{
  *bs << ridCount;
  bs->append((uint8_t*)absRids.get(), ridCount << 3);

  for (uint32_t i = 0; i < ridCount; ++i)
    *bs << strValues[i];
}

void BatchPrimitiveProcessor::sendResponse(messageqcpp::SBS& bs)
{
  // Here is the fast path for local EM to PM interaction. PM puts into the
  // input EM DEC queue directly.
  // !writelock has a 'same host connection' semantics here.
  if (initiatedByEM_ && !writelock)
  {
    // Flow Control now handles same node connections so the recieving DEC queue
    // is limited.
    if (sendThread->flowControlEnabled())
    {
      sendThread->sendResult({bs, sock, writelock, 0}, false);
    }
    else
    {
      sock->write(bs);
      bs.reset();
    }

    return;
  }

  if (sendThread->flowControlEnabled())
  {
    // newConnection should be set only for the first result of a batch job
    // it tells sendthread it should consider it for the connection array
    sendThread->sendResult(BPPSendThread::Msg_t(bs, sock, writelock, sockIndex), newConnection);
    newConnection = false;
  }
  else
  {
    boost::mutex::scoped_lock lk(*writelock);
    sock->write(*bs);
  }

  bs.reset();
}

/* The output of a filter chain is either ELEMENT_TYPE or STRING_ELEMENT_TYPE */
void BatchPrimitiveProcessor::makeResponse(messageqcpp::SBS& bs)
{
  ISMPacketHeader ism;
  PrimitiveHeader ph;

  // we don't need every field of these headers.  Init'ing them anyway
  // makes memory checkers happy.
  void* ismp = static_cast<void*>(&ism);
  void* php = static_cast<void*>(&ph);
  memset(ismp, 0, sizeof(ISMPacketHeader));
  memset(php, 0, sizeof(PrimitiveHeader));
  ph.SessionID = sessionID;
  ph.StepID = stepID;
  ph.UniqueID = uniqueID;

  bs.reset(new ByteStream());
  bs->append((uint8_t*)&ism, sizeof(ism));
  bs->append((uint8_t*)&ph, sizeof(ph));

  /* add-ons */
  if (hasScan)
  {
    if (validCPData)
    {
      *bs << (uint8_t)1;
      *bs << lbidForCP;
      *bs << ((uint8_t)cpDataFromDictScan);

      if (UNLIKELY(hasWideColumnOut))
      {
        // PSA width
        // Remove the assert for >16 bytes DTs.
        assert(wideColumnWidthOut == datatypes::MAXDECIMALWIDTH);
        *bs << (uint8_t)wideColumnWidthOut;
        *bs << min128Val;
        *bs << max128Val;
      }
      else
      {
        *bs << (uint8_t)utils::MAXLEGACYWIDTH;  // width of min/max value
        *bs << (uint64_t)minVal;
        *bs << (uint64_t)maxVal;
      }
    }
    else
    {
      *bs << (uint8_t)0;
      *bs << lbidForCP;
    }
  }

  /* results */
  /* Take the rid and value arrays, munge into OutputType ot */
  switch (ot)
  {
    case BPS_ELEMENT_TYPE: serializeElementTypes(bs); break;

    case STRING_ELEMENT_TYPE: serializeStrings(bs); break;

    default:
    {
      ostringstream oss;
      oss << "BPP: makeResponse(): Bad output type: " << ot;
      throw logic_error(oss.str());
    }
  }

  *bs << cachedIO;
  cachedIO = 0;
  *bs << physIO;
  physIO = 0;
  *bs << touchedBlocks;
  touchedBlocks = 0;

  // 	cout << "sent physIO=" << physIO << " cachedIO=" << cachedIO <<
  // 		" touchedBlocks=" << touchedBlocks << endl;
}

int BatchPrimitiveProcessor::operator()()
{
  utils::setThreadName("PPBatchPrimProc");
#ifdef PRIMPROC_STOPWATCH
  const static std::string msg{"BatchPrimitiveProcessor::operator()"};
  logging::StopWatch* stopwatch = profiler.getTimer();
#endif

  if (currentBlockOffset == 0)
  {
#ifdef PRIMPROC_STOPWATCH
    stopwatch->start(msg);
#endif
    idbassert(count > 0);
  }

  if (fAggregator && currentBlockOffset == 0)  // @bug4507, 8k
    fAggregator->aggReset();                   // @bug4507, 8k

  for (; currentBlockOffset < count; currentBlockOffset++)
  {
    if (!(sessionID & 0x80000000))  // can't do this with syscat queries
    {
      if (sendThread->aborted())
        break;

      if (sendThread->sizeTooBig())
      {
        // The send buffer is full of messages yet to be sent, so this thread would block anyway.
        freeLargeBuffers();
        return -1;  // the reschedule error code
      }
    }

    allocLargeBuffers();

    if (LIKELY(!hasWideColumnOut))
    {
      minVal = MAX64;
      maxVal = MIN64;
    }
    else
    {
      max128Val = datatypes::Decimal::minInt128;
      min128Val = datatypes::Decimal::maxInt128;
    }

    validCPData = false;
    cpDataFromDictScan = false;

    // auto allocator = exemgr::globServiceExeMgr->getRm().getAllocator<messageqcpp::BSBufType>();
    // messageqcpp::SBS bs(new ByteStream(allocator));
    messageqcpp::SBS bs(new ByteStream());

#ifdef PRIMPROC_STOPWATCH
    stopwatch->start("BPP() execute");
    execute(stopwatch);
    stopwatch->stop("BPP() execute");
#else
    execute(bs);
#endif

    if (projectCount == 0 && ot != ROW_GROUP)
      makeResponse(bs);

    try
    {
      sendResponse(bs);
    }
    catch (std::exception& e)
    {
      break;  // If we make this throw, be sure to do the cleanup at the end
    }

    // Bug 4475: Control outgoing socket so that all messages from a
    // batch go out the same socket
    sockIndex = (sockIndex + 1) % connectionsPerUM;

    nextLBID();

    /* Base RIDs are now a combination of partition#, segment#, extent#, and block#. */
    uint32_t partNum;
    uint16_t segNum;
    uint8_t extentNum;
    uint16_t blockNum;
    rowgroup::getLocationFromRid(baseRid, &partNum, &segNum, &extentNum, &blockNum);
    /*
    cout << "baseRid=" << baseRid << " partNum=" << partNum << " segNum=" << segNum <<
                    " extentNum=" << (int) extentNum
                    << " blockNum=" << blockNum << endl;
    */
    blockNum++;
    baseRid = rowgroup::convertToRid(partNum, segNum, extentNum, blockNum);
    /*
    cout << "-- baseRid=" << baseRid << " partNum=" << partNum << " extentNum=" << (int) extentNum
                    << " blockNum=" << blockNum << endl;
    */
  }
  vssCache.clear();
#ifndef __FreeBSD__
  pthread_mutex_unlock(&objLock);
#endif
  freeLargeBuffers();
#ifdef PRIMPROC_STOPWATCH
  stopwatch->stop(msg);
#endif
  // 	cout << "sent " << count << " responses" << endl;
  fBusy = false;
  return 0;
}

void BatchPrimitiveProcessor::allocLargeBuffers()
{
  auto allocator = exemgr::globServiceExeMgr->getRm().getAllocator<rowgroup::RGDataBufType>();

  if (ot == ROW_GROUP && !outRowGroupData)
  {
    outRowGroupData.reset(new RGData(outputRG, allocator));
    outputRG.setData(outRowGroupData.get());
  }

  if (fe1 && !fe1Data)
  {
    fe1Data.reset(new RGData(fe1Input, allocator));
    fe1Input.setData(fe1Data.get());
  }

  if (fe2 && !fe2Data)
  {
    fe2Data.reset(new RGData(fe2Output, allocator));
    fe2Output.setData(fe2Data.get());
  }

  if (getTupleJoinRowGroupData && !joinedRGMem)
  {
    joinedRGMem.reset(new RGData(joinedRG, allocator));
    joinedRG.setData(joinedRGMem.get());
  }
}

void BatchPrimitiveProcessor::freeLargeBuffers()
{
  /* Get rid of large buffers */
  if (ot == ROW_GROUP && outputRG.getMaxDataSizeWithStrings() > maxIdleBufferSize)
    outRowGroupData.reset();

  if (fe1 && fe1Input.getMaxDataSizeWithStrings() > maxIdleBufferSize)
    fe1Data.reset();

  if (fe2 && fe2Output.getMaxDataSizeWithStrings() > maxIdleBufferSize)
    fe2Data.reset();

  if (getTupleJoinRowGroupData && joinedRG.getMaxDataSizeWithStrings() > maxIdleBufferSize)
    joinedRGMem.reset();
}

void BatchPrimitiveProcessor::nextLBID()
{
  uint32_t i;

  for (i = 0; i < filterCount; i++)
    filterSteps[i]->nextLBID();

  for (i = 0; i < projectCount; i++)
    projectSteps[i]->nextLBID();
}

SBPP BatchPrimitiveProcessor::duplicate()
{
  SBPP bpp;
  uint32_t i;

  //	cout << "duplicating a bpp\n";

  bpp.reset(new BatchPrimitiveProcessor());
  bpp->ot = ot;
  bpp->versionInfo = versionInfo;
  bpp->txnID = txnID;
  bpp->sessionID = sessionID;
  bpp->stepID = stepID;
  bpp->uniqueID = uniqueID;
  bpp->needStrValues = needStrValues;
  bpp->wideColumnsWidths = wideColumnsWidths;
  bpp->gotAbsRids = gotAbsRids;
  bpp->gotValues = gotValues;
  bpp->LBIDTrace = LBIDTrace;
  bpp->hasScan = hasScan;
  bpp->hasFilterStep = hasFilterStep;
  bpp->filtOnString = filtOnString;
  bpp->hasRowGroup = hasRowGroup;
  bpp->getTupleJoinRowGroupData = getTupleJoinRowGroupData;
  bpp->bop = bop;
  bpp->hasPassThru = hasPassThru;
  bpp->forHJ = forHJ;
  bpp->processorThreads = processorThreads;  // is a power-of-2 at this point
  bpp->ptMask = processorThreads - 1;

  if (ot == ROW_GROUP)
  {
    bpp->outputRG = outputRG;

    if (fe1)
    {
      bpp->fe1.reset(new FuncExpWrapper(*fe1));
      bpp->fe1Input = fe1Input;
    }

    if (fe2)
    {
      bpp->fe2.reset(new FuncExpWrapper(*fe2));
      bpp->fe2Output = fe2Output;
    }
  }

  bpp->doJoin = doJoin;

  if (doJoin)
  {
    pthread_mutex_lock(&bpp->objLock);
    /* There are add'l join vars, but only these are necessary for processing
         a join */
    bpp->tJoinerSizes = tJoinerSizes;
    bpp->joinerCount = joinerCount;
    bpp->joinTypes = joinTypes;
    bpp->largeSideKeyColumns = largeSideKeyColumns;
    bpp->tJoiners = tJoiners;
    // bpp->_pools = _pools;
    bpp->typelessJoin = typelessJoin;
    bpp->tlLargeSideKeyColumns = tlLargeSideKeyColumns;
    bpp->tlSmallSideKeyColumns = tlSmallSideKeyColumns;
    bpp->tlJoiners = tlJoiners;
    bpp->tlSmallSideKeyLengths = tlSmallSideKeyLengths;
    bpp->storedKeyAllocators = storedKeyAllocators;
    bpp->joinNullValues = joinNullValues;
    bpp->doMatchNulls = doMatchNulls;
    bpp->hasJoinFEFilters = hasJoinFEFilters;
    bpp->hasSmallOuterJoin = hasSmallOuterJoin;
    bpp->mJOINHasSkewedKeyColumn = mJOINHasSkewedKeyColumn;
    bpp->mSmallSideRGPtr = mSmallSideRGPtr;
    bpp->mSmallSideKeyColumnsPtr = mSmallSideKeyColumnsPtr;
    if (!getTupleJoinRowGroupData && mJOINHasSkewedKeyColumn)
    {
      idbassert(!smallSideRGs.empty());
      bpp->smallSideRGs.push_back(smallSideRGs[0]);
    }

    if (hasJoinFEFilters)
    {
      bpp->joinFERG = joinFERG;
      bpp->joinFEFilters.reset(new scoped_ptr<FuncExpWrapper>[joinerCount]);

      for (i = 0; i < joinerCount; i++)
        if (joinFEFilters[i])
          bpp->joinFEFilters[i].reset(new FuncExpWrapper(*joinFEFilters[i]));
    }

    if (getTupleJoinRowGroupData)
    {
      bpp->smallSideRGs = smallSideRGs;
      bpp->largeSideRG = largeSideRG;
      bpp->smallSideRowLengths = smallSideRowLengths;
      bpp->smallSideRowData = smallSideRowData;
      bpp->smallNullRowData = smallNullRowData;
      bpp->smallNullPointers = smallNullPointers;
      bpp->joinedRG = joinedRG;
    }

#ifdef __FreeBSD__
    pthread_mutex_unlock(&bpp->objLock);
#endif
  }

  bpp->filterCount = filterCount;
  bpp->filterSteps.resize(filterCount);

  for (i = 0; i < filterCount; ++i)
    bpp->filterSteps[i] = filterSteps[i]->duplicate();

  bpp->projectCount = projectCount;
  bpp->projectSteps.resize(projectCount);

  for (i = 0; i < projectCount; ++i)
    bpp->projectSteps[i] = projectSteps[i]->duplicate();

  if (fAggregator.get() != nullptr)
  {
    bpp->fAggregateRG = fAggregateRG;
    bpp->fAggregator.reset(new RowAggregation(fAggregator->getGroupByCols(), fAggregator->getAggFunctions()));
    bpp->fAggregator->timeZone(fAggregator->timeZone());
  }

  bpp->sendRidsAtDelivery = sendRidsAtDelivery;
  bpp->prefetchThreshold = prefetchThreshold;

  bpp->sock = sock;
  bpp->writelock = writelock;
  bpp->hasDictStep = hasDictStep;
  bpp->sendThread = sendThread;
  bpp->newConnection = true;
  bpp->initProcessor();
  return bpp;
}

void BatchPrimitiveProcessor::asyncLoadProjectColumns()
{
  // relLBID is the LBID related to the primMsg->LBID,
  // it is used to keep the read ahead boundary for asyncLoads
  // 1. scan driven case: load blocks in # to (# + blocksReadAhead - 1) range,
  //    where # is a multiple of ColScanReadAheadBlocks in Columnstore.xml
  // 2. non-scan driven case: load blocks in the logical block.
  //    because 1 logical block per primMsg, asyncLoad only once per message.
  for (uint64_t i = 0; i < projectCount; ++i)
  {
    // only care about column commands
    ColumnCommand* col = dynamic_cast<ColumnCommand*>(projectSteps[i].get());

    if (col != nullptr)
    {
      asyncLoaded[i] = asyncLoaded[i] && (relLBID[i] % blocksReadAhead != 0);
      relLBID[i] += col->getWidth();

      if (!asyncLoaded[i] && col->willPrefetch())
      {
        loadBlockAsync(col->getLBID(), versionInfo, txnID, col->getCompType(), &cachedIO, &physIO, LBIDTrace,
                       sessionID, &counterLock, &busyLoaderCount, sendThread, &vssCache);
        asyncLoaded[i] = true;
      }
    }
  }
}

bool BatchPrimitiveProcessor::generateJoinedRowGroup(rowgroup::Row& baseRow, const uint32_t depth)
{
  Row& smallRow = smallRows[depth];
  const bool lowestLvl = (depth == joinerCount - 1);

  while (gjrgRowNumber < ridCount &&
         gjrgPlaceHolders[depth] < tSmallSideMatches[depth][gjrgRowNumber].size() && !gjrgFull)
  {
    const vector<uint32_t>& results = tSmallSideMatches[depth][gjrgRowNumber];
    const uint32_t size = results.size();

    if (depth == 0)
    {
      outputRG.getRow(gjrgRowNumber, &largeRow);
      applyMapping(gjrgMappings[joinerCount], largeRow, &baseRow);
      baseRow.setRid(largeRow.getRelRid());
    }

    // cout << "rowNum = " << gjrgRowNumber << " at depth " << depth << " size is " << size << endl;
    for (uint32_t& i = gjrgPlaceHolders[depth]; i < size && !gjrgFull; i++)
    {
      if (results[i] != (uint32_t)-1)
      {
        smallSideRGs[depth].getRow(results[i], &smallRow);
        // rowOffset = ((uint64_t) results[i]) * smallRowSize;
        // smallRow.setData(&rowDataAtThisLvl.rowData[rowOffset] + emptySize);
      }
      else
        smallRow.setPointer(smallNullPointers[depth]);

      // cout << "small row: " << smallRow.toString() << endl;
      applyMapping(gjrgMappings[depth], smallRow, &baseRow);

      if (!lowestLvl)
        generateJoinedRowGroup(baseRow, depth + 1);
      else
      {
        copyRow(baseRow, &joinedRow);
        // memcpy(joinedRow.getData(), baseRow.getData(), joinedRow.getSize());
        // cerr << "joined row " << joinedRG.getRowCount() << ": " << joinedRow.toString() << endl;
        joinedRow.nextRow();
        joinedRG.incRowCount();

        if (joinedRG.getRowCount() == 8192)
        {
          i++;
          gjrgFull = true;
        }
      }

      if (gjrgFull)
        break;
    }

    if (depth == 0 && gjrgPlaceHolders[0] == tSmallSideMatches[0][gjrgRowNumber].size())
    {
      gjrgPlaceHolders[0] = 0;
      gjrgRowNumber++;
    }
  }

  // 	if (depth == 0)
  // 		cout << "gjrg returning " << (uint32_t) gjrgFull << endl;
  if (!gjrgFull)
    gjrgPlaceHolders[depth] = 0;

  return gjrgFull;
}

void BatchPrimitiveProcessor::resetGJRG()
{
  gjrgFull = false;
  joinedRG.resetRowGroup(baseRid);
  joinedRG.getRow(0, &joinedRow);
  joinedRG.setDBRoot(dbRoot);
}

void BatchPrimitiveProcessor::initGJRG()
{
  for (uint32_t z = 0; z < joinerCount; z++)
    gjrgPlaceHolders[z] = 0;

  gjrgRowNumber = 0;
}

inline void BatchPrimitiveProcessor::getJoinResults(const Row& r, uint32_t jIndex, vector<uint32_t>& v)
{
  uint bucket;

  if (!typelessJoin[jIndex])
  {
    if (r.isNullValue(largeSideKeyColumns[jIndex]))
    {
      /* Bug 3524. This matches everything. */
      if (joinTypes[jIndex] & ANTI)
      {
        TJoiner::iterator it;

        for (uint i = 0; i < processorThreads; ++i)
          for (it = tJoiners[jIndex][i]->begin(); it != tJoiners[jIndex][i]->end(); ++it)
            v.push_back(it->second);

        return;
      }
      else
        return;
    }

    uint64_t largeKey;
    uint32_t colIndex = largeSideKeyColumns[jIndex];

    if (r.isUnsigned(colIndex))
    {
      largeKey = r.getUintField(colIndex);
    }
    else
    {
      largeKey = r.getIntField(colIndex);
    }

    bucket = bucketPicker((char*)&largeKey, 8, bpSeed) & ptMask;
    pair<TJoiner::iterator, TJoiner::iterator> range = tJoiners[jIndex][bucket]->equal_range(largeKey);
    for (; range.first != range.second; ++range.first)
      v.push_back(range.first->second);

    if (doMatchNulls[jIndex])  // add the nulls to the match list
    {
      bucket = bucketPicker((char*)&joinNullValues[jIndex], 8, bpSeed) & ptMask;
      range = tJoiners[jIndex][bucket]->equal_range(joinNullValues[jIndex]);
      for (; range.first != range.second; ++range.first)
        v.push_back(range.first->second);
    }
  }
  else
  {
    /* Bug 3524. Large-side NULL + ANTI join matches everything. */
    if (joinTypes[jIndex] & ANTI)
    {
      bool hasNullValue = false;

      for (unsigned int column : tlLargeSideKeyColumns[jIndex])
      {
        if (r.isNullValue(column))
        {
          hasNullValue = true;
          break;
        }
      }

      if (hasNullValue)
      {
        TLJoiner::iterator it;
        for (uint i = 0; i < processorThreads; ++i)
          for (it = tlJoiners[jIndex][i]->begin(); it != tlJoiners[jIndex][i]->end(); ++it)
            v.push_back(it->second);

        return;
      }
    }

    TypelessData largeKey(&r);
    bucket = r.hashTypeless(tlLargeSideKeyColumns[jIndex], mSmallSideKeyColumnsPtr,
                            mSmallSideRGPtr ? &mSmallSideRGPtr->getColWidths() : nullptr) &
             ptMask;
    pair<TLJoiner::iterator, TLJoiner::iterator> range = tlJoiners[jIndex][bucket]->equal_range(largeKey);
    for (; range.first != range.second; ++range.first)
      v.push_back(range.first->second);
  }
}

void BatchPrimitiveProcessor::buildVSSCache(uint32_t loopCount)
{
  vector<int64_t> lbidList;
  vector<BRM::VSSData> vssData;
  uint32_t i;
  int rc;

  for (i = 0; i < filterCount; i++)
    filterSteps[i]->getLBIDList(loopCount, &lbidList);

  for (i = 0; i < projectCount; i++)
    projectSteps[i]->getLBIDList(loopCount, &lbidList);

  rc = brm->bulkVSSLookup(lbidList, versionInfo, (int)txnID, &vssData);

  if (rc == 0)
    for (i = 0; i < vssData.size(); i++)
      vssCache.insert(make_pair(lbidList[i], vssData[i]));
}

}  // namespace primitiveprocessor

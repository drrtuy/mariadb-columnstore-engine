/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2016 MariaDB Corporation

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

/***********************************************************************
 *   $Id: primitiveserver.cpp 2147 2013-08-14 20:44:44Z bwilkinson $
 *
 *
 ***********************************************************************/

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>

// #define NDEBUG
#include <cassert>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <boost/foreach.hpp>
#include <tr1/unordered_map>
#include <tr1/unordered_set>
#include <pthread.h>
#include <cerrno>

using namespace std;

#include <boost/scoped_ptr.hpp>
#include <boost/scoped_array.hpp>
#include <utility>
#include <boost/thread.hpp>
using namespace boost;
#include "distributedenginecomm.h"
#include "serviceexemgr.h"
#include "primproc.h"
#include "primitiveserver.h"
#include "primitivemsg.h"
#include "umsocketselector.h"
#include "brm.h"
using namespace BRM;

#include "writeengine.h"

#include "messagequeue.h"
#include "samenodepseudosocket.h"
using namespace messageqcpp;

#include "blockrequestprocessor.h"
#include "blockcacheclient.h"
#include "stats.h"
using namespace dbbc;

#include "liboamcpp.h"
using namespace oam;

#include "configcpp.h"
using namespace config;

#include "bppseeder.h"
#include "primitiveprocessor.h"
#include "pp_logger.h"
using namespace primitives;

#include "errorcodes.h"
#include "exceptclasses.h"

#include "idbcompress.h"
using namespace compress;

#include "IDBDataFile.h"
#include "IDBPolicy.h"
using namespace idbdatafile;

using namespace threadpool;

#include "threadnaming.h"

#include "atomicops.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif

// make global for blockcache
//
static const char* statsName = {"pm"};
dbbc::Stats* gPMStatsPtr = nullptr;
bool gPMProfOn = false;
uint32_t gSession = 0;
dbbc::Stats pmstats(statsName);

// FIXME: there is an anon ns burried later in between 2 named namespaces...
namespace primitiveprocessor
{

BlockRequestProcessor** BRPp;
dbbc::Stats stats;
extern DebugLevel gDebugLevel;
BRM::DBRM* brm;
int fCacheCount;
bool fPMProfOn;
bool fLBIDTraceOn;

/* params from the config file */
uint32_t BPPCount;
uint32_t blocksReadAhead;
uint32_t defaultBufferSize;
uint32_t connectionsPerUM;
uint32_t highPriorityThreads;
uint32_t medPriorityThreads;
uint32_t lowPriorityThreads;
int directIOFlag = O_DIRECT;
int noVB = 0;

BPPMap bppMap;
boost::mutex bppLock;

boost::mutex djMutex;                             // lock for djLock, lol.
std::map<uint64_t, boost::shared_mutex*> djLock;  // djLock synchronizes destroy and joiner msgs, see bug 2619

volatile int32_t asyncCounter;
const int asyncMax = 20;  // current number of asynchronous loads

struct preFetchCond
{
  // uint64_t lbid;
  boost::condition cond;
  unsigned waiters;

  preFetchCond(const uint64_t)
  {
    waiters = 0;
  }

  ~preFetchCond() = default;
};

typedef preFetchCond preFetchBlock_t;
typedef std::tr1::unordered_map<uint64_t, preFetchBlock_t*> pfBlockMap_t;
typedef std::tr1::unordered_map<uint64_t, preFetchBlock_t*>::iterator pfBlockMapIter_t;

pfBlockMap_t pfBlockMap;
boost::mutex pfbMutex;  // = PTHREAD_MUTEX_INITIALIZER;

pfBlockMap_t pfExtentMap;
boost::mutex pfMutex;  // = PTHREAD_MUTEX_INITIALIZER;

map<uint32_t, boost::shared_ptr<DictEqualityFilter> > dictEqualityFilters;
boost::mutex eqFilterMutex;

uint32_t cacheNum(uint64_t lbid)
{
  return (lbid / brm->getExtentSize()) % fCacheCount;
}

void buildOidFileName(const BRM::OID_t oid, const uint16_t dbRoot, const uint16_t partNum,
                      const uint32_t segNum, char* file_name)
{
  WriteEngine::FileOp fileOp(false);

  if (fileOp.getFileName(oid, file_name, dbRoot, partNum, segNum) != WriteEngine::NO_ERROR)
  {
    file_name[0] = 0;
    throw std::runtime_error("fileOp.getFileName failed");
  }

  // cout << "Oid2Filename o: " << oid << " n: " << file_name << endl;
}

void waitForRetry(long count)
{
  timespec ts;
  ts.tv_sec = 5L * count / 10L;
  ts.tv_nsec = (5L * count % 10L) * 100000000L;
  nanosleep(&ts, nullptr);
}

void prefetchBlocks(const uint64_t lbid, const int compType, uint32_t* rCount)
{
  uint16_t dbRoot;
  uint32_t partNum;
  uint16_t segNum;
  uint32_t hwm;
  uint32_t fbo;
  uint32_t lowfbo;
  uint32_t highfbo;
  BRM::OID_t oid;
  pfBlockMap_t::const_iterator iter;
  uint64_t lowlbid = (lbid / blocksReadAhead) * blocksReadAhead;
  blockCacheClient bc(*BRPp[cacheNum(lbid)]);
  BRM::InlineLBIDRange range;
  int err;

  pfbMutex.lock();

  iter = pfBlockMap.find(lowlbid);

  if (iter != pfBlockMap.end())
  {
    iter->second->waiters++;
    iter->second->cond.wait(pfbMutex);
    iter->second->waiters--;
    pfbMutex.unlock();
    return;
  }

  preFetchBlock_t* pfb = nullptr;
  pfb = new preFetchBlock_t(lowlbid);

  pfBlockMap[lowlbid] = pfb;
  pfbMutex.unlock();

  // loadBlock will catch a versioned block so vbflag can be set to false here
  err = brm->lookupLocal(lbid, 0, false, oid, dbRoot, partNum, segNum, fbo);  // need the oid

  if (err < 0)
  {
    cerr << "prefetchBlocks(): BRM lookupLocal failed! Expect more errors.\n";
    goto cleanup;
  }

  // We ignore extState that tells us whether the extent is
  // an outOfService extent to be ignored.  The filtering for
  // outOfService extents is done elsewhere.
  int extState;
  err = brm->getLocalHWM(oid, partNum, segNum, hwm, extState);

  if (err < 0)
  {
    cerr << "prefetchBlock(): BRM getLocalHWM failed! Expect more errors.\n";
    goto cleanup;
  }

  lowfbo = fbo - (lbid - lowlbid);
  highfbo = lowfbo + blocksReadAhead - 1;
  range.start = lowlbid;

  if (hwm < highfbo)
    range.size = hwm - lowfbo + 1;
  else
    range.size = blocksReadAhead;

  try
  {
    if (range.size > blocksReadAhead)
    {
      ostringstream os;
      os << "Invalid Range from HWM for lbid " << lbid << ", range size should be <= blocksReadAhead: HWM "
         << hwm << ", dbroot " << dbRoot << ", highfbo " << highfbo << ", lowfbo " << lowfbo
         << ", blocksReadAhead " << blocksReadAhead << ", range size " << (int)range.size << endl;
      throw logging::InvalidRangeHWMExcept(os.str());
    }

    idbassert(range.size <= blocksReadAhead);

    bc.check(range, QueryContext(numeric_limits<VER_t>::max()), 0, compType, *rCount);
  }
  catch (...)
  {
    // Perform necessary cleanup before rethrowing the exception
    pfb->cond.notify_all();

    pfbMutex.lock();

    while (pfb->waiters > 0)
    {
      pfbMutex.unlock();
      // handle race condition with other threads going into wait before the broadcast above
      pfb->cond.notify_one();
      usleep(1);
      pfbMutex.lock();
    }

    if (pfBlockMap.erase(lowlbid) > 0)
      delete pfb;

    pfb = nullptr;
    pfbMutex.unlock();
    throw;
  }

cleanup:
  pfb->cond.notify_all();

  pfbMutex.lock();

  while (pfb->waiters > 0)
  {
    pfbMutex.unlock();
    // handle race condition with other threads going into wait before the broadcast above
    pfb->cond.notify_one();
    usleep(1);
    pfbMutex.lock();
  }

  if (pfBlockMap.erase(lowlbid) > 0)
    delete pfb;

  pfb = nullptr;
  pfbMutex.unlock();

}  // prefetchBlocks()

// returns the # that were cached.
uint32_t loadBlocks(LBID_t* lbids, QueryContext qc, VER_t txn, int compType, uint8_t** bufferPtrs,
                    uint32_t* rCount, bool LBIDTrace, uint32_t sessionID, uint32_t blockCount,
                    bool* blocksWereVersioned, bool doPrefetch, VSSCache* vssCache)
{
  blockCacheClient bc(*BRPp[cacheNum(lbids[0])]);
  uint32_t blksRead = 0;
  VSSCache::iterator it;
  uint32_t i, ret;
  bool* vbFlags;
  int* vssRCs;
  bool* cacheThisBlock;
  bool* wasCached;

  *blocksWereVersioned = false;

  if (LBIDTrace)
  {
    for (i = 0; i < blockCount; i++)
    {
      stats.touchedLBID(lbids[i], pthread_self(), sessionID);
    }
  }

  VER_t* vers = (VER_t*)alloca(blockCount * sizeof(VER_t));
  vbFlags = (bool*)alloca(blockCount);
  vssRCs = (int*)alloca(blockCount * sizeof(int));
  cacheThisBlock = (bool*)alloca(blockCount);
  wasCached = (bool*)alloca(blockCount);

  for (i = 0; i < blockCount; i++)
  {
    if (vssCache)
    {
      it = vssCache->find(lbids[i]);

      if (it != vssCache->end())
      {
        VSSData& vd = it->second;
        vers[i] = vd.verID;
        vbFlags[i] = vd.vbFlag;
        vssRCs[i] = vd.returnCode;

        if (vssRCs[i] == ERR_SNAPSHOT_TOO_OLD)
          throw runtime_error("Snapshot too old");
      }
    }

    if (!vssCache || it == vssCache->end())
      vssRCs[i] = brm->vssLookup(lbids[i], qc, txn, &vers[i], &vbFlags[i]);

    *blocksWereVersioned |= vbFlags[i];

    // If the block is being modified by this txn, set the useCache flag to false
    if (txn > 0 && vers[i] == txn && !vbFlags[i])
      cacheThisBlock[i] = false;
    else
      cacheThisBlock[i] = true;
  }

  /*
  cout << "  resolved ver #s: ";
  for (uint32_t i = 0; i < blockCount; i++)
      cout << " <" << vers[i] << ", " << (int) vbFlags[i] << ", " << (int)
              cacheThisBlock[i] << ">";
  cout << endl;
  */

  ret = bc.getCachedBlocks(lbids, vers, bufferPtrs, wasCached, blockCount);

  // Do we want to check any VB flags here?  Initial thought: no, because we have
  // no idea whether any other blocks in the prefetch range are versioned,
  // what's the difference if one in the visible range is?
  if (ret != blockCount && doPrefetch)
  {
    prefetchBlocks(lbids[0], compType, &blksRead);

    if (fPMProfOn)
      pmstats.markEvent(lbids[0], (pthread_t)-1, sessionID, 'M');

    /* After the prefetch they're all cached if they are in the same range, so
     * prune the block list and try getCachedBlocks again first, then fall back
     * to single-block IO requests if for some reason they aren't. */
    uint32_t l_blockCount = 0;

    for (i = 0; i < blockCount; i++)
    {
      if (!wasCached[i])
      {
        lbids[l_blockCount] = lbids[i];
        vers[l_blockCount] = vers[i];
        bufferPtrs[l_blockCount] = bufferPtrs[i];
        vbFlags[l_blockCount] = vbFlags[i];
        cacheThisBlock[l_blockCount] = cacheThisBlock[i];
        ++l_blockCount;
      }
    }

    ret += bc.getCachedBlocks(lbids, vers, bufferPtrs, wasCached, l_blockCount);

    if (ret != blockCount)
    {
      for (i = 0; i < l_blockCount; i++)
        if (!wasCached[i])
        {
          bool ver;

          qc.currentScn = vers[i];
          bc.getBlock(lbids[i], qc, txn, compType, (void*)bufferPtrs[i], vbFlags[i], wasCached[i], &ver,
                      cacheThisBlock[i], false);
          *blocksWereVersioned |= ver;
          blksRead++;
        }
    }
  }
  /* Some blocks weren't cached, prefetch is disabled -> issue single-block IO requests,
   * skip checking the cache again. */
  else if (ret != blockCount)
  {
    for (i = 0; i < blockCount; i++)
    {
      if (!wasCached[i])
      {
        bool ver;

        qc.currentScn = vers[i];
        bc.getBlock(lbids[i], qc, txn, compType, (void*)bufferPtrs[i], vbFlags[i], wasCached[i], &ver,
                    cacheThisBlock[i], false);
        *blocksWereVersioned |= ver;
        blksRead++;
      }
    }
  }

  if (rCount)
    *rCount = blksRead;

  // if (*verBlocks)
  //	cout << "loadBlock says there were versioned blocks\n";
  return ret;
}

void loadBlock(uint64_t lbid, QueryContext v, uint32_t t, int compType, void* bufferPtr,
               bool* pWasBlockInCache, uint32_t* rCount, bool LBIDTrace, uint32_t sessionID, bool doPrefetch,
               VSSCache* vssCache)
{
  bool flg = false;
  BRM::OID_t oid;
  BRM::VER_t txn = (BRM::VER_t)t;
  uint16_t dbRoot = 0;
  uint32_t partitionNum = 0;
  uint16_t segmentNum = 0;
  int rc;
  BRM::VER_t ver;
  blockCacheClient bc(*BRPp[cacheNum(lbid)]);
  char file_name[WriteEngine::FILE_NAME_SIZE] = {0};
  char* fileNamePtr = file_name;
  uint32_t blksRead = 0;
  VSSCache::iterator it;

  if (LBIDTrace)
    stats.touchedLBID(lbid, pthread_self(), sessionID);

  if (vssCache)
  {
    it = vssCache->find(lbid);

    if (it != vssCache->end())
    {
      VSSData& vd = it->second;
      ver = vd.verID;
      flg = vd.vbFlag;
      rc = vd.returnCode;
    }
  }

  if (!vssCache || it == vssCache->end())
    rc = brm->vssLookup((BRM::LBID_t)lbid, v, txn, &ver, &flg);

  v.currentScn = ver;
  // cout << "VSS l/u: l=" << lbid << " v=" << ver << " t=" << txn << " flg=" << flg << " rc: " << rc << endl;

  // if this block is locked by this session, don't cache it, just read it directly from disk
  if (txn > 0 && ver == txn && !flg && !noVB)
  {
    uint64_t offset;
    uint32_t fbo;
    boost::scoped_array<uint8_t> newBufferSa;
    boost::scoped_array<char> cmpHdrBufSa;
    boost::scoped_array<char> cmpBufSa;
    boost::scoped_array<unsigned char> uCmpBufSa;

    ptrdiff_t alignedBuffer = 0;
    void* readBufferPtr = nullptr;
    char* cmpHdrBuf = nullptr;
    char* cmpBuf = nullptr;
    unsigned char* uCmpBuf = nullptr;
    uint64_t cmpBufLen = 0;
    int blockReadRetryCount = 0;
    unsigned idx = 0;
    int pageSize = getpagesize();
    IDBDataFile* fp = nullptr;

    try
    {
      rc = brm->lookupLocal((BRM::LBID_t)lbid, ver, flg, oid, dbRoot, partitionNum, segmentNum, fbo);

      // load the block
      buildOidFileName(oid, dbRoot, partitionNum, segmentNum, fileNamePtr);
      int opts = directIOFlag ? IDBDataFile::USE_ODIRECT : 0;
      fp = IDBDataFile::open(IDBPolicy::getType(fileNamePtr, IDBPolicy::PRIMPROC), fileNamePtr, "r", opts);

      if (fp == nullptr)
      {
        int errCode = errno;
        SUMMARY_INFO2("open failed: ", fileNamePtr);
        char errbuf[80];
        string errMsg;
        // #if STRERROR_R_CHAR_P
        const char* p;

        if ((p = strerror_r(errCode, errbuf, 80)) != nullptr)
          errMsg = p;

        if (errCode == EINVAL)
        {
          throw logging::IDBExcept(logging::IDBErrorInfo::instance()->errorMsg(logging::ERR_O_DIRECT),
                                   logging::ERR_O_DIRECT);
        }

        string errStr(fileNamePtr);
        errStr += ": open: ";
        errStr += errMsg;
        throw std::runtime_error(errStr);
      }

      //  fd >= 0 must be true, otherwise above exception thrown.
      offset = (uint64_t)fbo * (uint64_t)DATA_BLOCK_SIZE;
      idx = offset / (4 * 1024 * 1024);

      errno = 0;
      rc = 0;
      int i = -1;

      if (compType == 0)
      {
        newBufferSa.reset(new uint8_t[DATA_BLOCK_SIZE + pageSize]);
        alignedBuffer = (ptrdiff_t)newBufferSa.get();

        if ((alignedBuffer % pageSize) != 0)
        {
          alignedBuffer &= ~((ptrdiff_t)pageSize - 1);
          alignedBuffer += pageSize;
        }

        readBufferPtr = (void*)alignedBuffer;
        i = fp->pread(readBufferPtr, offset, DATA_BLOCK_SIZE);
        memcpy(bufferPtr, readBufferPtr, i);
#ifdef IDB_COMP_POC_DEBUG
        {
          boost::mutex::scoped_lock lk(primitiveprocessor::compDebugMutex);
          cout << "pread2(" << fd << ", 0x" << hex << (ptrdiff_t)readBufferPtr << dec << ", "
               << DATA_BLOCK_SIZE << ", " << offset << ") = " << i << endl;
        }
#endif  // IDB_COMP_POC_DEBUG
      }  // if (compType == 0)
      else  // if (compType != 0)
      {
      // retry if file is out of sync -- compressed column file only.
      blockReadRetry:

        uCmpBufSa.reset(new unsigned char[4 * 1024 * 1024 + 4]);
        uCmpBuf = uCmpBufSa.get();
        cmpHdrBufSa.reset(new char[4096 * 3 + pageSize]);
        alignedBuffer = (ptrdiff_t)cmpHdrBufSa.get();

        if ((alignedBuffer % pageSize) != 0)
        {
          alignedBuffer &= ~((ptrdiff_t)pageSize - 1);
          alignedBuffer += pageSize;
        }

        cmpHdrBuf = (char*)alignedBuffer;

        i = fp->pread(&cmpHdrBuf[0], 0, 4096 * 3);

        CompChunkPtrList ptrList;
        std::unique_ptr<CompressInterface> decompressor(compress::getCompressInterfaceByType(
            compress::CompressInterface::getCompressionType(&cmpHdrBuf[0])));

        if (!decompressor)
        {
          // Use default?
          decompressor.reset(new compress::CompressInterfaceSnappy());
        }

        int dcrc = 0;

        if (i == 4096 * 3)
        {
          uint64_t numHdrs = 0;  // extra headers
          dcrc = compress::CompressInterface::getPtrList(&cmpHdrBuf[4096], 4096, ptrList);

          if (dcrc == 0 && ptrList.size() > 0)
            numHdrs = ptrList[0].first / 4096ULL - 2ULL;

          if (numHdrs > 0)
          {
            boost::scoped_array<char> nextHdrBufsa(new char[numHdrs * 4096 + pageSize]);
            alignedBuffer = (ptrdiff_t)nextHdrBufsa.get();

            if ((alignedBuffer % pageSize) != 0)
            {
              alignedBuffer &= ~((ptrdiff_t)pageSize - 1);
              alignedBuffer += pageSize;
            }

            char* nextHdrBufPtr = (char*)alignedBuffer;

            i = fp->pread(&nextHdrBufPtr[0], 4096 * 2, numHdrs * 4096);

            CompChunkPtrList nextPtrList;
            dcrc = compress::CompressInterface::getPtrList(&nextHdrBufPtr[0], numHdrs * 4096, nextPtrList);

            if (dcrc == 0)
              ptrList.insert(ptrList.end(), nextPtrList.begin(), nextPtrList.end());
          }
        }

        if (dcrc != 0 || idx >= ptrList.size())
        {
          // Due to race condition, the header on disk may not upated yet.
          // Log an info message and retry.
          if (blockReadRetryCount == 0)
          {
            logging::Message::Args args;
            args.add(oid);
            ostringstream infoMsg;
            infoMsg << "retry read from " << fileNamePtr << ". dcrc=" << dcrc << ", idx=" << idx
                    << ", ptr.size=" << ptrList.size();
            args.add(infoMsg.str());
            mlp->logInfoMessage(logging::M0061, args);
          }

          if (++blockReadRetryCount < 30)
          {
            waitForRetry(blockReadRetryCount);
            goto blockReadRetry;
          }
          else
          {
            rc = -1004;
          }
        }

        if (rc == 0)
        {
          unsigned cmpBlkOff = offset % (4 * 1024 * 1024);
          uint64_t cmpBufOff = ptrList[idx].first;
          uint64_t cmpBufSz = ptrList[idx].second;

          if (cmpBufSa.get() == nullptr || cmpBufLen < cmpBufSz)
          {
            cmpBufSa.reset(new char[cmpBufSz + pageSize]);
            cmpBufLen = cmpBufSz;
            alignedBuffer = (ptrdiff_t)cmpBufSa.get();

            if ((alignedBuffer % pageSize) != 0)
            {
              alignedBuffer &= ~((ptrdiff_t)pageSize - 1);
              alignedBuffer += pageSize;
            }

            cmpBuf = (char*)alignedBuffer;
          }

          size_t blen = 4 * 1024 * 1024;

          i = fp->pread(cmpBuf, cmpBufOff, cmpBufSz);

          dcrc = decompressor->uncompressBlock(cmpBuf, cmpBufSz, uCmpBuf, blen);

          if (dcrc == 0)
          {
            memcpy(bufferPtr, &uCmpBuf[cmpBlkOff], DATA_BLOCK_SIZE);
          }
          else
          {
            // Due to race condition, the header on disk may not upated yet.
            // Log an info message and retry.
            if (blockReadRetryCount == 0)
            {
              logging::Message::Args args;
              args.add(oid);
              ostringstream infoMsg;
              infoMsg << "retry read from " << fileNamePtr << ". dcrc=" << dcrc << ", idx=" << idx
                      << ", ptr.size=" << ptrList.size();
              args.add(infoMsg.str());
              mlp->logInfoMessage(logging::M0061, args);
            }

            if (++blockReadRetryCount < 30)
            {
              waitForRetry(blockReadRetryCount);
              goto blockReadRetry;
            }
            else
            {
              rc = -1006;
            }
          }
        }
      }

      if (rc < 0)
      {
        string msg("pread failed");
        ostringstream infoMsg;
        infoMsg << " rc:" << rc << ")";
        msg = msg + ", error:" + strerror(errno) + infoMsg.str();
        SUMMARY_INFO(msg);
        // FIXME: free-up allocated memory!
        throw std::runtime_error(msg);
      }
    }
    catch (...)
    {
      delete fp;
      fp = nullptr;
      throw;
    }

    delete fp;
    fp = nullptr;

    // log the retries
    if (blockReadRetryCount > 0)
    {
      logging::Message::Args args;
      args.add(oid);
      ostringstream infoMsg;
      infoMsg << "Successfully uncompress " << fileNamePtr << " chunk " << idx << " @"
              << " blockReadRetry:" << blockReadRetryCount;
      args.add(infoMsg.str());
      mlp->logInfoMessage(logging::M0006, args);
    }

    if (pWasBlockInCache)
      *pWasBlockInCache = false;

    if (rCount)
      *rCount = 1;

    return;
  }

  FileBuffer* fbPtr = nullptr;
  bool wasBlockInCache = false;

  fbPtr = bc.getBlockPtr(lbid, ver, flg);

  if (fbPtr)
  {
    memcpy(bufferPtr, fbPtr->getData(), BLOCK_SIZE);
    wasBlockInCache = true;
  }

  if (doPrefetch && !wasBlockInCache && !flg)
  {
    prefetchBlocks(lbid, compType, &blksRead);

    if (fPMProfOn)
      pmstats.markEvent(lbid, (pthread_t)-1, sessionID, 'M');

    bc.getBlock(lbid, v, txn, compType, (uint8_t*)bufferPtr, flg, wasBlockInCache);

    if (!wasBlockInCache)
      blksRead++;
  }
  else if (!wasBlockInCache)
  {
    bc.getBlock(lbid, v, txn, compType, (uint8_t*)bufferPtr, flg, wasBlockInCache);

    if (!wasBlockInCache)
      blksRead++;
  }

  if (pWasBlockInCache)
    *pWasBlockInCache = wasBlockInCache;

  if (rCount)
    *rCount = blksRead;
}

struct AsynchLoader
{
  AsynchLoader(uint64_t l, QueryContext v, uint32_t t, int ct, uint32_t* cCount, uint32_t* rCount, bool trace,
               uint32_t /*sesID*/, boost::mutex* m, uint32_t* loaderCount,
               boost::shared_ptr<BPPSendThread> st,  // sendThread for abort upon exception.
               VSSCache* vCache)
   : lbid(l)
   , ver(std::move(v))
   , txn(t)
   , compType(ct)
   , LBIDTrace(trace)
   , cacheCount(cCount)
   , readCount(rCount)
   , busyLoaders(loaderCount)
   , mutex(m)
   , sendThread(std::move(st))
   , vssCache(vCache)
  {
  }

  void operator()()
  {
    utils::setThreadName("PPAsyncLoader");
    bool cached = false;
    uint32_t rCount = 0;
    char buf[BLOCK_SIZE];

    // cout << "asynch started " << pthread_self() << " l: " << lbid << endl;
    try
    {
      loadBlock(lbid, ver, txn, compType, buf, &cached, &rCount, LBIDTrace, true, vssCache);
    }
    catch (std::exception& ex)
    {
      sendThread->abort();
      cerr << "AsynchLoader caught loadBlock exception: " << ex.what() << endl;
      idbassert(asyncCounter > 0);
      (void)atomicops::atomicDec(&asyncCounter);
      mutex->lock();
      --(*busyLoaders);
      mutex->unlock();
      logging::Message::Args args;
      args.add(string("PrimProc AsyncLoader caught error: "));
      args.add(ex.what());
      primitiveprocessor::mlp->logMessage(logging::M0000, args, false);
      return;
    }
    catch (...)
    {
      sendThread->abort();
      cerr << "AsynchLoader caught unknown exception: " << endl;
      // FIXME Use a locked processor primitive?
      idbassert(asyncCounter > 0);
      (void)atomicops::atomicDec(&asyncCounter);
      mutex->lock();
      --(*busyLoaders);
      mutex->unlock();
      logging::Message::Args args;
      args.add(string("PrimProc AsyncLoader caught unknown error"));
      primitiveprocessor::mlp->logMessage(logging::M0000, args, false);
      return;
    }

    idbassert(asyncCounter > 0);
    (void)atomicops::atomicDec(&asyncCounter);
    mutex->lock();

    if (cached)
      (*cacheCount)++;

    *readCount += rCount;
    --(*busyLoaders);
    mutex->unlock();
    // 			cerr << "done\n";
  }

 private:
  uint64_t lbid;
  QueryContext ver;
  uint32_t txn;
  int compType;
  bool LBIDTrace;
  uint32_t* cacheCount;
  uint32_t* readCount;
  uint32_t* busyLoaders;
  boost::mutex* mutex;
  boost::shared_ptr<BPPSendThread> sendThread;
  VSSCache* vssCache;
};

void loadBlockAsync(uint64_t lbid, const QueryContext& c, uint32_t txn, int compType, uint32_t* cCount,
                    uint32_t* rCount, bool LBIDTrace, uint32_t sessionID, boost::mutex* m,
                    uint32_t* busyLoaders,
                    boost::shared_ptr<BPPSendThread> sendThread,  // sendThread for abort upon exception.
                    VSSCache* vssCache)
{
  blockCacheClient bc(*BRPp[cacheNum(lbid)]);
  bool vbFlag;
  BRM::VER_t ver;
  VSSCache::iterator it;

  if (vssCache)
  {
    it = vssCache->find(lbid);

    if (it != vssCache->end())
    {
      // cout << "async: vss cache hit on " << lbid << endl;
      VSSData& vd = it->second;
      ver = vd.verID;
      vbFlag = vd.vbFlag;
    }
  }

  if (!vssCache || it == vssCache->end())
    brm->vssLookup((BRM::LBID_t)lbid, c, txn, &ver, &vbFlag);

  if (bc.exists(lbid, ver))
    return;

  /* a quick and easy stand-in for a threadpool for loaders */
  atomicops::atomicMb();

  if (asyncCounter >= asyncMax)
    return;

  (void)atomicops::atomicInc(&asyncCounter);

  boost::mutex::scoped_lock sl(*m);

  try
  {
    boost::thread thd(AsynchLoader(lbid, c, txn, compType, cCount, rCount, LBIDTrace, sessionID, m,
                                   busyLoaders, sendThread, vssCache));
    (*busyLoaders)++;
  }
  catch (boost::thread_resource_error& e)
  {
    cerr << "AsynchLoader: caught a thread resource error, need to lower asyncMax\n";
    idbassert(asyncCounter > 0);
    (void)atomicops::atomicDec(&asyncCounter);
  }
}

}  // namespace primitiveprocessor

// #define DCT_DEBUG 1
#define SETUP_GUARD                      \
  {                                      \
    unsigned char* o = outputp.get();    \
    memset(o, 0xa5, ouput_buf_size * 3); \
  }
#undef SETUP_GUARD
#define SETUP_GUARD
#define CHECK_GUARD(lbid)                                   \
  {                                                         \
    unsigned char* o = outputp.get();                       \
    for (int i = 0; i < ouput_buf_size; i++)                \
    {                                                       \
      if (*o++ != 0xa5)                                     \
      {                                                     \
        cerr << "Buffer underrun on LBID " << lbid << endl; \
        idbassert(0);                                       \
      }                                                     \
    }                                                       \
    o += ouput_buf_size;                                    \
    for (int i = 0; i < ouput_buf_size; i++)                \
    {                                                       \
      if (*o++ != 0xa5)                                     \
      {                                                     \
        cerr << "Buffer overrun on LBID " << lbid << endl;  \
        idbassert(0);                                       \
      }                                                     \
    }                                                       \
  }
#undef CHECK_GUARD
#define CHECK_GUARD(x)

namespace
{
using namespace primitiveprocessor;

/** @brief The job type to process a dictionary scan (pDictionaryScan class on the UM)
 * TODO: Move this & the impl into different files
 */
class DictScanJob : public threadpool::FairThreadPool::Functor
{
 public:
  DictScanJob(SP_UM_IOSOCK ios, SBS bs, SP_UM_MUTEX writeLock);
  ~DictScanJob() override;

  void write(const SBS);
  int operator()() override;
  void catchHandler(const std::string& ex, uint32_t id, uint16_t code = logging::primitiveServerErr);
  void sendErrorMsg(uint32_t id, uint16_t code);

 private:
  SP_UM_IOSOCK fIos;
  SBS fByteStream;
  SP_UM_MUTEX fWriteLock;
  posix_time::ptime dieTime;
};

DictScanJob::DictScanJob(SP_UM_IOSOCK ios, SBS bs, SP_UM_MUTEX writeLock)
 : fIos(std::move(ios)), fByteStream(std::move(bs)), fWriteLock(std::move(writeLock))
{
  dieTime = posix_time::second_clock::universal_time() + posix_time::seconds(100);
}

DictScanJob::~DictScanJob() = default;

void DictScanJob::write(const SBS sbs)
{
  // Here is the fast path for local EM to PM interaction. PM puts into the
  // input EM DEC queue directly.
  // !fWriteLock has a 'same host connection' semantics here.
  if (!fWriteLock)
  {
    fIos->write(sbs);
    return;
  }
  boost::mutex::scoped_lock lk(*fWriteLock);
  fIos->write(*sbs);
}

int DictScanJob::operator()()
{
  utils::setThreadName("PPDictScanJob");
  uint8_t data[DATA_BLOCK_SIZE];
  // Reducing this buffer one might face unrelated issues in DictScanStep.
  const uint32_t output_buf_size = MAX_BUFFER_SIZE;
  uint32_t session;
  uint32_t uniqueId = 0;
  bool wasBlockInCache = false;
  uint32_t blocksRead = 0;
  uint16_t runCount;

  boost::shared_ptr<DictEqualityFilter> eqFilter;
  TokenByScanRequestHeader* cmd;
  PrimitiveProcessor pproc(gDebugLevel);
  TokenByScanResultHeader* output;
  QueryContext verInfo;

  try
  {
#ifdef DCT_DEBUG
    DebugLevel oldDebugLevel = gDebugLevel;
    gDebugLevel = VERBOSE;
#endif
    fByteStream->advance(sizeof(TokenByScanRequestHeader));
    *fByteStream >> verInfo;
    cmd = (TokenByScanRequestHeader*)fByteStream->buf();

    session = cmd->Hdr.SessionID;
    uniqueId = cmd->Hdr.UniqueID;
    runCount = cmd->Count;
#ifdef VALGRIND
    memset(output, 0, sizeof(TokenByScanResultHeader));
#endif

    /* Grab the equality filter if one is specified */
    if (cmd->flags & HAS_EQ_FILTER)
    {
      boost::mutex::scoped_lock sl(eqFilterMutex);
      map<uint32_t, boost::shared_ptr<DictEqualityFilter> >::iterator it;
      it = dictEqualityFilters.find(uniqueId);

      if (it != dictEqualityFilters.end())
        eqFilter = it->second;

      sl.unlock();

      if (!eqFilter)
      {
        if (posix_time::second_clock::universal_time() < dieTime)
        {
          fByteStream->rewind();
          return -1;  // it's still being built, wait for it
        }
        else
          return 0;  // was probably aborted, go away...
      }
    }

    for (uint16_t i = 0; i < runCount; ++i)
    {
      SBS results(new ByteStream(output_buf_size));
      output = (TokenByScanResultHeader*)results->getInputPtr();

      loadBlock(cmd->LBID, verInfo, cmd->Hdr.TransactionID, cmd->CompType, data, &wasBlockInCache,
                &blocksRead, fLBIDTraceOn, session);
      pproc.setBlockPtr((int*)data);
      pproc.p_TokenByScan(cmd, output, output_buf_size, eqFilter);

      if (wasBlockInCache)
        output->CacheIO++;
      else
        output->PhysicalIO += blocksRead;

      results->advanceInputPtr(output->NBYTES);
      write(results);
      cmd->LBID++;
    }

#ifdef DCT_DEBUG
    gDebugLevel = oldDebugLevel;
#endif
  }
  catch (logging::IDBExcept& iex)
  {
    cerr << "DictScanJob caught an IDBException: " << iex.what() << endl;
    catchHandler(iex.what(), uniqueId, iex.errorCode());
  }
  catch (std::exception& re)
  {
    cerr << "DictScanJob caught an exception: " << re.what() << endl;
    catchHandler(re.what(), uniqueId);
  }
  catch (...)
  {
    string msg("Unknown exception caught in DictScanJob.");
    cerr << msg << endl;
    catchHandler(msg, uniqueId);
  }

  return 0;
}

void DictScanJob::catchHandler(const string& ex, uint32_t id, uint16_t code)
{
  Logger log;
  log.logMessage(ex);
  sendErrorMsg(id, code);
}

void DictScanJob::sendErrorMsg(uint32_t id, uint16_t code)
{
  ISMPacketHeader ism;
  PrimitiveHeader ph;
  ism.Status = code;
  ph.UniqueID = id;

  SBS msg(new ByteStream(sizeof(ISMPacketHeader) + sizeof(PrimitiveHeader)));
  msg->append((uint8_t*)&ism, sizeof(ism));
  msg->append((uint8_t*)&ph, sizeof(ph));

  write(msg);
}

struct BPPHandler
{
  BPPHandler(PrimitiveServer* ps) : fPrimitiveServerPtr(ps)
  {
  }

  // Keep a list of keys so that if connection fails we don't leave BPP
  // threads lying around
  std::vector<uint32_t> bppKeys;
  std::vector<uint32_t>::iterator bppKeysIt;

  ~BPPHandler()
  {
    boost::mutex::scoped_lock scoped(bppLock);

    for (bppKeysIt = bppKeys.begin(); bppKeysIt != bppKeys.end(); ++bppKeysIt)
    {
      uint32_t key = *bppKeysIt;
      BPPMap::iterator it;

      it = bppMap.find(key);

      if (it != bppMap.end())
      {
        it->second->abort();
        bppMap.erase(it);
      }

      fPrimitiveServerPtr->getProcessorThreadPool()->removeJobs(key);
      fPrimitiveServerPtr->getOOBProcessorThreadPool()->removeJobs(key);
    }

    scoped.unlock();
  }

  struct BPPHandlerFunctor : public FairThreadPool::Functor
  {
    BPPHandlerFunctor(boost::shared_ptr<BPPHandler> r, SBS b) : rt(std::move(r)), bs(std::move(b))
    {
      dieTime = posix_time::second_clock::universal_time() + posix_time::seconds(100);
    }

    boost::shared_ptr<BPPHandler> rt;
    SBS bs;
    posix_time::ptime dieTime;
  };

  struct LastJoiner : public BPPHandlerFunctor
  {
    LastJoiner(boost::shared_ptr<BPPHandler> r, SBS b) : BPPHandlerFunctor(std::move(r), std::move(b))
    {
    }
    int operator()() override
    {
      utils::setThreadName("PPHandLastJoiner");
      return rt->lastJoinerMsg(*bs, dieTime);
    }
  };

  struct Create : public BPPHandlerFunctor
  {
    Create(boost::shared_ptr<BPPHandler> r, SBS b) : BPPHandlerFunctor(std::move(r), std::move(b))
    {
    }
    int operator()() override
    {
      utils::setThreadName("PPHandCreate");
      rt->createBPP(*bs);
      return 0;
    }
  };

  struct Destroy : public BPPHandlerFunctor
  {
    Destroy(boost::shared_ptr<BPPHandler> r, SBS b) : BPPHandlerFunctor(std::move(r), std::move(b))
    {
    }
    int operator()() override
    {
      utils::setThreadName("PPHandDestroy");
      return rt->destroyBPP(*bs, dieTime);
    }
  };

  struct AddJoiner : public BPPHandlerFunctor
  {
    AddJoiner(boost::shared_ptr<BPPHandler> r, SBS b) : BPPHandlerFunctor(std::move(r), std::move(b))
    {
    }
    int operator()() override
    {
      utils::setThreadName("PPHandAddJoiner");
      return rt->addJoinerToBPP(*bs, dieTime);
    }
  };

  struct Abort : public BPPHandlerFunctor
  {
    Abort(boost::shared_ptr<BPPHandler> r, SBS b) : BPPHandlerFunctor(r, b)
    {
    }
    int operator()() override
    {
      utils::setThreadName("PPHandAbort");
      return rt->doAbort(*bs, dieTime);
    }
  };

  int doAbort(ByteStream& bs, const posix_time::ptime& dieTime)
  {
    uint32_t key;
    BPPMap::iterator it;

    try
    {
      bs.advance(sizeof(ISMPacketHeader));
      bs >> key;
    }
    catch (...)
    {
      // MCOL-857 We don't have the full packet yet
      bs.rewind();
      return -1;
    }

    boost::mutex::scoped_lock scoped(bppLock);
    bppKeysIt = std::find(bppKeys.begin(), bppKeys.end(), key);

    if (bppKeysIt != bppKeys.end())
    {
      bppKeys.erase(bppKeysIt);
    }

    it = bppMap.find(key);

    if (it != bppMap.end())
    {
      it->second->abort();
      bppMap.erase(it);
    }
    else
    {
      if (posix_time::second_clock::universal_time() > dieTime)
      {
        std::cout << "doAbort: job for key " << key << " has been killed." << std::endl;
        return 0;
      }
      else
      {
        bs.rewind();
        return -1;
      }
    }

    scoped.unlock();
    fPrimitiveServerPtr->getProcessorThreadPool()->removeJobs(key);
    fPrimitiveServerPtr->getOOBProcessorThreadPool()->removeJobs(key);
    return 0;
  }

  int doAck(ByteStream& bs)
  {
    uint32_t key;
    int16_t msgCount;
    SBPPV bpps;
    const ISMPacketHeader* ism = (const ISMPacketHeader*)bs.buf();

    key = ism->Interleave;
    msgCount = (int16_t)ism->Size;
    bs.advance(sizeof(ISMPacketHeader));

    bpps = grabBPPs(key);

    if (bpps)
    {
      bpps->getSendThread()->sendMore(msgCount);
      return 0;
    }
    else
    {
      bs.rewind();
      return -1;
    }
  }

  void createBPP(ByteStream& bs)
  {
    uint32_t i;
    uint32_t key, initMsgsLeft;
    SBPP bpp;
    SBPPV bppv;

    // make the new BPP object
    bppv.reset(new BPPV(fPrimitiveServerPtr));
    bpp.reset(new BatchPrimitiveProcessor(bs, fPrimitiveServerPtr->prefetchThreshold(), bppv->getSendThread(),
                                          fPrimitiveServerPtr->ProcessorThreads()));

    if (bs.length() > 0)
      bs >> initMsgsLeft;
    else
    {
      initMsgsLeft = -1;
    }

    idbassert(bs.length() == 0);
    bppv->getSendThread()->sendMore(initMsgsLeft);
    bppv->add(bpp);

    // this block of code creates some BPP instances up front for user queries,
    // seems to get us slightly better query times
    if ((bpp->getSessionID() & 0x80000000) == 0)
    {
      for (i = 1; i < BPPCount / 8; i++)
      {
        SBPP dup = bpp->duplicate();

        /* Uncomment these lines to verify duplicate().  == op might need updating */
        //  					if (*bpp != *dup)
        // 	 					cerr << "createBPP: duplicate mismatch at index " << i
        // << endl;
        //  					idbassert(*bpp == *dup);
        bppv->add(dup);
      }
    }

    boost::mutex::scoped_lock scoped(bppLock);
    key = bpp->getUniqueID();
    bppKeys.push_back(key);
    bool newInsert;
    newInsert = bppMap.insert(pair<uint32_t, SBPPV>(key, bppv)).second;
    scoped.unlock();

    if (!newInsert)
    {
      if (bpp->getSessionID() & 0x80000000)
        cerr << "warning: createBPP() tried to clobber a BPP with duplicate sessionID & stepID. sessionID="
             << (int)(bpp->getSessionID() ^ 0x80000000) << " stepID=" << bpp->getStepID() << " (syscat)"
             << endl;
      else
        cerr << "warning: createBPP() tried to clobber a BPP with duplicate sessionID & stepID.  sessionID="
             << bpp->getSessionID() << " stepID=" << bpp->getStepID() << endl;
    }
  }

  inline SBPPV grabBPPs(uint32_t uniqueID)
  {
    BPPMap::iterator it;

    SBPPV ret;

    boost::mutex::scoped_lock scoped(bppLock);
    it = bppMap.find(uniqueID);

    if (it != bppMap.end())
      return it->second;
    else
      return SBPPV();
  }

  inline boost::shared_mutex& getDJLock(uint32_t uniqueID)
  {
    boost::mutex::scoped_lock lk(djMutex);
    auto it = djLock.find(uniqueID);
    if (it != djLock.end())
      return *it->second;
    else
    {
      auto ret = djLock.insert(make_pair(uniqueID, new boost::shared_mutex())).first;
      return *ret->second;
    }
  }

  inline void deleteDJLock(uint32_t uniqueID)
  {
    boost::mutex::scoped_lock lk(djMutex);
    auto it = djLock.find(uniqueID);
    if (it != djLock.end())
    {
      delete it->second;
      djLock.erase(it);
    }
  }

  int addJoinerToBPP(ByteStream& bs, const posix_time::ptime& dieTime)
  {
    SBPPV bppv;
    uint32_t uniqueID;
    const uint8_t* buf;

    /* call addToJoiner() on the first BPP */

    buf = bs.buf();
    /* the uniqueID is after the ISMPacketHeader, sessionID, and stepID */
    uniqueID = *((const uint32_t*)&buf[sizeof(ISMPacketHeader) + 2 * sizeof(uint32_t)]);

    bppv = grabBPPs(uniqueID);

    if (bppv)
    {
      boost::shared_lock<boost::shared_mutex> lk(getDJLock(uniqueID));
      bppv->get()[0]->addToJoiner(bs);
      return 0;
    }
    else
    {
      if (posix_time::second_clock::universal_time() > dieTime)
      {
        cout << "addJoinerToBPP: job for id " << uniqueID << " has been killed." << endl;
        return 0;
      }
      else
        return -1;
    }
  }

  int lastJoinerMsg(ByteStream& bs, const posix_time::ptime& dieTime)
  {
    SBPPV bppv;
    uint32_t uniqueID, i;
    const uint8_t* buf;
    int err;

    /* call endOfJoiner() on the every BPP */

    buf = bs.buf();
    /* the uniqueID is after the ISMPacketHeader, sessionID, and stepID */
    uniqueID = *((const uint32_t*)&buf[sizeof(ISMPacketHeader) + 2 * sizeof(uint32_t)]);
    bppv = grabBPPs(uniqueID);

    if (!bppv)
    {
      if (posix_time::second_clock::universal_time() > dieTime)
      {
        cout << "LastJoiner: job for id " << uniqueID << " has been killed." << endl;
        return 0;
      }
      else
      {
        return -1;
      }
    }

    boost::unique_lock<boost::shared_mutex> lk(getDJLock(uniqueID));
    for (i = 0; i < bppv->get().size(); i++)
    {
      err = bppv->get()[i]->endOfJoiner();

      if (err == -1)
      {
        if (posix_time::second_clock::universal_time() > dieTime)
        {
          cout << "LastJoiner: job for id " << uniqueID
               << " has been killed waiting for joiner messages for too long." << endl;
          return 0;
        }
        else
          return -1;
      }
    }

    /* Note: some of the duplicate/run/join sync was moved to the BPPV class to do
    more intelligent scheduling.  Once the join data is received, BPPV will
    start letting jobs run and create more BPP instances on demand. */

    bppv->joinDataReceived = true;
    return 0;
  }

  int destroyBPP(ByteStream& bs, const posix_time::ptime& dieTime)
  {
    uint32_t uniqueID, sessionID, stepID;
    BPPMap::iterator it;
    if (bs.length() < sizeof(ISMPacketHeader) + sizeof(sessionID) + sizeof(stepID) + sizeof(uniqueID))
    {
      // MCOL-857 We don't appear to have the full packet yet!
      return -1;
    }

    // throw here will be actual error, not a phony one.
    bs.advance(sizeof(ISMPacketHeader));
    bs >> sessionID;
    bs >> stepID;
    bs >> uniqueID;

    boost::shared_ptr<BPPV> bppv = nullptr;
    {
      boost::unique_lock<boost::shared_mutex> lk(getDJLock(uniqueID));
      boost::mutex::scoped_lock scoped(bppLock);

      bppKeysIt = std::find(bppKeys.begin(), bppKeys.end(), uniqueID);

      if (bppKeysIt != bppKeys.end())
      {
        bppKeys.erase(bppKeysIt);
      }

      it = bppMap.find(uniqueID);

      if (it != bppMap.end())
      {
        bppv = it->second;

        if (bppv->joinDataReceived)
        {
          bppMap.erase(it);
        }
        else
        {
          // MCOL-5. On ubuntu, a crash was happening. Checking
          // joinDataReceived here fixes it.
          // We're not ready for a destroy. Reschedule to wait
          // for all joiners to arrive.
          // TODO there might be no joiners if the query is canceled.
          // The memory will leak.
          // Rewind to the beginning of ByteStream buf b/c of the advance above.
          bs.rewind();
          return -1;
        }
      }
      else
      {
        if (posix_time::second_clock::universal_time() > dieTime)
        {
          cout << "destroyBPP: job for id " << uniqueID << " and sessionID " << sessionID
               << " has been killed." << endl;
          // If for some reason there are jobs for this uniqueID that arrived later
          // they won't leave PP thread pool staying there forever.
        }
        else
        {
          bs.rewind();
          return -1;
        }
      }

      fPrimitiveServerPtr->getProcessorThreadPool()->removeJobs(uniqueID);
      fPrimitiveServerPtr->getOOBProcessorThreadPool()->removeJobs(uniqueID);
      lk.unlock();
      deleteDJLock(uniqueID);
    }

    if (bppv)
    {
      bppv->abort();
    }
    return 0;
  }

  void setBPPToError(uint32_t uniqueID, const string& error, logging::ErrorCodeValues errorCode)
  {
    SBPPV bppv;

    bppv = grabBPPs(uniqueID);

    if (!bppv)
      return;

    for (uint32_t i = 0; i < bppv->get().size(); i++)
      bppv->get()[i]->setError(error, errorCode);

    if (bppv->get().empty() && !bppMap.empty())
      bppMap.begin()->second.get()->get()[0]->setError(error, errorCode);
  }

  // Would be good to define the structure of these msgs somewhere...
  inline uint32_t getUniqueID(SBS bs, uint8_t command)
  {
    uint8_t* buf;

    buf = bs->buf();

    switch (command)
    {
      case BATCH_PRIMITIVE_ABORT: return *((uint32_t*)&buf[sizeof(ISMPacketHeader)]);

      case BATCH_PRIMITIVE_ACK:
      {
        ISMPacketHeader* ism = (ISMPacketHeader*)buf;
        return ism->Interleave;
      }

      case BATCH_PRIMITIVE_ADD_JOINER:
      case BATCH_PRIMITIVE_END_JOINER:
      case BATCH_PRIMITIVE_DESTROY: return *((uint32_t*)&buf[sizeof(ISMPacketHeader) + 2 * sizeof(uint32_t)]);

      default: return 0;
    }
  }

  PrimitiveServer* fPrimitiveServerPtr;
};

class DictionaryOp : public FairThreadPool::Functor
{
 public:
  DictionaryOp(SBS cmd) : bs(std::move(cmd))
  {
    dieTime = posix_time::second_clock::universal_time() + posix_time::seconds(100);
  }
  virtual int execute() = 0;
  int operator()() override
  {
    utils::setThreadName("PPDictOp");
    int ret;
    ret = execute();

    if (ret != 0)
    {
      bs->rewind();

      if (posix_time::second_clock::universal_time() > dieTime)
      {
        cout << "DictionaryOp::operator(): job has been killed." << endl;
        return 0;
      }
    }

    return ret;
  }

 protected:
  SBS bs;

 private:
  posix_time::ptime dieTime;
};

class CreateEqualityFilter : public DictionaryOp
{
 public:
  CreateEqualityFilter(SBS cmd) : DictionaryOp(cmd)
  {
  }
  int execute() override
  {
    createEqualityFilter();
    return 0;
  }

 private:
  void createEqualityFilter()
  {
    uint32_t uniqueID, count, i, charsetNumber;
    string str;
    bs->advance(sizeof(ISMPacketHeader));
    *bs >> uniqueID;
    *bs >> charsetNumber;

    datatypes::Charset cs(charsetNumber);
    boost::shared_ptr<DictEqualityFilter> filter(new DictEqualityFilter(cs));

    *bs >> count;

    for (i = 0; i < count; i++)
    {
      *bs >> str;
      filter->insert(str);
    }

    boost::mutex::scoped_lock sl(eqFilterMutex);
    dictEqualityFilters[uniqueID] = filter;
  }
};

class DestroyEqualityFilter : public DictionaryOp
{
 public:
  DestroyEqualityFilter(SBS cmd) : DictionaryOp(std::move(cmd))
  {
  }
  int execute() override
  {
    return destroyEqualityFilter();
  }

  int destroyEqualityFilter()
  {
    uint32_t uniqueID;
    map<uint32_t, boost::shared_ptr<DictEqualityFilter> >::iterator it;

    bs->advance(sizeof(ISMPacketHeader));
    *bs >> uniqueID;

    boost::mutex::scoped_lock sl(eqFilterMutex);
    it = dictEqualityFilters.find(uniqueID);

    if (it != dictEqualityFilters.end())
    {
      dictEqualityFilters.erase(it);
      return 0;
    }
    else
    {
      bs->rewind();
      return -1;
    }
  }
};

struct ReadThread
{
  ReadThread(const string& serverName, IOSocket& ios, PrimitiveServer* ps)
   : fServerName(serverName), fIos(ios), fPrimitiveServerPtr(ps)
  {
    fBPPHandler.reset(new BPPHandler(ps));
  }

  const ByteStream buildCacheOpResp(int32_t result)
  {
    const int msgsize = sizeof(ISMPacketHeader) + sizeof(int32_t);
    ByteStream::byte msgbuf[msgsize];
    memset(msgbuf, 0, sizeof(ISMPacketHeader));
    ISMPacketHeader* hdrp = reinterpret_cast<ISMPacketHeader*>(&msgbuf[0]);
    hdrp->Command = CACHE_OP_RESULTS;
    int32_t* resp = reinterpret_cast<int32_t*>(&msgbuf[sizeof(ISMPacketHeader)]);
    *resp = result;
    return ByteStream(msgbuf, msgsize);
  }

  /* Message format:
   * 	ISMPacketHeader
   * 	OID count - 32 bits
   *  OID array - 32 bits * count
   */
  void doCacheFlushByOID(SP_UM_IOSOCK ios, ByteStream& bs)
  {
    uint8_t* buf = bs.buf();
    buf += sizeof(ISMPacketHeader);
    uint32_t count = *((uint32_t*)buf);
    buf += 4;
    uint32_t* oids = (uint32_t*)buf;

    for (int i = 0; i < fCacheCount; i++)
    {
      blockCacheClient bc(*BRPp[i]);
      bc.flushOIDs(oids, count);
    }

    ios->write(buildCacheOpResp(0));
  }

  /* Message format:
   * 	ISMPacketHeader
   * 	Partition count - 32 bits
   * 	Partition set - sizeof(LogicalPartition)  boost::shared_ptr* count
   * 	OID count - 32 bits
   * 	OID array - 32 bits * count
   */
  void doCacheFlushByPartition(SP_UM_IOSOCK ios, ByteStream& bs)
  {
    set<BRM::LogicalPartition> partitions;
    vector<OID_t> oids;

    bs.advance(sizeof(ISMPacketHeader));
    deserializeSet<LogicalPartition>(bs, partitions);
    deserializeInlineVector<OID_t>(bs, oids);

    idbassert(bs.length() == 0);

    for (int i = 0; i < fCacheCount; i++)
    {
      blockCacheClient bc(*BRPp[i]);
      bc.flushPartition(oids, partitions);
    }

    ios->write(buildCacheOpResp(0));
  }

  void doCacheFlushCmd(SP_UM_IOSOCK ios, const ByteStream& /*bs*/)
  {
    for (int i = 0; i < fCacheCount; i++)
    {
      blockCacheClient bc(*BRPp[i]);
      bc.flushCache();
    }

    ios->write(buildCacheOpResp(0));
  }

  void doCacheDropFDs(SP_UM_IOSOCK ios, ByteStream& bs)
  {
    std::vector<BRM::FileInfo> files;

    bs.advance(sizeof(ISMPacketHeader));
    dropFDCache();
    ios->write(buildCacheOpResp(0));
  }

  void doCachePurgeFDs(SP_UM_IOSOCK ios, ByteStream& bs)
  {
    std::vector<BRM::FileInfo> files;

    bs.advance(sizeof(ISMPacketHeader));
    deserializeInlineVector<BRM::FileInfo>(bs, files);
    purgeFDCache(files);
    ios->write(buildCacheOpResp(0));
  }

  // N.B. this fcn doesn't actually clean the VSS, but rather instructs PP to flush its
  //   cache of specific LBID's
  void doCacheCleanVSSCmd(SP_UM_IOSOCK ios, const ByteStream& bs)
  {
    const ByteStream::byte* bytePtr = bs.buf();
    const uint32_t* cntp = reinterpret_cast<const uint32_t*>(&bytePtr[sizeof(ISMPacketHeader)]);
    const LbidAtVer* itemp =
        reinterpret_cast<const LbidAtVer*>(&bytePtr[sizeof(ISMPacketHeader) + sizeof(uint32_t)]);

    for (int i = 0; i < fCacheCount; i++)
    {
      blockCacheClient bc(*BRPp[i]);
      bc.flushMany(itemp, *cntp);
    }

    ios->write(buildCacheOpResp(0));
  }

  void doCacheFlushAllversion(SP_UM_IOSOCK ios, const ByteStream& bs)
  {
    const ByteStream::byte* bytePtr = bs.buf();
    const uint32_t* cntp = reinterpret_cast<const uint32_t*>(&bytePtr[sizeof(ISMPacketHeader)]);
    const LBID_t* itemp =
        reinterpret_cast<const LBID_t*>(&bytePtr[sizeof(ISMPacketHeader) + sizeof(uint32_t)]);

    for (int i = 0; i < fCacheCount; i++)
    {
      blockCacheClient bc(*BRPp[i]);
      bc.flushManyAllversion(itemp, *cntp);
    }

    ios->write(buildCacheOpResp(0));
  }

  static void dispatchPrimitive(SBS sbs, boost::shared_ptr<BPPHandler>& fBPPHandler,
                                boost::shared_ptr<threadpool::FairThreadPool> procPool,
                                std::shared_ptr<threadpool::PriorityThreadPool> OOBProcPool,
                                SP_UM_IOSOCK& outIos, SP_UM_MUTEX& writeLock, const uint32_t processorThreads,
                                const bool ptTrace)
  {
    const ISMPacketHeader* ismHdr = reinterpret_cast<const ISMPacketHeader*>(sbs->buf());
    switch (ismHdr->Command)
    {
      case DICT_CREATE_EQUALITY_FILTER:
      case DICT_DESTROY_EQUALITY_FILTER:
      case BATCH_PRIMITIVE_CREATE:
      case BATCH_PRIMITIVE_ADD_JOINER:
      case BATCH_PRIMITIVE_END_JOINER:
      case BATCH_PRIMITIVE_DESTROY:
      case BATCH_PRIMITIVE_ABORT:
      {
        const uint8_t* buf = sbs->buf();
        uint32_t pos = sizeof(ISMPacketHeader) - 2;
        const uint32_t txnId = *((uint32_t*)&buf[pos + 2]);
        const uint32_t stepID = *((uint32_t*)&buf[pos + 6]);
        const uint32_t uniqueID = *((uint32_t*)&buf[pos + 10]);
        const uint32_t weight = threadpool::MetaJobsInitialWeight;
        const uint32_t priority = 0;

        uint32_t id = 0;
        boost::shared_ptr<FairThreadPool::Functor> functor;
        if (ismHdr->Command == DICT_CREATE_EQUALITY_FILTER)
        {
          functor.reset(new CreateEqualityFilter(sbs));
        }
        else if (ismHdr->Command == DICT_DESTROY_EQUALITY_FILTER)
        {
          functor.reset(new DestroyEqualityFilter(sbs));
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_CREATE)
        {
          functor.reset(new BPPHandler::Create(fBPPHandler, sbs));
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_ADD_JOINER)
        {
          functor.reset(new BPPHandler::AddJoiner(fBPPHandler, sbs));
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_END_JOINER)
        {
          id = fBPPHandler->getUniqueID(sbs, ismHdr->Command);
          functor.reset(new BPPHandler::LastJoiner(fBPPHandler, sbs));
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_DESTROY)
        {
          id = fBPPHandler->getUniqueID(sbs, ismHdr->Command);
          functor.reset(new BPPHandler::Destroy(fBPPHandler, sbs));
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_ABORT)
        {
          id = fBPPHandler->getUniqueID(sbs, ismHdr->Command);
          functor.reset(new BPPHandler::Abort(fBPPHandler, sbs));
        }
        PriorityThreadPool::Job job(uniqueID, stepID, txnId, functor, outIos, weight, priority, id);
        OOBProcPool->addJob(job);
        break;
      }

      case DICT_TOKEN_BY_SCAN_COMPARE:
      case BATCH_PRIMITIVE_RUN:
      {
        TokenByScanRequestHeader* hdr = nullptr;
        boost::shared_ptr<FairThreadPool::Functor> functor;
        uint32_t id = 0;
        uint32_t weight = 0;
        uint32_t priority = 0;
        uint32_t txnId = 0;
        uint32_t stepID = 0;
        uint32_t uniqueID = 0;

        if (ismHdr->Command == DICT_TOKEN_BY_SCAN_COMPARE)
        {
          idbassert(sbs->length() >= sizeof(TokenByScanRequestHeader));
          hdr = (TokenByScanRequestHeader*)ismHdr;
          functor.reset(new DictScanJob(outIos, sbs, writeLock));
          id = hdr->Hdr.UniqueID;
          weight = LOGICAL_BLOCK_RIDS;
          priority = hdr->Hdr.Priority;
          const uint8_t* buf = sbs->buf();
          const uint32_t pos = sizeof(ISMPacketHeader) - 2;
          txnId = *((uint32_t*)&buf[pos + 2]);
          stepID = *((uint32_t*)&buf[pos + 6]);
          uniqueID = *((uint32_t*)&buf[pos + 10]);
        }
        else if (ismHdr->Command == BATCH_PRIMITIVE_RUN)
        {
          functor.reset(new BPPSeeder(sbs, writeLock, outIos, processorThreads, ptTrace));
          BPPSeeder* bpps = dynamic_cast<BPPSeeder*>(functor.get());
          id = bpps->getID();
          priority = bpps->priority();
          const uint8_t* buf = sbs->buf();
          const uint32_t pos = sizeof(ISMPacketHeader) - 2;
          txnId = *((uint32_t*)&buf[pos + 2]);
          stepID = *((uint32_t*)&buf[pos + 6]);
          uniqueID = *((uint32_t*)&buf[pos + 10]);
          weight = ismHdr->Size + *((uint32_t*)&buf[pos + 18]) + 100000;
        }
        if (hdr && hdr->flags & IS_SYSCAT)
        {
          PriorityThreadPool::Job job(uniqueID, stepID, txnId, functor, outIos, weight, priority, id);
          OOBProcPool->addJob(job);
        }
        else
        {
          FairThreadPool::Job job(uniqueID, stepID, txnId, functor, outIos, weight, priority, id);
          procPool->addJob(job);
        }

        break;
      }

      case BATCH_PRIMITIVE_ACK:
      {
        fBPPHandler->doAck(*sbs);
        break;
      }
      default:
      {
        std::ostringstream os;
        Logger log;
        os << "unknown primitive cmd: " << ismHdr->Command;
        log.logMessage(os.str());
        break;
      }
    }  // the switch stmt
  }

  void operator()()
  {
    utils::setThreadName("PPReadThread");
    auto procPool = fPrimitiveServerPtr->getProcessorThreadPool();
    auto OOBProcPool = fPrimitiveServerPtr->getOOBProcessorThreadPool();
    SBS bs;
    UmSocketSelector* pUmSocketSelector = UmSocketSelector::instance();

    // Establish default output IOSocket (and mutex) based on the input
    // IOSocket. If we end up rotating through multiple output sockets
    // for the same UM, we will use UmSocketSelector to select output.
    SP_UM_IOSOCK outIosDefault(new IOSocket(fIos));
    SP_UM_MUTEX writeLockDefault(new boost::mutex());

    bool bRotateDest = fPrimitiveServerPtr->rotatingDestination();

    if (bRotateDest)
    {
      // If we tried adding an IP address not listed as UM in config
      // file; probably a DMLProc connection.  We allow the connection
      // but disable destination rotation since not in Columnstore.xml.
      if (!pUmSocketSelector->addConnection(outIosDefault, writeLockDefault))
      {
        bRotateDest = false;
      }
    }

    SP_UM_IOSOCK outIos(outIosDefault);
    SP_UM_MUTEX writeLock(writeLockDefault);

    //..Loop to process incoming messages on IOSocket fIos
    for (;;)
    {
      try
      {
        bs = fIos.read();
      }
      catch (...)
      {
        // This connection is dead, nothing useful will come from it ever again
        // We can't rely on the state of bs at this point...
        if (bRotateDest && pUmSocketSelector)
          pUmSocketSelector->delConnection(fIos);

        fIos.close();
        break;
      }

      try
      {
        if (bs->length() != 0)
        {
          idbassert(bs->length() >= sizeof(ISMPacketHeader));

          const ISMPacketHeader* ismHdr = reinterpret_cast<const ISMPacketHeader*>(bs->buf());
          /* This switch is for the OOB commands */
          switch (ismHdr->Command)
          {
            case CACHE_FLUSH_PARTITION:
              doCacheFlushByPartition(outIos, *bs);
              fIos.close();
              return;

            case CACHE_FLUSH_BY_OID:
              doCacheFlushByOID(outIos, *bs);
              fIos.close();
              return;

            case CACHE_FLUSH:
              doCacheFlushCmd(outIos, *bs);
              fIos.close();
              return;

            case CACHE_CLEAN_VSS:
              doCacheCleanVSSCmd(outIos, *bs);
              fIos.close();
              return;

            case FLUSH_ALL_VERSION:
              doCacheFlushAllversion(outIos, *bs);
              fIos.close();
              return;

            case CACHE_DROP_FDS:
              doCacheDropFDs(outIos, *bs);
              fIos.close();
              return;

            case CACHE_PURGE_FDS:
              doCachePurgeFDs(outIos, *bs);
              fIos.close();
              return;

            default: break;
          }
          dispatchPrimitive(bs, fBPPHandler, procPool, OOBProcPool, outIos, writeLock,
                            fPrimitiveServerPtr->ProcessorThreads(), fPrimitiveServerPtr->PTTrace());
        }
        else  // bs.length() == 0
        {
          if (bRotateDest)
            pUmSocketSelector->delConnection(fIos);

          fIos.close();
          break;
        }
      }  // the try- surrounding the if stmt
      catch (std::exception& e)
      {
        Logger logger;
        logger.logMessage(e.what());
      }
    }
  }

  // If this function is called, we have a "bug" of some sort.  We added
  // the "fIos" connection to UmSocketSelector earlier, so at the very
  // least, UmSocketSelector should have been able to return that con-
  // nection/port.  We will try to recover by using the original fIos to
  // send the response msg; but as stated, if this ever happens we have
  // a bug we need to resolve.
  void handleUmSockSelErr(const string& cmd)
  {
    ostringstream oss;
    oss << "Unable to rotate through socket destinations (" << cmd << ") for connection: " << fIos.toString();
    cerr << oss.str() << endl;
    logging::Message::Args args;
    args.add(oss.str());
    mlp->logMessage(logging::M0058, args, false);
  }

  ~ReadThread() = default;
  string fServerName;
  IOSocket fIos;
  PrimitiveServer* fPrimitiveServerPtr;
  boost::shared_ptr<BPPHandler> fBPPHandler;
};

struct ServerThread
{
  ServerThread(string serverName, PrimitiveServer* ps) : fServerName(serverName), fPrimitiveServerPtr(ps)
  {
    SUMMARY_INFO2("starting server ", fServerName);

    bool tellUser = true;
    bool toldUser = false;

    for (;;)
    {
      try
      {
        mqServerPtr = new MessageQueueServer(fServerName);
        break;
      }
      catch (runtime_error& re)
      {
        string what = re.what();

        if (what.find("Address already in use") != string::npos)
        {
          if (tellUser)
          {
            cerr << "Address already in use, retrying..." << endl;
            tellUser = false;
            toldUser = true;
          }

          sleep(5);
        }
        else
        {
          throw;
        }
      }
    }

    if (toldUser)
      cerr << "Ready." << endl;
  }

  void operator()()
  {
    utils::setThreadName("PPServerThr");
    IOSocket ios;

    try
    {
      for (;;)
      {
        ios = mqServerPtr->accept();
        // startup a detached thread to handle this socket's I/O
        boost::thread rt(ReadThread(fServerName, ios, fPrimitiveServerPtr));
      }
    }
    catch (std::exception& ex)
    {
      SUMMARY_INFO2("exception caught in ServerThread: ", ex.what());
    }
    catch (...)
    {
      SUMMARY_INFO("exception caught in ServerThread.");
    }
  }

  string fServerName;
  PrimitiveServer* fPrimitiveServerPtr;
  MessageQueueServer* mqServerPtr;
};

}  // namespace

namespace primitiveprocessor
{
PrimitiveServer::PrimitiveServer(int serverThreads, int serverQueueSize, int processorWeight,
                                 int /*processorQueueSize*/, bool rotatingDestination, uint32_t BRPBlocks,
                                 int BRPThreads, int cacheCount, int maxBlocksPerRead, int readAheadBlocks,
                                 uint32_t deleteBlocks, bool ptTrace, double prefetch, uint64_t /*smallSide*/)
 : fServerThreads(serverThreads)
 , fServerQueueSize(serverQueueSize)
 , fProcessorWeight(processorWeight)
 , fMaxBlocksPerRead(maxBlocksPerRead)
 , fReadAheadBlocks(readAheadBlocks)
 , fRotatingDestination(rotatingDestination)
 , fPTTrace(ptTrace)
 , fPrefetchThreshold(prefetch)
{
  fCacheCount = cacheCount;
  fServerpool.setMaxThreads(fServerThreads);
  fServerpool.setQueueSize(fServerQueueSize);
  fServerpool.setName("PrimitiveServer");

  fProcessorPool.reset(new threadpool::FairThreadPool(fProcessorWeight, highPriorityThreads,
                                                      medPriorityThreads, lowPriorityThreads, 0));
  // We're not using either the priority or the job-clustering features, just need a threadpool
  // that can reschedule jobs, and an unlimited non-blocking queue
  fOOBPool.reset(new threadpool::PriorityThreadPool(1, 5, 0, 0, 1));

  asyncCounter = 0;

  brm = new DBRM();

  BRPp = new BlockRequestProcessor*[fCacheCount];

  try
  {
    for (int i = 0; i < fCacheCount; i++)
      BRPp[i] = new BlockRequestProcessor(BRPBlocks / fCacheCount, BRPThreads / fCacheCount,
                                          fMaxBlocksPerRead, deleteBlocks / fCacheCount);
  }
  catch (...)
  {
    cerr << "Unable to allocate " << BRPBlocks
         << " cache blocks. Adjust the DBBC config parameter "
            "downward."
         << endl;
    mlp->logMessage(logging::M0045, logging::Message::Args(), true);
    exit(1);
  }
}

PrimitiveServer::~PrimitiveServer()
{
}

void PrimitiveServer::start(Service* service, utils::USpaceSpinLock& startupRaceLock)
{
  // start all the server threads
  for (int i = 1; i <= fServerThreads; i++)
  {
    string s("PMS");
    stringstream oss;
    oss << s << i;

    fServerpool.invoke(ServerThread(oss.str(), this));
  }

  startupRaceLock.release();
  service->NotifyServiceStarted();

  std::thread sameHostServerThread(
      [this]()
      {
        utils::setThreadName("PPSHServerThr");
        auto* exeMgrDecPtr = (exemgr::globServiceExeMgr) ? exemgr::globServiceExeMgr->getDec() : nullptr;
        while (!exeMgrDecPtr)
        {
          sleep(1);
          exeMgrDecPtr = (exemgr::globServiceExeMgr) ? exemgr::globServiceExeMgr->getDec() : nullptr;
        }
        // This is a pseudo socket that puts data into DEC queue directly.
        // It can be used for PP to EM communication only.
        SP_UM_IOSOCK outIos(new IOSocket(new SameNodePseudoSocket(exeMgrDecPtr)));
        // This empty SP transmits "same-host" messaging semantics.
        SP_UM_MUTEX writeLock(nullptr);
        auto procPool = this->getProcessorThreadPool();
        auto OOBProcPool = this->getOOBProcessorThreadPool();
        boost::shared_ptr<BPPHandler> fBPPHandler(new BPPHandler(this));
        for (;;)
        {
          joblist::DistributedEngineComm::SBSVector primitiveMsgs;
          for (auto sbs : exeMgrDecPtr->readLocalQueueMessagesOrWait(primitiveMsgs))
          {
            if (sbs->length() == 0)
            {
              std::cout << "PPSHServerThr got an empty ByteStream." << std::endl;
              continue;
            }
            idbassert(sbs->length() >= sizeof(ISMPacketHeader));

            ReadThread::dispatchPrimitive(sbs, fBPPHandler, procPool, OOBProcPool, outIos, writeLock,
                                          this->ProcessorThreads(), this->PTTrace());
          }
        }
      });

  fServerpool.wait();

  cerr << "PrimitiveServer::start() exiting!" << endl;
}

BPPV::BPPV(PrimitiveServer* ps)
{
  sendThread.reset(new BPPSendThread());
  sendThread->setProcessorPool(ps->getProcessorThreadPool());
  v.reserve(BPPCount);
  pos = 0;
}

BPPV::~BPPV()
{
}

void BPPV::add(boost::shared_ptr<BatchPrimitiveProcessor> a)
{
  /* Right now BPP initialization locks the object if there is a join to
  prevent the instance from being used before the join data is received.
  The init/join/run sync code is quite old and confusing at this point,
  and this makes it a little worse by circumventing the original design.
  Due for a rewrite. */

  if (!unusedInstance)
  {
    unusedInstance = a->duplicate();

    if (a->hasJoin())
    {
      joinDataReceived = false;
      unusedInstance->unlock();
    }
    else
      joinDataReceived = true;
  }

  v.push_back(a);
}
const vector<boost::shared_ptr<BatchPrimitiveProcessor> >& BPPV::get()
{
  return v;
}

boost::shared_ptr<BatchPrimitiveProcessor> BPPV::next()
{
  uint32_t size = v.size();
  uint32_t i = 0;

  // This block of code creates BPP instances if/when they are needed

  // don't use a processor thread when it will just block, reschedule it instead
  if (!joinDataReceived)
    return boost::shared_ptr<BatchPrimitiveProcessor>();

  for (i = 0; i < size; i++)
  {
    uint32_t index = (i + pos) % size;

    if (!(v[index]->busy()))
    {
      pos = (index + 1) % size;
      v[index]->busy(true);
      return v[index];
    }
  }

  // honor the BPPCount limit, mostly for debugging purposes.
  if (size >= BPPCount)
    return boost::shared_ptr<BatchPrimitiveProcessor>();

  SBPP newone = unusedInstance->duplicate();

  if (newone->hasJoin())
    newone->unlock();

  newone->busy(true);
  v.push_back(newone);
  return newone;
}

void BPPV::abort()
{
  sendThread->abort();
  BOOST_FOREACH (boost::shared_ptr<BatchPrimitiveProcessor> bpp, v)
  {
    bpp->unlock();
  }
}

bool BPPV::aborted()
{
  return sendThread->aborted();
}

// end workaround

}  // namespace primitiveprocessor

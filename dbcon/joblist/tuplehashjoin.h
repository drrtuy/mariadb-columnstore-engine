/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2019 MariaDB Corporation.

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

//  $Id: tuplehashjoin.h 9655 2013-06-25 23:08:13Z xlou $

#pragma once

#include "jobstep.h"
#include "calpontsystemcatalog.h"
#include "hasher.h"
#include "tuplejoiner.h"
#include "threadnaming.h"
#include <boost/shared_ptr.hpp>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include "resourcemanager.h"
#include "exceptclasses.h"

namespace joblist
{
class BatchPrimitive;
class TupleBPS;
struct FunctionJoinInfo;
class DiskJoinStep;

class TupleHashJoinStep : public JobStep, public TupleDeliveryStep
{
 public:
  /**
   * @param
   */
  explicit TupleHashJoinStep(const JobInfo& jobInfo);
  ~TupleHashJoinStep() override;

  void setLargeSideBPS(BatchPrimitive*);
  void setLargeSideStepsOut(const std::vector<SJSTEP>& largeSideSteps);
  void setSmallSideStepsOut(const std::vector<SJSTEP>& smallSideSteps);

  /* mandatory JobStep interface */
  void run() override;
  void join() override;
  const std::string toString() const override;

  /* These tableOID accessors can go away soon */
  execplan::CalpontSystemCatalog::OID tableOid() const override
  {
    return fTableOID2;
  }
  execplan::CalpontSystemCatalog::OID tableOid1() const
  {
    return fTableOID1;
  }
  execplan::CalpontSystemCatalog::OID tableOid2() const
  {
    return fTableOID2;
  }
  void tableOid1(execplan::CalpontSystemCatalog::OID tableOid1)
  {
    fTableOID1 = tableOid1;
    if (fTableOID1 >= 1000 && fTableOID1 < 3000)
    {
      numCores = 1;  // syscat query, no need for more than 1 thread
    }
  }
  void tableOid2(execplan::CalpontSystemCatalog::OID tableOid2)
  {
    fTableOID2 = tableOid2;
  }

  std::string alias1() const
  {
    return fAlias1;
  }
  void alias1(const std::string& alias)
  {
    fAlias1 = alias;
  }
  std::string alias2() const
  {
    return fAlias2;
  }
  void alias2(const std::string& alias)
  {
    fAlias = fAlias2 = alias;
  }

  std::string view1() const
  {
    return fView1;
  }
  void view1(const std::string& vw)
  {
    fView1 = vw;
  }
  std::string view2() const
  {
    return fView2;
  }
  void view2(const std::string& vw)
  {
    fView = fView2 = vw;
  }

  std::string schema1() const
  {
    return fSchema1;
  }
  void schema1(const std::string& s)
  {
    fSchema1 = s;
  }
  std::string schema2() const
  {
    return fSchema2;
  }
  void schema2(const std::string& s)
  {
    fSchema = fSchema2 = s;
  }

  int32_t sequence1() const
  {
    return fSequence1;
  }
  void sequence1(int32_t seq)
  {
    fSequence1 = seq;
  }
  int32_t sequence2() const
  {
    return fSequence2;
  }
  void sequence2(int32_t seq)
  {
    fSequence2 = seq;
  }

  const execplan::ReturnedColumn* column1() const
  {
    return fColumn1;
  }
  void column1(const execplan::ReturnedColumn* pos)
  {
    fColumn1 = pos;
  }
  const execplan::ReturnedColumn* column2() const
  {
    return fColumn2;
  }
  void column2(const execplan::ReturnedColumn* pos)
  {
    fColumn2 = pos;
  }

  int correlatedSide() const
  {
    return fCorrelatedSide;
  }
  void correlatedSide(int c)
  {
    fCorrelatedSide = c;
  }
  using JobStep::tupleId;
  uint64_t tupleId() const override
  {
    return fTupleId2;
  }
  uint64_t tupleId1() const
  {
    return fTupleId1;
  }
  uint64_t tupleId2() const
  {
    return fTupleId2;
  }
  void tupleId1(uint64_t id)
  {
    fTupleId1 = id;
  }
  void tupleId2(uint64_t id)
  {
    fTupleId2 = id;
  }

  void addSmallSideRG(const std::vector<rowgroup::RowGroup>& rgs, const std::vector<std::string>& tableNames);
  void addJoinKeyIndex(const std::vector<JoinType>& jt, const std::vector<bool>& typeless,
                       const std::vector<std::vector<uint32_t>>& smallkeys,
                       const std::vector<std::vector<uint32_t>>& largekeys);

  void configSmallSideRG(const std::vector<rowgroup::RowGroup>& rgs,
                         const std::vector<std::string>& tableNames);
  void configLargeSideRG(const rowgroup::RowGroup& rg);

  void configJoinKeyIndex(const std::vector<JoinType>& jt, const std::vector<bool>& typeless,
                          const std::vector<std::vector<uint32_t>>& smallkeys,
                          const std::vector<std::vector<uint32_t>>& largekeys);

  void setOutputRowGroup(const rowgroup::RowGroup& rg) override;

  uint32_t nextBand(messageqcpp::ByteStream& bs) override;

  const rowgroup::RowGroup& getOutputRowGroup() const override
  {
    return outputRG;
  }
  const rowgroup::RowGroup& getSmallRowGroup() const
  {
    return smallRGs[0];
  }
  const std::vector<rowgroup::RowGroup>& getSmallRowGroups() const
  {
    return smallRGs;
  }
  const rowgroup::RowGroup& getLargeRowGroup() const
  {
    return largeRG;
  }
  uint32_t getSmallKey() const
  {
    return smallSideKeys[0][0];
  }
  const std::vector<std::vector<uint32_t>>& getSmallKeys() const
  {
    return smallSideKeys;
  }
  const std::vector<std::vector<uint32_t>>& getLargeKeys() const
  {
    return largeSideKeys;
  }

  /* Some compat fcns to get rid of later */
  void oid1(execplan::CalpontSystemCatalog::OID oid)
  {
    fOid1 = oid;
  }
  execplan::CalpontSystemCatalog::OID oid1() const
  {
    return fOid1;
  }
  void oid2(execplan::CalpontSystemCatalog::OID oid)
  {
    fOid2 = oid;
  }
  execplan::CalpontSystemCatalog::OID oid2() const
  {
    return fOid2;
  }
  void dictOid1(execplan::CalpontSystemCatalog::OID oid)
  {
    fDictOid1 = oid;
  }
  execplan::CalpontSystemCatalog::OID dictOid1() const
  {
    return fDictOid1;
  }
  void dictOid2(execplan::CalpontSystemCatalog::OID oid)
  {
    fDictOid2 = oid;
  }
  execplan::CalpontSystemCatalog::OID dictOid2() const
  {
    return fDictOid2;
  }

  /* The replacements.  I don't think there's a need for setters or vars.
      OIDs are already in the rowgroups. */
  // s - sth table pair; k - kth key in compound join, 0 for non-compand join
  execplan::CalpontSystemCatalog::OID smallSideKeyOID(uint32_t s, uint32_t k) const;
  execplan::CalpontSystemCatalog::OID largeSideKeyOID(uint32_t s, uint32_t k) const;

  void deliveryStep(const SJSTEP& ds)
  {
    fDeliveryStep = ds;
  }

  /* Iteration 18 mods */
  void setLargeSideDLIndex(uint32_t i)
  {
    largeSideIndex = i;
  }

  /* obsolete, need to update JLF */
  void setJoinType(JoinType jt)
  {
    joinType = jt;
  }
  JoinType getJoinType() const
  {
    return joinType;
  }

  /* Functions & Expressions interface */
  /* Cross-table Functions & Expressions in where clause */
  void addFcnExpGroup2(const boost::shared_ptr<execplan::ParseTree>& fe);
  bool hasFcnExpGroup2()
  {
    return (fe2 != nullptr);
  }

  /* Functions & Expressions in select and groupby clause */
  void setFcnExpGroup3(const std::vector<execplan::SRCP>& fe) override;
  void setFE23Output(const rowgroup::RowGroup& rg) override;

  /* result rowgroup */
  const rowgroup::RowGroup& getDeliveredRowGroup() const override;
  void deliverStringTableRowGroup(bool b) override;
  bool deliverStringTableRowGroup() const override;

  // joinId
  void joinId(int64_t id)
  {
    fJoinId = id;
  }
  int64_t joinId() const
  {
    return fJoinId;
  }

  /* semi-join support */
  void addJoinFilter(boost::shared_ptr<execplan::ParseTree>, uint32_t index);
  bool hasJoinFilter() const
  {
    return (fe.size() > 0);
  }
  bool hasJoinFilter(uint32_t index) const;
  boost::shared_ptr<funcexp::FuncExpWrapper> getJoinFilter(uint32_t index) const;
  void setJoinFilterInputRG(const rowgroup::RowGroup& rg);

  bool stringTableFriendly() override
  {
    return true;
  }

  uint32_t tokenJoin() const
  {
    return fTokenJoin;
  }
  void tokenJoin(uint32_t k)
  {
    fTokenJoin = k;
  }

  //@bug3683 function join
  boost::shared_ptr<FunctionJoinInfo>& funcJoinInfo()
  {
    return fFunctionJoinInfo;
  }
  void funcJoinInfo(const boost::shared_ptr<FunctionJoinInfo>& fji)
  {
    fFunctionJoinInfo = fji;
  }

  void abort() override;
  void returnMemory()
  {
    if (fMemSizeForOutputRG > 0)
    {
      resourceManager->returnMemory(fMemSizeForOutputRG);
      fMemSizeForOutputRG = 0;
    }
  }
  bool getMemory(uint64_t memSize)
  {
    bool gotMem = resourceManager->getMemory(memSize);
    if (gotMem)
      fMemSizeForOutputRG += memSize;
    return gotMem;
  }

 private:
  TupleHashJoinStep();
  TupleHashJoinStep(const TupleHashJoinStep&);
  TupleHashJoinStep& operator=(const TupleHashJoinStep&);

  void errorLogging(const std::string& msg, int err) const;
  void startAdjoiningSteps();

  void formatMiniStatsPerJoiner(uint32_t index);
  void formatMiniStats();

  RowGroupDL *largeDL, *outputDL;
  std::vector<RowGroupDL*> smallDLs;
  std::vector<uint32_t> smallIts;
  uint32_t largeIt;

  JoinType joinType;  // deprecated
  std::vector<JoinType> joinTypes;
  execplan::CalpontSystemCatalog::OID fTableOID1;
  execplan::CalpontSystemCatalog::OID fTableOID2;
  execplan::CalpontSystemCatalog::OID fOid1;
  execplan::CalpontSystemCatalog::OID fOid2;

  // v-table string join
  execplan::CalpontSystemCatalog::OID fDictOid1;
  execplan::CalpontSystemCatalog::OID fDictOid2;

  std::string fAlias1;
  std::string fAlias2;

  std::string fView1;
  std::string fView2;

  std::string fSchema1;
  std::string fSchema2;

  int32_t fSequence1;
  int32_t fSequence2;

  // @bug3398, add tuple id to steps
  uint64_t fTupleId1;
  uint64_t fTupleId2;

  // @bug3524
  // for NOT IN subquery where correlate join in subquery is treated as additional comparison.
  // These simple columns are for converting join to expression.
  // DON'T delete, they owned by exec plan.
  const execplan::ReturnedColumn* fColumn1;
  const execplan::ReturnedColumn* fColumn2;

  int fCorrelatedSide;

  std::vector<bool> typelessJoin;  // the size of the vector is # of small side
  std::vector<std::vector<uint32_t>> largeSideKeys;
  std::vector<std::vector<uint32_t>> smallSideKeys;

  ResourceManager* resourceManager;
  uint64_t fMemSizeForOutputRG;

  struct JoinerSorter
  {
    inline bool operator()(const std::shared_ptr<joiner::TupleJoiner>& j1,
                           const std::shared_ptr<joiner::TupleJoiner>& j2) const
    {
      return *j1 < *j2;
    }
  };
  std::vector<std::shared_ptr<joiner::TupleJoiner>> joiners;
  boost::scoped_array<std::vector<rowgroup::RGData>> rgData;
  TupleBPS* largeBPS;
  rowgroup::RowGroup largeRG, outputRG;
  std::vector<rowgroup::RowGroup> smallRGs;
  ssize_t pmMemLimit;

  void hjRunner();
  void smallRunnerFcn(uint32_t index, uint threadID, uint64_t* threads);

  struct HJRunner
  {
    explicit HJRunner(TupleHashJoinStep* hj) : HJ(hj)
    {
    }
    void operator()()
    {
      utils::setThreadName("HJSBigSide");
      HJ->hjRunner();
    }
    TupleHashJoinStep* HJ;
  };
  struct SmallRunner
  {
    SmallRunner(TupleHashJoinStep* hj, uint32_t i) : HJ(hj), index(i)
    {
    }
    void operator()()
    {
      HJ->startSmallRunners(index);
    }
    TupleHashJoinStep* HJ;
    uint32_t index;
  };

  int64_t mainRunner;  // thread handle from thread pool

  // for notify TupleAggregateStep PM hashjoin
  // Ideally, hashjoin and delivery communicate with RowGroupDL,
  // they don't need to know each other.
  // Due to dynamic PM/UM hashjoin selection and support PM aggregation,
  // delivery step need to know if raw or partially aggregated to process.
  SJSTEP fDeliveryStep;

  // temporary hack to make sure JobList only calls run, join once
  boost::mutex jlLock;
  bool runRan, joinRan;

  /* Iteration 18 mods */
  uint32_t largeSideIndex;
  bool joinIsTooBig;

  /* Functions & Expressions support */
  boost::shared_ptr<funcexp::FuncExpWrapper> fe2;
  std::vector<uint32_t> fe2TableDeps;
  rowgroup::RowGroup fe2Output;
  bool runFE2onPM;

  // Support Mixed Join Type
  int64_t fJoinId;

  /* Semi-join support */
  std::vector<int> feIndexes;
  std::vector<boost::shared_ptr<funcexp::FuncExpWrapper>> fe;
  rowgroup::RowGroup joinFilterRG;

  /* Casual Partitioning forwarding */
  void forwardCPData();
  uint32_t uniqueLimit;

  /* UM Join support.  Most of this code is ported from the UM join code in tuple-bps.cpp.
   * They should be kept in sync as much as possible. */
  struct JoinRunner
  {
    JoinRunner(TupleHashJoinStep* hj, uint32_t i) : HJ(hj), index(i)
    {
    }
    void operator()()
    {
      std::string name = "HJSJoinRun" + std::to_string(index);
      utils::setThreadName(name.c_str());
      HJ->joinRunnerFcn(index);
    }
    TupleHashJoinStep* HJ;
    uint32_t index;
  };
  void joinRunnerFcn(uint32_t index);
  void startJoinThreads();
  void generateJoinResultSet(const uint32_t threadID, const std::vector<std::vector<rowgroup::Row::Pointer>>& joinerOutput,
                             rowgroup::Row& baseRow,
                             const std::shared_ptr<std::shared_ptr<int[]>[]>& mappings, const uint32_t depth,
                             rowgroup::RowGroup& outputRG, rowgroup::RGData& rgData,
                             std::vector<rowgroup::RGData>& outputData,
                             const std::shared_ptr<rowgroup::Row[]>& smallRows, rowgroup::Row& joinedRow,
                             RowGroupDL* outputDL);
  void grabSomeWork(std::vector<rowgroup::RGData>* work);
  void sendResult(const std::vector<rowgroup::RGData>& res);
  void processFE2(rowgroup::RowGroup& input, rowgroup::RowGroup& output, rowgroup::Row& inRow,
                  rowgroup::Row& outRow, std::vector<rowgroup::RGData>* rgData,
                  funcexp::FuncExpWrapper* local_fe);
  void joinOneRG(uint32_t threadID, std::vector<rowgroup::RGData>& out, rowgroup::RowGroup& inputRG,
                 rowgroup::RowGroup& joinOutput, rowgroup::Row& largeSideRow, rowgroup::Row& joinFERow,
                 rowgroup::Row& joinedRow, rowgroup::Row& baseRow,
                 std::vector<std::vector<rowgroup::Row::Pointer>>& joinMatches,
                 std::shared_ptr<rowgroup::Row[]>& smallRowTemplates, RowGroupDL* outputDL,
                 std::vector<std::shared_ptr<joiner::TupleJoiner>>* joiners = nullptr,
                 std::shared_ptr<std::shared_ptr<int[]>[]>* rgMappings = nullptr,
                 std::shared_ptr<std::shared_ptr<int[]>[]>* feMappings = nullptr,
                 boost::scoped_array<boost::scoped_array<uint8_t>>* smallNullMem = nullptr);
  void finishSmallOuterJoin();
  void makeDupList(const rowgroup::RowGroup& rg);
  void processDupList(uint32_t threadID, rowgroup::RowGroup& ingrp, std::vector<rowgroup::RGData>* rowData);

  std::vector<uint64_t> joinRunners;  // thread handles from thread pool
  boost::mutex inputDLLock, outputDLLock;
  std::shared_ptr<std::shared_ptr<int[]>[]> columnMappings, fergMappings;
  std::shared_ptr<int[]> fe2Mapping;
  uint32_t joinThreadCount;
  boost::scoped_array<boost::scoped_array<uint8_t>> smallNullMemory;
  uint64_t outputIt;
  bool moreInput;
  std::vector<std::pair<uint32_t, uint32_t>> dupList;
  boost::scoped_array<rowgroup::Row> dupRows;
  std::vector<std::string> smallTableNames;
  bool isExeMgr;
  uint32_t lastSmallOuterJoiner;

  //@bug5958 & 6117, stores the table key for identify token join
  uint32_t fTokenJoin;

  // moved from base class JobStep
  boost::mutex* fStatsMutexPtr;

  //@bug3683 function join
  boost::shared_ptr<FunctionJoinInfo> fFunctionJoinInfo;
  std::set<uint32_t> fFunctionJoinKeys;  // for skipping CP forward

  /* Disk-based join support */
  std::vector<std::shared_ptr<DiskJoinStep>> djs;
  boost::scoped_array<boost::shared_ptr<RowGroupDL>> fifos;
  void djsReaderFcn(int index);
  uint64_t djsReader;  // thread handle from thread pool
  struct DJSReader
  {
    DJSReader(TupleHashJoinStep* hj, uint32_t i) : HJ(hj), index(i)
    {
    }
    void operator()()
    {
      utils::setThreadName("DJSReader");
      HJ->djsReaderFcn(index);
    }
    TupleHashJoinStep* HJ;
    uint32_t index;
  };

  uint64_t djsRelay;  // thread handle from thread pool
  void djsRelayFcn();
  struct DJSRelay
  {
    explicit DJSRelay(TupleHashJoinStep* hj) : HJ(hj)
    {
    }
    void operator()()
    {
      HJ->djsRelayFcn();
    }
    TupleHashJoinStep* HJ;
  };

  boost::shared_ptr<int64_t> djsSmallUsage;
  int64_t djsSmallLimit;
  int64_t djsLargeLimit;
  uint64_t djsPartitionSize;
  uint32_t djsMaxPartitionTreeDepth;
  bool djsForceRun;
  bool isDML;
  bool allowDJS;

  // hacky mechanism to prevent nextBand from starting before the final
  // THJS configuration is settled.  Debatable whether to use a bool and poll instead;
  // once the config is settled it stays settled, technically no need to
  // keep grabbing locks after that.
  boost::mutex deliverMutex;
  bool ownsOutputDL;

  void segregateJoiners();
  std::vector<std::shared_ptr<joiner::TupleJoiner>> tbpsJoiners;
  std::vector<std::shared_ptr<joiner::TupleJoiner>> djsJoiners;
  std::vector<size_t> joinerRunnerInputRecordsStats;
  std::vector<size_t> joinerRunnerInputMatchedStats;
  std::vector<int> djsJoinerMap;
  boost::scoped_array<ssize_t> memUsedByEachJoin;
  boost::mutex djsLock;
  boost::shared_ptr<int64_t> sessionMemLimit;

  /* Threaded UM join support */
  int numCores;
  boost::mutex dlMutex, memTrackMutex, saneErrMsg;
  boost::condition memTrackDone;
  std::atomic<bool> rgdLock;
  bool stopMemTracking;
  void trackMem(uint index);
  void startSmallRunners(uint index);
  void outOfMemoryHandler(std::shared_ptr<joiner::TupleJoiner> joiner);

  friend class DiskJoinStep;
};

}  // namespace joblist

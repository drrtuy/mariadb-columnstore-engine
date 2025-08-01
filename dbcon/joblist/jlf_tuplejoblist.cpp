/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2019-2021 MariaDB Corporation

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

//  $Id: jlf_tuplejoblist.cpp 9728 2013-07-26 22:08:20Z xlou $

// Cross engine needs to be at the top due to MySQL includes
#define PREFER_MY_CONFIG_H
#include "crossenginestep.h"
#include <iostream>
#include <stack>
#include <iterator>
#include <algorithm>
// #define NDEBUG
// #include <cassert>
#include <vector>
#include <set>
#include <map>
#include <limits>
using namespace std;

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
using namespace boost;

#include "calpontsystemcatalog.h"
#include "logicoperator.h"
using namespace execplan;

#include "rowgroup.h"
#include "rowaggregation.h"
using namespace rowgroup;

#include "idberrorinfo.h"
#include "errorids.h"
#include "exceptclasses.h"
using namespace logging;

#include "dataconvert.h"
using namespace dataconvert;

#include "elementtype.h"
#include "jlf_common.h"
#include "limitedorderby.h"
#include "jobstep.h"
#include "primitivestep.h"
#include "expressionstep.h"
#include "subquerystep.h"
#include "tupleaggregatestep.h"
#include "tupleannexstep.h"
#include "tupleconstantstep.h"
#include "tuplehashjoin.h"
#include "tuplehavingstep.h"
#include "tupleunion.h"
#include "windowfunctionstep.h"
#include "configcpp.h"
#include "jlf_tuplejoblist.h"
using namespace joblist;

#include "statistics_manager/statistics.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpotentially-evaluated-expression"
// for warnings on typeid :expression with side effects will be evaluated despite being used as an operand to
// 'typeid'
#endif

namespace
{
// construct a pcolstep from column key
void tupleKeyToProjectStep(uint32_t key, JobStepVector& jsv, JobInfo& jobInfo)
{
  // this JSA is for pcolstep construct, is not taking input/output
  // because the pcolstep is to be added into TBPS
  CalpontSystemCatalog::OID oid = jobInfo.keyInfo->tupleKeyVec[key].fId;
  DictOidToColOidMap::iterator mit = jobInfo.keyInfo->dictOidToColOid.find(oid);

  // if the key is for a dictionary, start with its token key
  if (mit != jobInfo.keyInfo->dictOidToColOid.end())
  {
    oid = mit->second;

    for (map<uint32_t, uint32_t>::iterator i = jobInfo.keyInfo->dictKeyMap.begin();
         i != jobInfo.keyInfo->dictKeyMap.end(); i++)
    {
      if (key == i->second)
      {
        key = i->first;
        break;
      }
    }

    jobInfo.tokenOnly[key] = false;
  }

  CalpontSystemCatalog::OID tableOid = jobInfo.keyInfo->tupleKeyToTableOid[key];
  //	JobStepAssociation dummyJsa;
  //	AnyDataListSPtr adl(new AnyDataList());
  //	RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
  //	dl->OID(oid);
  //	adl->rowGroupDL(dl);
  //	dummyJsa.outAdd(adl);

  CalpontSystemCatalog::ColType ct = jobInfo.keyInfo->colType[key];

  if (jobInfo.keyInfo->token2DictTypeMap.find(key) != jobInfo.keyInfo->token2DictTypeMap.end())
    ct = jobInfo.keyInfo->token2DictTypeMap[key];

  uint32_t pt = jobInfo.keyInfo->pseudoType[key];

  SJSTEP sjs;

  if (pt == 0)
    sjs.reset(new pColStep(oid, tableOid, ct, jobInfo));
  else
    sjs.reset(new PseudoColStep(oid, tableOid, pt, ct, jobInfo));

  sjs->alias(jobInfo.keyInfo->tupleKeyVec[key].fTable);
  sjs->view(jobInfo.keyInfo->tupleKeyVec[key].fView);
  sjs->schema(jobInfo.keyInfo->tupleKeyVec[key].fSchema);
  sjs->name(jobInfo.keyInfo->keyName[key]);
  sjs->tupleId(key);

  jsv.push_back(sjs);

  bool tokenOnly = false;
  map<uint32_t, bool>::iterator toIt = jobInfo.tokenOnly.find(key);

  if (toIt != jobInfo.tokenOnly.end())
    tokenOnly = toIt->second;

  if (sjs.get()->isDictCol() && !tokenOnly)
  {
    // Need a dictionary step
    uint32_t dictKey = jobInfo.keyInfo->dictKeyMap[key];
    CalpontSystemCatalog::OID dictOid = jobInfo.keyInfo->tupleKeyVec[dictKey].fId;
    sjs.reset(new pDictionaryStep(dictOid, tableOid, ct, jobInfo));
    sjs->alias(jobInfo.keyInfo->tupleKeyVec[dictKey].fTable);
    sjs->view(jobInfo.keyInfo->tupleKeyVec[dictKey].fView);
    sjs->schema(jobInfo.keyInfo->tupleKeyVec[dictKey].fSchema);
    sjs->name(jobInfo.keyInfo->keyName[dictKey]);
    sjs->tupleId(dictKey);

    jobInfo.keyInfo->dictOidToColOid[dictOid] = oid;

    jsv.push_back(sjs);
  }
}

inline void addColumnToRG(uint32_t cid, vector<uint32_t>& pos, vector<uint32_t>& oids, vector<uint32_t>& keys,
                          vector<uint32_t>& scale, vector<uint32_t>& precision,
                          vector<CalpontSystemCatalog::ColDataType>& types, vector<uint32_t>& csNums,
                          JobInfo& jobInfo)
{
  TupleInfo ti(getTupleInfo(cid, jobInfo));
  pos.push_back(pos.back() + ti.width);
  oids.push_back(ti.oid);
  keys.push_back(ti.key);
  types.push_back(ti.dtype);
  csNums.push_back(ti.csNum);
  scale.push_back(ti.scale);
  precision.push_back(ti.precision);
}

inline void addColumnInExpToRG(uint32_t cid, vector<uint32_t>& pos, vector<uint32_t>& oids,
                               vector<uint32_t>& keys, vector<uint32_t>& scale, vector<uint32_t>& precision,
                               vector<CalpontSystemCatalog::ColDataType>& types, vector<uint32_t>& csNums,
                               JobInfo& jobInfo)
{
  if (jobInfo.keyInfo->dictKeyMap.find(cid) != jobInfo.keyInfo->dictKeyMap.end())
    cid = jobInfo.keyInfo->dictKeyMap[cid];

  if (find(keys.begin(), keys.end(), cid) == keys.end())
    addColumnToRG(cid, pos, oids, keys, scale, precision, types, csNums, jobInfo);
}

inline void addColumnsToRG(uint32_t tid, vector<uint32_t>& pos, vector<uint32_t>& oids,
                           vector<uint32_t>& keys, vector<uint32_t>& scale, vector<uint32_t>& precision,
                           vector<CalpontSystemCatalog::ColDataType>& types, vector<uint32_t>& csNums,
                           TableInfoMap& tableInfoMap, JobInfo& jobInfo)
{
  // -- the selected columns
  vector<uint32_t>& pjCol = tableInfoMap[tid].fProjectCols;

  for (unsigned i = 0; i < pjCol.size(); i++)
  {
    addColumnToRG(pjCol[i], pos, oids, keys, scale, precision, types, csNums, jobInfo);
  }

  // -- any columns will be used in cross-table exps
  vector<uint32_t>& exp2 = tableInfoMap[tid].fColsInExp2;

  for (unsigned i = 0; i < exp2.size(); i++)
  {
    addColumnInExpToRG(exp2[i], pos, oids, keys, scale, precision, types, csNums, jobInfo);
  }

  // -- any columns will be used in returned exps
  vector<uint32_t>& expr = tableInfoMap[tid].fColsInRetExp;

  for (unsigned i = 0; i < expr.size(); i++)
  {
    addColumnInExpToRG(expr[i], pos, oids, keys, scale, precision, types, csNums, jobInfo);
  }

  // -- any columns will be used in final outer join expression
  vector<uint32_t>& expo = tableInfoMap[tid].fColsInOuter;

  for (unsigned i = 0; i < expo.size(); i++)
  {
    addColumnInExpToRG(expo[i], pos, oids, keys, scale, precision, types, csNums, jobInfo);
  }
}

void constructJoinedRowGroup(RowGroup& rg, uint32_t large, uint32_t prev, bool root, set<uint32_t>& tableSet,
                             TableInfoMap& tableInfoMap, JobInfo& jobInfo)
{
  // Construct the output rowgroup for the join.
  vector<uint32_t> pos;
  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  pos.push_back(2);

  // -- start with the join keys
  // lead by joinkeys -- to have more controls on joins
  //    [loop throuh the key list to support compound join]
  if (root == false)  // not root
  {
    vector<uint32_t>& joinKeys = jobInfo.tableJoinMap[make_pair(large, prev)].fLeftKeys;

    for (vector<uint32_t>::iterator i = joinKeys.begin(); i != joinKeys.end(); i++)
      addColumnToRG(*i, pos, oids, keys, scale, precision, types, csNums, jobInfo);
  }

  // -- followed by the columns in select or expression
  for (set<uint32_t>::iterator i = tableSet.begin(); i != tableSet.end(); i++)
    addColumnsToRG(*i, pos, oids, keys, scale, precision, types, csNums, tableInfoMap, jobInfo);

  RowGroup tmpRg(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold);
  rg = tmpRg;
}

void constructJoinedRowGroup(RowGroup& rg, set<uint32_t>& tableSet, TableInfoMap& tableInfoMap,
                             JobInfo& jobInfo)
{
  // Construct the output rowgroup for the join.
  vector<uint32_t> pos;
  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  pos.push_back(2);

  for (set<uint32_t>::iterator i = tableSet.begin(); i != tableSet.end(); i++)
  {
    // columns in select or expression
    addColumnsToRG(*i, pos, oids, keys, scale, precision, types, csNums, tableInfoMap, jobInfo);

    // keys to be joined if not already in the rowgroup
    vector<uint32_t>& adjList = tableInfoMap[*i].fAdjacentList;

    for (vector<uint32_t>::iterator j = adjList.begin(); j != adjList.end(); j++)
    {
      if (find(tableSet.begin(), tableSet.end(), *j) == tableSet.end())
      {
        // not joined
        vector<uint32_t>& joinKeys = jobInfo.tableJoinMap[make_pair(*i, *j)].fLeftKeys;

        for (vector<uint32_t>::iterator k = joinKeys.begin(); k != joinKeys.end(); k++)
        {
          if (find(keys.begin(), keys.end(), *k) == keys.end())
            addColumnToRG(*k, pos, oids, keys, scale, precision, types, csNums, jobInfo);
        }
      }
    }
  }

  RowGroup tmpRg(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold);
  rg = tmpRg;
}

void updateExp2Cols(JobStepVector& expSteps, TableInfoMap& tableInfoMap, JobInfo& /*jobInfo*/)
{
  for (JobStepVector::iterator it = expSteps.begin(); it != expSteps.end(); it++)
  {
    ExpressionStep* exps = dynamic_cast<ExpressionStep*>(it->get());
    const vector<uint32_t>& tables = exps->tableKeys();
    const vector<uint32_t>& columns = exps->columnKeys();

    for (uint64_t i = 0; i < tables.size(); ++i)
    {
      vector<uint32_t>& exp2 = tableInfoMap[tables[i]].fColsInExp2;
      vector<uint32_t>::iterator cit = find(exp2.begin(), exp2.end(), columns[i]);

      if (cit != exp2.end())
        exp2.erase(cit);
    }
  }
}

void adjustLastStep(JobStepVector& querySteps, DeliveredTableMap& deliverySteps, JobInfo& jobInfo)
{
  SJSTEP spjs = querySteps.back();
  BatchPrimitive* bps = dynamic_cast<BatchPrimitive*>(spjs.get());
  TupleHashJoinStep* thjs = dynamic_cast<TupleHashJoinStep*>(spjs.get());
  SubAdapterStep* sas = dynamic_cast<SubAdapterStep*>(spjs.get());

  if (!bps && !thjs && !sas)
    throw runtime_error("Bad last step");

  // original output rowgroup of the step
  TupleJobStep* tjs = dynamic_cast<TupleJobStep*>(spjs.get());
  const RowGroup* rg0 = &(tjs->getOutputRowGroup());

  if (jobInfo.trace)
    cout << "Output RowGroup 0: " << rg0->toString() << endl;

  // Construct a rowgroup that matches the select columns
  TupleInfoVector v = jobInfo.pjColList;
  vector<uint32_t> pos;
  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  pos.push_back(2);

  for (unsigned i = 0; i < v.size(); i++)
  {
    pos.push_back(pos.back() + v[i].width);
    oids.push_back(v[i].oid);
    keys.push_back(v[i].key);
    types.push_back(v[i].dtype);
    csNums.push_back(v[i].csNum);
    scale.push_back(v[i].scale);
    precision.push_back(v[i].precision);
  }

  RowGroup rg1(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold);

  // evaluate the returned/groupby expressions if any
  JobStepVector& expSteps = jobInfo.returnedExpressions;

  if (expSteps.size() > 0)
  {
    // create a RG has the keys not in rg0
    pos.clear();
    oids.clear();
    keys.clear();
    scale.clear();
    precision.clear();
    types.clear();
    csNums.clear();
    pos.push_back(2);

    const vector<uint32_t>& keys0 = rg0->getKeys();

    for (unsigned i = 0; i < v.size(); i++)
    {
      if (find(keys0.begin(), keys0.end(), v[i].key) == keys0.end())
      {
        pos.push_back(pos.back() + v[i].width);
        oids.push_back(v[i].oid);
        keys.push_back(v[i].key);
        types.push_back(v[i].dtype);
        csNums.push_back(v[i].csNum);
        scale.push_back(v[i].scale);
        precision.push_back(v[i].precision);
      }
    }

    // for v0.9.3.0, the output and input to the expression are in the same row
    // add the returned column into the rg0 as rg01
    RowGroup rg01 = *rg0 + RowGroup(oids.size(), pos, oids, keys, types, csNums, scale, precision,
                                    jobInfo.stringTableThreshold);

    if (jobInfo.trace)
      cout << "Output RowGroup 01: " << rg01.toString() << endl;

    map<uint32_t, uint32_t> keyToIndexMap0;  // maps key to the index in the input RG

    for (uint64_t i = 0; i < rg01.getKeys().size(); ++i)
      keyToIndexMap0.insert(make_pair(rg01.getKeys()[i], i));

    vector<SRCP> exps;  // columns to be evaluated

    for (JobStepVector::iterator eit = expSteps.begin(); eit != expSteps.end(); ++eit)
    {
      ExpressionStep* es = dynamic_cast<ExpressionStep*>(eit->get());
      es->updateInputIndex(keyToIndexMap0, jobInfo);
      es->updateOutputIndex(keyToIndexMap0, jobInfo);  // same row as input
      exps.push_back(es->expression());
    }

    // last step can be tbps (no join) or thjs, either one can have a group 3 expression
    if (bps || thjs)
    {
      // this part may set FE2 (setFE23Output()) and may affect behavior of PrimProc's
      // batchprimitiveprocessor's execute() function when processing aggregates.
      tjs->setOutputRowGroup(rg01);
      tjs->setFcnExpGroup3(exps);
      tjs->setFE23Output(rg1);
    }
    else if (sas)
    {
      sas->setFeRowGroup(rg01);
      sas->addExpression(exps);
      sas->setOutputRowGroup(rg1);
    }
  }
  else
  {
    // this may change behavior in the primproc side, look into
    // a primitives/prim-proc/batchprimitiveprocessor.
    // This is especially important for aggregation.
    if (thjs && thjs->hasFcnExpGroup2())
      thjs->setFE23Output(rg1);
    else
      tjs->setOutputRowGroup(rg1);
  }

  if (jobInfo.trace)
    cout << "Output RowGroup 1: " << rg1.toString() << endl;

  if (jobInfo.hasAggregation == false)
  {
    if (thjs != NULL)  // setup a few things for the final thjs step...
      thjs->outputAssociation(JobStepAssociation());

    deliverySteps[CNX_VTABLE_ID] = spjs;
  }
  else
  {
    TupleDeliveryStep* tds = dynamic_cast<TupleDeliveryStep*>(spjs.get());
    idbassert(tds != NULL);
    SJSTEP ads = TupleAggregateStep::prepAggregate(spjs, jobInfo);
    querySteps.push_back(ads);

    if (ads.get() != NULL)
      deliverySteps[CNX_VTABLE_ID] = ads;
    else
      throw std::logic_error("Failed to prepare Aggregation Delivery Step.");
  }

  if (jobInfo.havingStep)
  {
    TupleDeliveryStep* ds = dynamic_cast<TupleDeliveryStep*>(deliverySteps[CNX_VTABLE_ID].get());

    AnyDataListSPtr spdlIn(new AnyDataList());
    RowGroupDL* dlIn = new RowGroupDL(1, jobInfo.fifoSize);
    dlIn->OID(CNX_VTABLE_ID);
    spdlIn->rowGroupDL(dlIn);
    JobStepAssociation jsaIn;
    jsaIn.outAdd(spdlIn);
    dynamic_cast<JobStep*>(ds)->outputAssociation(jsaIn);
    jobInfo.havingStep->inputAssociation(jsaIn);

    AnyDataListSPtr spdlOut(new AnyDataList());
    RowGroupDL* dlOut = new RowGroupDL(1, jobInfo.fifoSize);
    dlOut->OID(CNX_VTABLE_ID);
    spdlOut->rowGroupDL(dlOut);
    JobStepAssociation jsaOut;
    jsaOut.outAdd(spdlOut);
    jobInfo.havingStep->outputAssociation(jsaOut);

    querySteps.push_back(jobInfo.havingStep);
    dynamic_cast<TupleHavingStep*>(jobInfo.havingStep.get())->initialize(ds->getDeliveredRowGroup(), jobInfo);
    deliverySteps[CNX_VTABLE_ID] = jobInfo.havingStep;
  }

  if (jobInfo.windowCols.size() > 0)
  {
    spjs = querySteps.back();
    SJSTEP ws = WindowFunctionStep::makeWindowFunctionStep(spjs, jobInfo);
    idbassert(ws.get());
    querySteps.push_back(ws);
    deliverySteps[CNX_VTABLE_ID] = ws;
  }

  // TODO MCOL-894 we don't need to run sorting|distinct
  // every time
  //    if ((jobInfo.limitCount != (uint64_t) - 1) ||
  //            (jobInfo.constantCol == CONST_COL_EXIST) ||
  //            (jobInfo.hasDistinct))
  //    {
  if (jobInfo.annexStep.get() == NULL)
    jobInfo.annexStep.reset(new TupleAnnexStep(jobInfo));

  TupleAnnexStep* tas = dynamic_cast<TupleAnnexStep*>(jobInfo.annexStep.get());
  tas->setLimit(jobInfo.limitStart, jobInfo.limitCount);

  if (jobInfo.orderByColVec.size() > 0)
  {
    tas->addOrderBy(new LimitedOrderBy());
    if (jobInfo.orderByThreads > 1)
      tas->setParallelOp();
    tas->setMaxThreads(jobInfo.orderByThreads);
  }

  if (jobInfo.constantCol == CONST_COL_EXIST)
    tas->addConstant(new TupleConstantStep(jobInfo));

  if (jobInfo.hasDistinct)
    tas->setDistinct();

  //    }

  if (jobInfo.annexStep)
  {
    TupleDeliveryStep* ds = dynamic_cast<TupleDeliveryStep*>(deliverySteps[CNX_VTABLE_ID].get());
    RowGroup rg2 = ds->getDeliveredRowGroup();

    if (jobInfo.trace)
      cout << "Output RowGroup 2: " << rg2.toString() << endl;

    AnyDataListSPtr spdlIn(new AnyDataList());
    RowGroupDL* dlIn;
    if (jobInfo.orderByColVec.size() > 0)
      dlIn = new RowGroupDL(jobInfo.orderByThreads, jobInfo.fifoSize);
    else
      dlIn = new RowGroupDL(1, jobInfo.fifoSize);
    dlIn->OID(CNX_VTABLE_ID);
    spdlIn->rowGroupDL(dlIn);
    JobStepAssociation jsaIn;
    jsaIn.outAdd(spdlIn);
    dynamic_cast<JobStep*>(ds)->outputAssociation(jsaIn);
    jobInfo.annexStep->inputAssociation(jsaIn);

    AnyDataListSPtr spdlOut(new AnyDataList());
    RowGroupDL* dlOut = new RowGroupDL(1, jobInfo.fifoSize);
    dlOut->OID(CNX_VTABLE_ID);
    spdlOut->rowGroupDL(dlOut);
    JobStepAssociation jsaOut;
    jsaOut.outAdd(spdlOut);
    jobInfo.annexStep->outputAssociation(jsaOut);

    querySteps.push_back(jobInfo.annexStep);
    dynamic_cast<TupleAnnexStep*>(jobInfo.annexStep.get())->initialize(rg2, jobInfo);
    deliverySteps[CNX_VTABLE_ID] = jobInfo.annexStep;
  }

  // Check if constant false
  if (jobInfo.constantFalse)
  {
    TupleConstantBooleanStep* tcs = new TupleConstantBooleanStep(jobInfo, false);
    tcs->outputAssociation(querySteps.back().get()->outputAssociation());
    TupleDeliveryStep* tds = dynamic_cast<TupleDeliveryStep*>(deliverySteps[CNX_VTABLE_ID].get());
    tcs->initialize(tds->getDeliveredRowGroup(), jobInfo);

    JobStepVector::iterator it = querySteps.begin();

    while (it != querySteps.end())
    {
      if ((dynamic_cast<TupleAggregateStep*>(it->get()) != NULL) ||
          (dynamic_cast<TupleAnnexStep*>(it->get()) != NULL))
        break;

      it++;
    }

    SJSTEP bs(tcs);

    if (it != querySteps.end())
      tcs->outputAssociation((*it)->inputAssociation());
    else
      deliverySteps[CNX_VTABLE_ID] = bs;

    querySteps.erase(querySteps.begin(), it);
    querySteps.insert(querySteps.begin(), bs);
  }

  if (jobInfo.trace)
  {
    TupleDeliveryStep* ds = dynamic_cast<TupleDeliveryStep*>(deliverySteps[CNX_VTABLE_ID].get());

    if (ds)
      cout << "Delivered RowGroup: " << ds->getDeliveredRowGroup().toString() << endl;
  }
}

// add the project steps into the query TBPS and construct the output rowgroup
void addProjectStepsToBps(TableInfoMap::iterator& mit, BatchPrimitive* bps, JobInfo& jobInfo)
{
  // make sure we have a good tuple bps
  if (bps == NULL)
    throw runtime_error("BPS is null");

  // construct a pcolstep for each joinkey to be projected
  vector<uint32_t>& joinKeys = mit->second.fJoinKeys;
  JobStepVector keySteps;
  vector<uint32_t> fjKeys;

  for (vector<uint32_t>::iterator kit = joinKeys.begin(); kit != joinKeys.end(); kit++)
  {
    if (jobInfo.keyInfo.get()->tupleKeyToTableOid[*kit] != CNX_EXP_TABLE_ID)
      tupleKeyToProjectStep(*kit, keySteps, jobInfo);
    else
      fjKeys.push_back(*kit);
  }

  // construct pcolstep for columns in expresssions
  JobStepVector expSteps;
  vector<uint32_t>& exp1 = mit->second.fColsInExp1;

  for (vector<uint32_t>::iterator kit = exp1.begin(); kit != exp1.end(); kit++)
    tupleKeyToProjectStep(*kit, expSteps, jobInfo);

  vector<uint32_t>& exp2 = mit->second.fColsInExp2;

  for (vector<uint32_t>::iterator kit = exp2.begin(); kit != exp2.end(); kit++)
    tupleKeyToProjectStep(*kit, expSteps, jobInfo);

  vector<uint32_t>& expRet = mit->second.fColsInRetExp;

  for (vector<uint32_t>::iterator kit = expRet.begin(); kit != expRet.end(); kit++)
    tupleKeyToProjectStep(*kit, expSteps, jobInfo);

  vector<uint32_t>& expOut = mit->second.fColsInOuter;

  for (vector<uint32_t>::iterator kit = expOut.begin(); kit != expOut.end(); kit++)
    tupleKeyToProjectStep(*kit, expSteps, jobInfo);

  vector<uint32_t>& expFj = mit->second.fColsInFuncJoin;

  for (vector<uint32_t>::iterator kit = expFj.begin(); kit != expFj.end(); kit++)
    tupleKeyToProjectStep(*kit, expSteps, jobInfo);

  // for output rowgroup
  vector<uint32_t> pos;
  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  pos.push_back(2);

  // this psv is a copy of the project steps, the original vector in mit is not changed
  JobStepVector psv = mit->second.fProjectSteps;              // columns being selected
  psv.insert(psv.begin(), keySteps.begin(), keySteps.end());  // add joinkeys to project
  psv.insert(psv.end(), expSteps.begin(), expSteps.end());    // add expressions to project
  set<uint32_t> seenCols;                                     // columns already processed

  // for passthru conversion
  // passthru is disabled (default lastTupleId to -1) unless the TupleBPS::bop is BOP_AND.
  uint64_t lastTupleId = -1;
  TupleBPS* tbps = dynamic_cast<TupleBPS*>(bps);

  if (tbps != NULL && tbps->getBOP() == BOP_AND && exp1.size() == 0)
    lastTupleId = tbps->getLastTupleId();

  for (JobStepVector::iterator it = psv.begin(); it != psv.end(); it++)
  {
    JobStep* js = it->get();
    uint32_t tupleKey = js->tupleId();

    if (seenCols.find(tupleKey) != seenCols.end())
      continue;

    // update processed column set
    seenCols.insert(tupleKey);

    // if the projected column is the last accessed predicate
    pColStep* pcol = dynamic_cast<pColStep*>(js);

    if (pcol != NULL && js->tupleId() == lastTupleId)
    {
      PassThruStep* pts = new PassThruStep(*pcol);

      if (dynamic_cast<PseudoColStep*>(pcol))
        pts->pseudoType(dynamic_cast<PseudoColStep*>(pcol)->pseudoColumnId());

      pts->alias(pcol->alias());
      pts->view(pcol->view());
      pts->name(pcol->name());
      pts->tupleId(pcol->tupleId());
      it->reset(pts);
    }

    // add projected column to TBPS
    bool tokenOnly = false;
    map<uint32_t, bool>::iterator toIt = jobInfo.tokenOnly.find(js->tupleId());

    if (toIt != jobInfo.tokenOnly.end())
      tokenOnly = toIt->second;

    if (it->get()->isDictCol() && !tokenOnly)
    {
      //			if (jobInfo.trace && bps->tableOid() >= 3000)
      //				cout << "1 setting project BPP for " << tbps->toString() << " with "
      //<< 					it->get()->toString() << " and " << (it+1)->get()->toString()
      //<< endl;
      bps->setProjectBPP(it->get(), (it + 1)->get());

      // this is a two-step project step, remove the token step from id vector
      vector<uint32_t>& pjv = mit->second.fProjectCols;
      uint32_t tokenKey = js->tupleId();

      for (vector<uint32_t>::iterator i = pjv.begin(); i != pjv.end(); ++i)
      {
        if (*i == tokenKey)
        {
          pjv.erase(i);
          break;
        }
      }

      // move to the dictionary step
      js = (++it)->get();
      tupleKey = js->tupleId();
      seenCols.insert(tupleKey);
    }
    else
    {
      //			if (jobInfo.trace && bps->tableOid() >= 3000)
      //				cout << "2 setting project BPP for " << tbps->toString() << " with "
      //<< 					it->get()->toString() << " and " << "NULL" << endl;
      bps->setProjectBPP(it->get(), NULL);
    }

    // add the tuple info of the column into the RowGroup
    TupleInfo ti(getTupleInfo(tupleKey, jobInfo));
    pos.push_back(pos.back() + ti.width);
    oids.push_back(ti.oid);
    keys.push_back(ti.key);
    types.push_back(ti.dtype);
    csNums.push_back(ti.csNum);
    scale.push_back(ti.scale);
    precision.push_back(ti.precision);
  }

  // add function join columns
  for (vector<uint32_t>::iterator i = fjKeys.begin(); i != fjKeys.end(); i++)
  {
    TupleInfo ti(getTupleInfo(*i, jobInfo));
    pos.push_back(pos.back() + ti.width);
    oids.push_back(ti.oid);
    keys.push_back(ti.key);
    types.push_back(ti.dtype);
    csNums.push_back(ti.csNum);
    scale.push_back(ti.scale);
    precision.push_back(ti.precision);
  }

  // construct RowGroup
  RowGroup rg(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold);

  // fix the output association
  AnyDataListSPtr spdl(new AnyDataList());
  RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
  spdl->rowGroupDL(dl);
  dl->OID(mit->first);
  JobStepAssociation jsa;
  jsa.outAdd(spdl);
  bps->outputAssociation(jsa);
  bps->setOutputRowGroup(rg);
}

// add one-table expression steps into the query TBPS
void addExpresssionStepsToBps(TableInfoMap::iterator& mit, SJSTEP& sjsp, JobInfo& jobInfo)
{
  BatchPrimitive* bps = dynamic_cast<BatchPrimitive*>(sjsp.get());
  CalpontSystemCatalog::OID tableOid = mit->second.fTableOid;
  JobStepVector& exps = mit->second.fOneTableExpSteps;
  JobStepVector& fjs = mit->second.fFuncJoinExps;
  ExpressionStep* exp0 = NULL;

  if (exps.size() > 0)
    exp0 = dynamic_cast<ExpressionStep*>(exps[0].get());
  else
    exp0 = dynamic_cast<ExpressionStep*>(fjs[0].get());

  if (bps == NULL)
  {
    if (tableOid > 0)
    {
      uint32_t key0 = exp0->columnKey();
      CalpontSystemCatalog::ColType ct = jobInfo.keyInfo->colType[key0];
      map<uint32_t, CalpontSystemCatalog::ColType>::iterator dkMit;

      if (jobInfo.keyInfo->token2DictTypeMap.find(key0) != jobInfo.keyInfo->token2DictTypeMap.end())
        ct = jobInfo.keyInfo->token2DictTypeMap[key0];

      scoped_ptr<pColScanStep> pcss(new pColScanStep(exp0->oid(), tableOid, ct, jobInfo));

      sjsp.reset(new TupleBPS(*pcss, jobInfo));
      TupleBPS* tbps = dynamic_cast<TupleBPS*>(sjsp.get());
      tbps->setJobInfo(&jobInfo);
      tbps->setFirstStepType(SCAN);

      // add the first column to BPP's filterSteps
      tbps->setBPP(pcss.get());

      bps = tbps;
    }
    else
    {
      sjsp.reset(new CrossEngineStep(mit->second.fSchema, mit->second.fName, mit->second.fPartitions,
                                     mit->second.fAlias, jobInfo));

      bps = dynamic_cast<CrossEngineStep*>(sjsp.get());
    }
  }

  // rowgroup for evaluating the one table expression
  vector<uint32_t> pos;
  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  pos.push_back(2);

  vector<uint32_t> cols;
  JobStepVector& fjExp = mit->second.fFuncJoinExps;

  for (JobStepVector::iterator it = fjExp.begin(); it != fjExp.end(); it++)
  {
    ExpressionStep* e = dynamic_cast<ExpressionStep*>(it->get());
    cols.push_back(getExpTupleKey(jobInfo, e->expressionId()));
  }

  cols.insert(cols.end(), mit->second.fColsInExp1.begin(), mit->second.fColsInExp1.end());
  cols.insert(cols.end(), mit->second.fColsInFuncJoin.begin(), mit->second.fColsInFuncJoin.end());
  uint32_t index = 0;                     // index in the rowgroup
  map<uint32_t, uint32_t> keyToIndexMap;  // maps key to the index in the RG

  for (vector<uint32_t>::iterator kit = cols.begin(); kit != cols.end(); kit++)
  {
    uint32_t key = *kit;

    if (jobInfo.keyInfo->dictKeyMap.find(key) != jobInfo.keyInfo->dictKeyMap.end())
      key = jobInfo.keyInfo->dictKeyMap[key];

    // check if this key is already in
    if (keyToIndexMap.find(key) != keyToIndexMap.end())
      continue;

    // update processed column set
    keyToIndexMap.insert(make_pair(key, index++));

    // add the tuple info of the column into the RowGroup
    TupleInfo ti(getTupleInfo(key, jobInfo));
    pos.push_back(pos.back() + ti.width);
    oids.push_back(ti.oid);
    keys.push_back(ti.key);
    types.push_back(ti.dtype);
    csNums.push_back(ti.csNum);
    scale.push_back(ti.scale);
    precision.push_back(ti.precision);
  }

  // construct RowGroup and add to TBPS
  RowGroup rg(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold);
  bps->setFE1Input(rg);

  if (jobInfo.trace)
    cout << "FE1 input RowGroup: " << rg.toString() << endl << endl;

  // add the expression steps into TBPS, the input-indices are set in SCs.
  for (JobStepVector::iterator it = exps.begin(); it != exps.end(); it++)
  {
    ExpressionStep* e = dynamic_cast<ExpressionStep*>(it->get());

    if (e->functionJoin())
      continue;

    e->updateInputIndex(keyToIndexMap, jobInfo);
    boost::shared_ptr<ParseTree> sppt(new ParseTree);
    sppt->copyTree(*(e->expressionFilter()));
    bps->addFcnExpGroup1(sppt);
  }

  // add the function join expression steps into TBPS, too
  if (fjs.size() > 0)
  {
    vector<SRCP> fjCols;

    for (JobStepVector::iterator it = fjs.begin(); it != fjs.end(); it++)
    {
      ExpressionStep* e = dynamic_cast<ExpressionStep*>(it->get());

      if (e->virtualStep())
        continue;

      e->updateInputIndex(keyToIndexMap, jobInfo);
      e->updateOutputIndex(keyToIndexMap, jobInfo);
      fjCols.push_back(e->expression());
    }

    bps->addFcnJoinExp(fjCols);
  }
}

bool combineJobStepsByTable(TableInfoMap::iterator& mit, JobInfo& jobInfo)
{
  TableInfo& tableInfo = mit->second;
  JobStepVector& qsv = tableInfo.fQuerySteps;
  JobStepVector newSteps;  // store combined steps
  RowGroup rgOut;          // rowgroup of combined steps
  CalpontSystemCatalog::OID tableOid = tableInfo.fTableOid;

  if (tableOid != CNX_VTABLE_ID)
  {
    // real table
    if (qsv.size() == 0)
    {
      // find a column in FE1, FE2, or FE3
      uint32_t key = -1;

      if (tableInfo.fColsInExp1.size() > 0)
        key = tableInfo.fColsInExp1[0];
      else if (tableInfo.fColsInExp2.size() > 0)
        key = tableInfo.fColsInExp2[0];
      else if (tableInfo.fColsInRetExp.size() > 0)
        key = tableInfo.fColsInRetExp[0];
      else if (tableInfo.fColsInOuter.size() > 0)
        key = tableInfo.fColsInOuter[0];
      else if (tableInfo.fColsInColMap.size() > 0)
        key = tableInfo.fColsInColMap[0];
      else
        throw runtime_error("No query step");

      // construct a pcolscanstep to initialize the tbps
      CalpontSystemCatalog::OID oid = jobInfo.keyInfo->tupleKeyVec[key].fId;
      CalpontSystemCatalog::ColType ct = jobInfo.keyInfo->colType[key];
      map<uint32_t, CalpontSystemCatalog::ColType>::iterator dkMit;

      if (jobInfo.keyInfo->token2DictTypeMap.find(key) != jobInfo.keyInfo->token2DictTypeMap.end())
        ct = jobInfo.keyInfo->token2DictTypeMap[key];

      SJSTEP sjs(new pColScanStep(oid, tableOid, ct, jobInfo));
      sjs->alias(jobInfo.keyInfo->tupleKeyVec[key].fTable);
      sjs->view(jobInfo.keyInfo->tupleKeyVec[key].fView);
      sjs->schema(jobInfo.keyInfo->tupleKeyVec[key].fSchema);
      sjs->name(jobInfo.keyInfo->keyName[key]);
      sjs->tupleId(key);
      qsv.push_back(sjs);
    }

    SJSTEP sjsp;                        // shared_ptr for the new BatchPrimitive
    BatchPrimitive* bps = NULL;         // pscan/pcol/filter/etc combined to
    vector<DictionaryScanInfo> pdsVec;  // pds for string filters
    JobStepVector::iterator begin = qsv.begin();
    JobStepVector::iterator end = qsv.end();
    JobStepVector::iterator it = begin;

    // make sure there is a pcolscan if there is a pcolstep
    while (it != end)
    {
      if (typeid(*(it->get())) == typeid(pColScanStep))
        break;

      if (typeid(*(it->get())) == typeid(pColStep))
      {
        pColStep* pcs = dynamic_cast<pColStep*>(it->get());
        (*it).reset(new pColScanStep(*pcs));
        break;
      }

      it++;
    }

    // ---- predicates ----
    // setup TBPS and dictionaryscan
    it = begin;

    while (it != end)
    {
      if (typeid(*(it->get())) == typeid(pColScanStep))
      {
        if (bps == NULL)
        {
          if (tableOid > 0)
          {
            sjsp.reset(new TupleBPS(*(dynamic_cast<pColScanStep*>(it->get())), jobInfo));
            TupleBPS* tbps = dynamic_cast<TupleBPS*>(sjsp.get());
            tbps->setJobInfo(&jobInfo);
            tbps->setFirstStepType(SCAN);
            bps = tbps;
          }
          else
          {
            sjsp.reset(new CrossEngineStep(mit->second.fSchema, mit->second.fName, mit->second.fPartitions,
                                           mit->second.fAlias, jobInfo));
            bps = dynamic_cast<CrossEngineStep*>(sjsp.get());
          }
        }
        else
        {
          pColScanStep* pcss = dynamic_cast<pColScanStep*>(it->get());
          (*it).reset(new pColStep(*pcss));
        }
      }

      unsigned itInc = 1;               // iterator increase number
      unsigned numOfStepsAddToBps = 0;  // # steps to be added into TBPS

      if ((std::distance(it, end) > 2 && dynamic_cast<pDictionaryScan*>(it->get()) != NULL &&
           (dynamic_cast<pColScanStep*>((it + 1)->get()) != NULL ||
            dynamic_cast<pColStep*>((it + 1)->get()) != NULL) &&
           dynamic_cast<TupleHashJoinStep*>((it + 2)->get()) != NULL) ||
          (std::distance(it, end) > 1 && dynamic_cast<pDictionaryScan*>(it->get()) != NULL &&
           dynamic_cast<TupleHashJoinStep*>((it + 1)->get()) != NULL))
      {
        // string access predicate
        // setup pDictionaryScan
        pDictionaryScan* pds = dynamic_cast<pDictionaryScan*>(it->get());
        vector<uint32_t> pos;
        vector<uint32_t> oids;
        vector<uint32_t> keys;
        vector<uint32_t> scale;
        vector<uint32_t> precision;
        vector<CalpontSystemCatalog::ColDataType> types;
        vector<uint32_t> csNums;
        pos.push_back(2);

        pos.push_back(2 + 8);
        CalpontSystemCatalog::OID coid = jobInfo.keyInfo->dictOidToColOid[pds->oid()];
        oids.push_back(coid);
        uint32_t keyId = pds->tupleId();
        keys.push_back(keyId);
        types.push_back(CalpontSystemCatalog::BIGINT);
        csNums.push_back(pds->colType().charsetNumber);
        scale.push_back(0);
        precision.push_back(0);

        RowGroup rg(oids.size(), pos, oids, keys, types, csNums, scale, precision,
                    jobInfo.stringTableThreshold);

        if (jobInfo.trace)
          cout << "RowGroup pds(and): " << rg.toString() << endl;

        pds->setOutputRowGroup(rg);
        newSteps.push_back(*it);

        DictionaryScanInfo pdsInfo;
        pdsInfo.fTokenId = keyId;
        pdsInfo.fDl = pds->outputAssociation().outAt(0);
        pdsInfo.fRowGroup = rg;
        pdsVec.push_back(pdsInfo);

        // save the token join to the last
        itInc = 1;
        numOfStepsAddToBps = 0;
      }
      else if (std::distance(begin, it) > 1 &&
               (dynamic_cast<pDictionaryScan*>((it - 1)->get()) != NULL ||
                dynamic_cast<pDictionaryScan*>((it - 2)->get()) != NULL) &&
               dynamic_cast<TupleHashJoinStep*>(it->get()) != NULL)
      {
        // save the token join to the last, by pdsInfo
        itInc = 1;
        numOfStepsAddToBps = 0;
      }
      else if (std::distance(it, end) > 2 && dynamic_cast<pColStep*>((it + 1)->get()) != NULL &&
               dynamic_cast<FilterStep*>((it + 2)->get()) != NULL)
      {
        itInc = 3;
        numOfStepsAddToBps = 3;
      }
      else if (std::distance(it, end) > 3 && dynamic_cast<pColStep*>((it + 1)->get()) != NULL &&
               dynamic_cast<pDictionaryStep*>((it + 2)->get()) != NULL &&
               dynamic_cast<FilterStep*>((it + 3)->get()) != NULL)
      {
        itInc = 4;
        numOfStepsAddToBps = 4;
      }
      else if (std::distance(it, end) > 3 && dynamic_cast<pDictionaryStep*>((it + 1)->get()) != NULL &&
               dynamic_cast<pColStep*>((it + 2)->get()) != NULL &&
               dynamic_cast<FilterStep*>((it + 3)->get()) != NULL)
      {
        itInc = 4;
        numOfStepsAddToBps = 4;
      }
      else if (std::distance(it, end) > 4 && dynamic_cast<pDictionaryStep*>((it + 1)->get()) != NULL &&
               dynamic_cast<pColStep*>((it + 2)->get()) != NULL &&
               dynamic_cast<pDictionaryStep*>((it + 3)->get()) != NULL &&
               dynamic_cast<FilterStep*>((it + 4)->get()) != NULL)
      {
        itInc = 5;
        numOfStepsAddToBps = 5;
      }
      else if (std::distance(it, end) > 1 &&
               (dynamic_cast<pColStep*>(it->get()) != NULL ||
                dynamic_cast<pColScanStep*>(it->get()) != NULL) &&
               dynamic_cast<pDictionaryStep*>((it + 1)->get()) != NULL)
      {
        itInc = 2;
        numOfStepsAddToBps = 2;
      }
      else if (dynamic_cast<pColStep*>(it->get()) != NULL)
      {
        pColStep* pcol = dynamic_cast<pColStep*>(it->get());

        if (pcol->getFilters().size() == 0)
        {
          // not an access predicate, pcol for token will be added later if necessary
          numOfStepsAddToBps = 0;
        }
        else
        {
          numOfStepsAddToBps = 1;
        }

        itInc = 1;
      }
      else if (dynamic_cast<pColScanStep*>(it->get()) != NULL)
      {
        numOfStepsAddToBps = 1;
        itInc = 1;
      }
      else
      {
        // Not a combinable step, or step pattern not recognized.
        cerr << boldStart << "Try to combine " << typeid(*(it->get())).name() << ": " << it->get()->oid()
             << " into TBPS" << boldStop << endl;
        return false;
      }

      // now add the steps into the TBPS
      if (numOfStepsAddToBps > 0 && bps == NULL)
        throw runtime_error("BPS not created 1");

      for (unsigned i = 0; i < numOfStepsAddToBps; i++)
      {
        auto pp = (it + i)->get();
        bps->setBPP(pp);
        bps->setStepCount();
        bps->setLastTupleId(pp->tupleId());
      }

      it += itInc;
    }

    // add one-table expression steps to TBPS
    if (tableInfo.fOneTableExpSteps.size() > 0 || tableInfo.fFuncJoinExps.size() > 0)
      addExpresssionStepsToBps(mit, sjsp, jobInfo);

    // add TBPS to the step vector
    newSteps.push_back(sjsp);

    // ---- projects ----
    // now, add the joinkeys to the project step vector
    addProjectStepsToBps(mit, bps, jobInfo);

    // rowgroup has the joinkeys and selected columns
    // this is the expected output of this table
    rgOut = bps->getOutputRowGroup();

    // add token joins
    if (pdsVec.size() > 0)
    {
      // ---- token joins ----
      // construct a TupleHashJoinStep
      TupleBPS* tbps = dynamic_cast<TupleBPS*>(bps);
      TupleHashJoinStep* thjs = new TupleHashJoinStep(jobInfo);
      thjs->tableOid1(0);
      thjs->tableOid2(tableInfo.fTableOid);
      thjs->alias1(tableInfo.fAlias);
      thjs->alias2(tableInfo.fAlias);
      thjs->view1(tableInfo.fView);
      thjs->view2(tableInfo.fView);
      thjs->schema1(tableInfo.fSchema);
      thjs->schema2(tableInfo.fSchema);
      thjs->setLargeSideBPS(tbps);
      thjs->joinId(-1);  // token join is a filter force it done before other joins
      thjs->setJoinType(INNER);
      thjs->tokenJoin(mit->first);
      tbps->incWaitToRunStepCnt();
      SJSTEP spthjs(thjs);

      // rowgroup of the TBPS side
      // start with the expected output of the table, tokens will be appended
      RowGroup rgTbps = rgOut;

      // input jobstepassociation
      // 1.  small sides -- pdictionaryscan steps
      vector<RowGroup> rgPdsVec;
      map<uint32_t, uint32_t> addedCol;
      vector<JoinType> jointypes;
      vector<bool> typeless;
      vector<vector<uint32_t>> smallKeyIndices;
      vector<vector<uint32_t>> largeKeyIndices;
      vector<string> tableNames;
      JobStepAssociation inJsa;

      for (vector<DictionaryScanInfo>::iterator i = pdsVec.begin(); i != pdsVec.end(); i++)
      {
        // add the token steps to the TBPS
        uint32_t tupleKey = i->fTokenId;
        map<uint32_t, uint32_t>::iterator k = addedCol.find(tupleKey);
        unsigned largeSideIndex = rgTbps.getColumnCount();

        if (k == addedCol.end())
        {
          SJSTEP sjs(new pColStep(jobInfo.keyInfo->tupleKeyVec[tupleKey].fId, tableInfo.fTableOid,
                                  jobInfo.keyInfo->token2DictTypeMap[tupleKey], jobInfo));
          sjs->alias(tableInfo.fAlias);
          sjs->view(tableInfo.fView);
          sjs->schema(tableInfo.fSchema);
          sjs->name(jobInfo.keyInfo->keyName[tupleKey]);
          sjs->tupleId(tupleKey);
          bps->setProjectBPP(sjs.get(), NULL);

          // Update info, which will be used to config the hashjoin later.
          rgTbps += i->fRowGroup;
          addedCol[tupleKey] = largeSideIndex;
        }
        else
        {
          largeSideIndex = k->second;
        }

        inJsa.outAdd(i->fDl);
        tableNames.push_back(jobInfo.keyInfo->tupleKeyVec[tupleKey].fTable);
        rgPdsVec.push_back(i->fRowGroup);
        jointypes.push_back(INNER);
        typeless.push_back(false);
        smallKeyIndices.push_back(vector<uint32_t>(1, 0));
        largeKeyIndices.push_back(vector<uint32_t>(1, largeSideIndex));
      }

      // 2. large side
      if (jobInfo.trace)
        cout << "RowGroup bps(and): " << rgTbps.toString() << endl;

      bps->setOutputRowGroup(rgTbps);
      inJsa.outAdd(bps->outputAssociation().outAt(0));

      // set input jobstepassociation
      thjs->inputAssociation(inJsa);
      thjs->setLargeSideDLIndex(inJsa.outSize() - 1);

      // output jobstepassociation
      AnyDataListSPtr spdl(new AnyDataList());
      RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
      spdl->rowGroupDL(dl);
      dl->OID(mit->first);
      JobStepAssociation jsaOut;
      jsaOut.outAdd(spdl);
      thjs->outputAssociation(jsaOut);

      // config the tuplehashjoin
      thjs->configSmallSideRG(rgPdsVec, tableNames);
      thjs->configLargeSideRG(rgTbps);
      thjs->configJoinKeyIndex(jointypes, typeless, smallKeyIndices, largeKeyIndices);
      thjs->setOutputRowGroup(rgOut);
      newSteps.push_back(spthjs);
    }
  }
  else
  {
    // table derived from subquery
    SubQueryStep* subStep = NULL;
    SubAdapterStep* adaStep = NULL;

    for (JobStepVector::iterator it = qsv.begin(); it != qsv.end(); it++)
    {
      if (((subStep = dynamic_cast<SubQueryStep*>(it->get())) != NULL) ||
          ((adaStep = dynamic_cast<SubAdapterStep*>(it->get())) != NULL))
        newSteps.push_back(*it);
    }

    if (subStep == NULL && adaStep == NULL)
      throw runtime_error("No step for subquery.");

    if (subStep)
    {
      rgOut = subStep->getOutputRowGroup();
    }
    else
    {
      // add one-table expression steps to the adapter
      if (tableInfo.fOneTableExpSteps.size() > 0)
        adaStep->addExpression(tableInfo.fOneTableExpSteps, jobInfo);

      // add function join steps
      if (tableInfo.fFuncJoinExps.size() > 0)
      {
        // fe rowgroup info
        RowGroup feRg = adaStep->getFeRowGroup();

        if (feRg.getColumnCount() == 0)
          feRg = adaStep->getOutputRowGroup();

        const vector<uint32_t>& feKeys = feRg.getKeys();
        map<uint32_t, uint32_t> keyToIndexMapFe;

        for (uint64_t i = 0; i < feKeys.size(); ++i)
          keyToIndexMapFe.insert(make_pair(feKeys[i], i));

        // output rowgroup info
        const RowGroup& outRg = adaStep->getOutputRowGroup();
        const vector<uint32_t>& outKeys = outRg.getKeys();
        map<uint32_t, uint32_t> keyToIndexMapOut;

        for (uint64_t i = 0; i < outKeys.size(); ++i)
          keyToIndexMapOut.insert(make_pair(outKeys[i], i));

        // make sure the function join columns are present in the rgs
        vector<uint32_t> fjKeys;
        vector<SRCP> fjCols;
        TupleInfoVector tis;
        uint64_t lastFeIdx = feKeys.size();
        JobStepVector& fjs = tableInfo.fFuncJoinExps;

        for (JobStepVector::iterator it = fjs.begin(); it != fjs.end(); it++)
        {
          ExpressionStep* e = dynamic_cast<ExpressionStep*>(it->get());
          TupleInfo ti = setExpTupleInfo(e->expression().get(), jobInfo);

          if (find(feKeys.begin(), feKeys.end(), ti.key) == feKeys.end())
          {
            tis.push_back(ti);
            keyToIndexMapFe.insert(make_pair(ti.key, lastFeIdx++));
          }

          e->updateInputIndex(keyToIndexMapFe, jobInfo);
          e->updateOutputIndex(keyToIndexMapFe, jobInfo);
          fjCols.push_back(e->expression());
        }

        // additional fields in the rowgroup
        vector<uint32_t> pos;
        vector<uint32_t> oids;
        vector<uint32_t> keys;
        vector<uint32_t> scale;
        vector<uint32_t> precision;
        vector<CalpontSystemCatalog::ColDataType> types;
        vector<uint32_t> csNums;
        pos.push_back(2);

        for (unsigned i = 0; i < tis.size(); i++)
        {
          pos.push_back(pos.back() + tis[i].width);
          oids.push_back(tis[i].oid);
          keys.push_back(tis[i].key);
          types.push_back(tis[i].dtype);
          csNums.push_back(tis[i].csNum);
          scale.push_back(tis[i].scale);
          precision.push_back(tis[i].precision);
        }

        RowGroup addRg(oids.size(), pos, oids, keys, types, csNums, scale, precision,
                       jobInfo.stringTableThreshold);

        RowGroup feRg1 = feRg;
        RowGroup outRg1 = outRg;

        if (addRg.getColumnCount() > 0)
        {
          feRg1 += addRg;
          outRg1 += addRg;
        }

        adaStep->addFcnJoinExp(fjCols);
        adaStep->setFeRowGroup(feRg1);
        adaStep->setOutputRowGroup(outRg1);
      }

      rgOut = adaStep->getOutputRowGroup();
    }
  }

  tableInfo.fDl = newSteps.back()->outputAssociation().outAt(0);
  tableInfo.fRowGroup = rgOut;

  if (jobInfo.trace)
    cout << "RowGroup for " << mit->first << " : " << mit->second.fRowGroup.toString() << endl;

  qsv.swap(newSteps);

  return true;
}

bool addFunctionJoin(vector<uint32_t>& joinedTables, JobStepVector& joinSteps, set<uint32_t>& nodeSet,
                     set<pair<uint32_t, uint32_t>>& pathSet, TableInfoMap& tableInfoMap, JobInfo& jobInfo)
{
  // @bug3683, adding function joins for not joined tables, one pair at a time.
  // design review comment:
  //     all convertable expressions between a pair of tables should be done all, or none.
  vector<JobStep*>::iterator i = jobInfo.functionJoins.begin();  // candidates
  set<pair<uint32_t, uint32_t>> functionJoinPairs;               // pairs
  bool added = false;                                            // new node added

  // for function join tables' scope checking, not to try semi join inside subquery.
  set<uint32_t> tables;  // tables to join
  tables.insert(jobInfo.tableList.begin(), jobInfo.tableList.end());

  // table pairs to be joined by function joins
  TableJoinMap::iterator m1 = jobInfo.tableJoinMap.end();
  TableJoinMap::iterator m2 = jobInfo.tableJoinMap.end();

  for (; (i != jobInfo.functionJoins.end()); i++)
  {
    ExpressionStep* es = dynamic_cast<ExpressionStep*>((*i));
    idbassert(es);

    if (es->functionJoin())
      continue;  // already converted to a join

    boost::shared_ptr<FunctionJoinInfo> fji = es->functionJoinInfo();
    uint32_t key1 = fji->fJoinKey[0];
    uint32_t key2 = fji->fJoinKey[1];
    uint32_t tid1 = fji->fTableKey[0];
    uint32_t tid2 = fji->fTableKey[1];

    if (nodeSet.find(tid1) != nodeSet.end() && nodeSet.find(tid2) != nodeSet.end())
      continue;  // both connected, will be a cycle if added.

    if (nodeSet.find(tid1) == nodeSet.end() && nodeSet.find(tid2) == nodeSet.end())
      continue;  // both isolated, wait until one is connected.

    if (tables.find(tid1) == tables.end() || tables.find(tid2) == tables.end())
      continue;  // sub-query case

    // one & only one is already connected
    pair<uint32_t, uint32_t> p(tid1, tid2);

    if (functionJoinPairs.empty())
    {
      functionJoinPairs.insert(make_pair(tid1, tid2));
      functionJoinPairs.insert(make_pair(tid2, tid1));
      tableInfoMap[tid1].fAdjacentList.push_back(tid2);
      tableInfoMap[tid2].fAdjacentList.push_back(tid1);

      if (find(joinedTables.begin(), joinedTables.end(), tid1) == joinedTables.end())
      {
        joinedTables.push_back(tid1);
        nodeSet.insert(tid1);
        pathSet.insert(make_pair(tid2, tid1));
      }
      else
      {
        joinedTables.push_back(tid2);
        nodeSet.insert(tid2);
        pathSet.insert(make_pair(tid1, tid2));
      }

      added = true;

      m1 = jobInfo.tableJoinMap.insert(m1, make_pair(make_pair(tid1, tid2), JoinData()));
      m2 = jobInfo.tableJoinMap.insert(m1, make_pair(make_pair(tid2, tid1), JoinData()));

      if (m1 == jobInfo.tableJoinMap.end() || m2 == jobInfo.tableJoinMap.end())
        throw runtime_error("Bad table map.");

      TupleInfo ti1 = getTupleInfo(key1, jobInfo);
      TupleInfo ti2 = getTupleInfo(key2, jobInfo);

      // Enable Typeless JOIN for char and wide decimal types.
      if (datatypes::isCharType(ti1.dtype) || (datatypes::isWideDecimalType(ti1.dtype, ti1.width) ||
                                               datatypes::isWideDecimalType(ti2.dtype, ti2.width)))
        m1->second.fTypeless = m2->second.fTypeless = true;  // ti2 is compatible
      else
        m1->second.fTypeless = m2->second.fTypeless = false;
    }
    else if (functionJoinPairs.find(p) == functionJoinPairs.end())
    {
      continue;  // different path
    }
    else
    {
      // path already added, do compound join
      m1->second.fTypeless = m2->second.fTypeless = true;
    }

    // simple or compound function join
    es->functionJoin(true);
    updateTableKey(key1, tid1, jobInfo);
    updateTableKey(key2, tid2, jobInfo);

    tableInfoMap[tid1].fJoinKeys.push_back(key1);
    tableInfoMap[tid2].fJoinKeys.push_back(key2);

    if (fji->fStep[0])
      tableInfoMap[tid1].fFuncJoinExps.push_back(fji->fStep[0]);

    if (fji->fStep[1])
      tableInfoMap[tid2].fFuncJoinExps.push_back(fji->fStep[1]);

    vector<uint32_t>& cols1 = tableInfoMap[tid1].fColsInFuncJoin;
    cols1.insert(cols1.end(), fji->fColumnKeys[0].begin(), fji->fColumnKeys[0].end());
    vector<uint32_t>& cols2 = tableInfoMap[tid2].fColsInFuncJoin;
    cols2.insert(cols2.end(), fji->fColumnKeys[1].begin(), fji->fColumnKeys[1].end());

    // construct a join step
    TupleHashJoinStep* thjs = new TupleHashJoinStep(jobInfo);
    thjs->tableOid1(fji->fTableOid[0]);
    thjs->tableOid2(fji->fTableOid[1]);
    thjs->oid1(fji->fOid[0]);
    thjs->oid2(fji->fOid[1]);
    thjs->alias1(fji->fAlias[0]);
    thjs->alias2(fji->fAlias[1]);
    thjs->view1(fji->fView[0]);
    thjs->view2(fji->fView[1]);
    thjs->schema1(fji->fSchema[0]);
    thjs->schema2(fji->fSchema[1]);
    thjs->column1(fji->fExpression[0]);
    thjs->column2(fji->fExpression[1]);
    thjs->sequence1(fji->fSequence[0]);
    thjs->sequence2(fji->fSequence[1]);
    thjs->joinId(fji->fJoinId);
    thjs->setJoinType(fji->fJoinType);
    thjs->funcJoinInfo(fji);
    thjs->tupleId1(key1);
    thjs->tupleId2(key2);
    SJSTEP spjs(thjs);

    // check correlated info
    JoinType joinType = fji->fJoinType;

    if (!(joinType & CORRELATED))
    {
      joinSteps.push_back(spjs);

      // Keep a map of the join (table, key) pairs
      m1->second.fLeftKeys.push_back(key1);
      m1->second.fRightKeys.push_back(key2);

      m2->second.fLeftKeys.push_back(key2);
      m2->second.fRightKeys.push_back(key1);

      // Keep a map of the join type between the keys.
      // OUTER join and SEMI/ANTI join are mutually exclusive.
      if (joinType == LEFTOUTER)
      {
        m1->second.fTypes.push_back(SMALLOUTER);
        m2->second.fTypes.push_back(LARGEOUTER);
        jobInfo.outerOnTable.insert(tid2);
      }
      else if (joinType == RIGHTOUTER)
      {
        m1->second.fTypes.push_back(LARGEOUTER);
        m2->second.fTypes.push_back(SMALLOUTER);
        jobInfo.outerOnTable.insert(tid1);
      }
      else if ((joinType & SEMI) &&
               ((joinType & LEFTOUTER) == LEFTOUTER || (joinType & RIGHTOUTER) == RIGHTOUTER))
      {
        // @bug3998, DML UPDATE borrows "SEMI" flag,
        // allowing SEMI and LARGEOUTER combination to support update with outer join.
        if ((joinType & LEFTOUTER) == LEFTOUTER)
        {
          joinType ^= LEFTOUTER;
          m1->second.fTypes.push_back(joinType);
          m2->second.fTypes.push_back(joinType | LARGEOUTER);
          jobInfo.outerOnTable.insert(tid2);
        }
        else
        {
          joinType ^= RIGHTOUTER;
          m1->second.fTypes.push_back(joinType | LARGEOUTER);
          m2->second.fTypes.push_back(joinType);
          jobInfo.outerOnTable.insert(tid1);
        }
      }
      else
      {
        m1->second.fTypes.push_back(joinType);
        m2->second.fTypes.push_back(joinType);

        if (joinType == INNER)
        {
          jobInfo.innerOnTable.insert(tid1);
          jobInfo.innerOnTable.insert(tid2);
        }
      }

      // need id to keep the join order
      m1->second.fJoinId = m2->second.fJoinId = thjs->joinId();
    }
    else
    {
      // one of the tables is in outer query
      jobInfo.correlateSteps.push_back(spjs);
    }
  }

  return added;
}

// This class represents a circular inner join graph transformer.
// It collects a cycles in the given join graph as disjoint set of the join edges, for each collected cycle
// removes the join edge (if column statistics available it tries to remove join edge which could produce
// highest intermediate join result, otherwise just a random edge in a cycle) and adds that join edge to
// jobinfo class be restored as `post join` equal fiter in a join ordering part.
class CircularJoinGraphTransformer
{
 public:
  // Ctor.
  CircularJoinGraphTransformer(TableInfoMap& infoMap, JobInfo& jobInfo, JobStepVector& joinSteps)
   : infoMap(infoMap), jobInfo(jobInfo), joinSteps(joinSteps)
  {
  }
  // Delete all other ctrs/dctrs.
  CircularJoinGraphTransformer() = delete;
  CircularJoinGraphTransformer(const CircularJoinGraphTransformer&) = delete;
  CircularJoinGraphTransformer(CircularJoinGraphTransformer&&) = delete;
  CircularJoinGraphTransformer& operator=(const CircularJoinGraphTransformer&) = delete;
  CircularJoinGraphTransformer& operator=(CircularJoinGraphTransformer&&) = delete;
  virtual ~CircularJoinGraphTransformer() = default;

  // Transform join graph.
  void transformJoinGraph();

 protected:
  // Analyzes the `join graph` based on DFS algorithm.
  virtual void analyzeJoinGraph(uint32_t currentTable, uint32_t prevTable);
  // For each cycle breaks it and collects join edges.
  void breakCyclesAndCollectJoinEdges();
  // Removes given `join edge` from the `join graph`.
  void breakCycleAndCollectJoinEdge(const std::pair<JoinEdge, int64_t>& edgeForward);
  // Initializes the `join graph` based on the table connections.
  virtual void initializeJoinGraph();
  // Check if the given join edge has FK - FK relations.
  bool isForeignKeyForeignKeyLink(const JoinEdge& edge, statistics::StatisticsManager* statisticsManager);
  // Based on column statistics tries to search `join edge` with maximum join cardinality.
  virtual void chooseEdgeToTransform(Cycle& cycle, std::pair<JoinEdge, int64_t>& resultEdge);
  // Removes given `tableId` from adjacent list.
  void removeFromAdjacentList(uint32_t tableId, std::vector<uint32_t>& adjList);
  // Removes associated join step associated with the given `joinEdge` from job steps.
  void removeAssociatedHashJoinStepFromJoinSteps(const JoinEdge& joinEdge);

  // Join information.
  TableInfoMap& infoMap;
  JobInfo& jobInfo;
  JobStepVector& joinSteps;

  // Represents a collection of cycles.
  Cycles cycles;
  // Represents internal `join graph.`
  JoinGraph joinGraph;
  // Represents a set of join edges to erase for each cycle in `join graph`.
  JoinEdges edgesToTransform;
  // Represents a table to start analysis.
  uint32_t headTable{0};
};

// Circular inner joins methods.
void CircularJoinGraphTransformer::analyzeJoinGraph(uint32_t currentTable, uint32_t prevTable)
{
  // Mark as `GREY` to specify processing table node.
  joinGraph[currentTable].fTableColor = JoinTableColor::GREY;
  joinGraph[currentTable].fParent = prevTable;

  // For each adjacent node.
  for (const auto adjNode : joinGraph[currentTable].fAdjacentList)
  {
    if (prevTable != adjNode)
    {
      if (joinGraph[adjNode].fTableColor == JoinTableColor::GREY)
      {
        Cycle cycle;
        const auto edgeForward = make_pair(currentTable, adjNode);
        const auto edgeBackward = make_pair(adjNode, currentTable);

        if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
        {
          edgesToTransform.insert(edgeForward);
          cycle.push_back(edgeForward);
        }

        auto nodeIt = currentTable;
        auto nextNode = joinGraph[nodeIt].fParent;
        // Walk back until we find node `adjNode` we identified before.
        while (nextNode != std::numeric_limits<uint32_t>::max() && nextNode != adjNode)
        {
          const auto edgeForward = make_pair(nextNode, nodeIt);
          const auto edgeBackward = make_pair(nodeIt, nextNode);

          if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
          {
            edgesToTransform.insert(edgeForward);
            cycle.push_back(edgeForward);
          }

          nodeIt = nextNode;
          nextNode = joinGraph[nodeIt].fParent;
        }

        // Add the last edge.
        if (nextNode != std::numeric_limits<uint32_t>::max())
        {
          const auto edgeForward = make_pair(nextNode, nodeIt);
          const auto edgeBackward = make_pair(nodeIt, nextNode);

          if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
          {
            edgesToTransform.insert(edgeForward);
            cycle.push_back(edgeForward);
          }
        }

        if (jobInfo.trace && cycle.size())
        {
          std::cout << "Cycle found.\n";
          std::cout << "Collected cycle \n";
          for (const auto& edge : cycle)
            std::cout << "Join edge: " << edge.first << " <-> " << edge.second << '\n';
        }

        // Collect the cycle.
        if (cycle.size())
          cycles.push_back(std::move(cycle));
      }
      // If not visited - go there.
      else if (joinGraph[adjNode].fTableColor == JoinTableColor::WHITE)
      {
        analyzeJoinGraph(adjNode, currentTable);
      }
    }
  }

  // Mark `BLACK` to specify this node is finished.
  joinGraph[currentTable].fTableColor = JoinTableColor::BLACK;
}

void CircularJoinGraphTransformer::removeFromAdjacentList(uint32_t tableId, std::vector<uint32_t>& adjList)
{
  auto tableIdIt = std::find(adjList.begin(), adjList.end(), tableId);
  if (tableIdIt != adjList.end())
    adjList.erase(tableIdIt);
}

bool CircularJoinGraphTransformer::isForeignKeyForeignKeyLink(
    const JoinEdge& edge, statistics::StatisticsManager* statisticsManager)
{
  const auto end = jobInfo.tableJoinMap.end();
  auto it = jobInfo.tableJoinMap.find(edge);
  if (it == end)
  {
    it = jobInfo.tableJoinMap.find(make_pair(edge.second, edge.first));
    if (it == end)
      return false;
  }

  std::vector<statistics::KeyType> leftKeys, rightKeys;
  std::vector<uint32_t> lOid, rOid;

  for (auto key : it->second.fLeftKeys)
  {
    auto oid = jobInfo.keyInfo->tupleKeyVec[key].fId;
    if (!statisticsManager->hasKey(oid))
      return false;

    auto keyType = statisticsManager->getKeyType(oid);
    leftKeys.push_back(keyType);
    lOid.push_back(oid);

    if (jobInfo.trace)
      std::cout << "OID " << oid << " with key type " << (uint32_t)keyType << std::endl;
  }

  for (auto key : it->second.fRightKeys)
  {
    auto oid = jobInfo.keyInfo->tupleKeyVec[key].fId;
    if (!statisticsManager->hasKey(oid))
      return false;

    auto keyType = statisticsManager->getKeyType(oid);
    rightKeys.push_back(keyType);
    rOid.push_back(oid);

    if (jobInfo.trace)
      std::cout << "OID " << oid << " with key type " << (uint32_t)keyType << std::endl;
  }

  if (rightKeys.size() == 0 || leftKeys.size() == 0)
    return false;

  statistics::KeyType leftType = statistics::KeyType::PK;
  for (auto keyType : leftKeys)
  {
    if (keyType == statistics::KeyType::FK)
    {
      leftType = keyType;
      break;
    }
  }

  statistics::KeyType rightType = statistics::KeyType::PK;
  for (auto keyType : rightKeys)
  {
    if (keyType == statistics::KeyType::FK)
    {
      rightType = keyType;
      break;
    }
  }

  if (rightType == statistics::KeyType::FK && leftType == statistics::KeyType::FK)
  {
    if (jobInfo.trace)
    {
      std::cout << "Found FK <-> FK connection " << lOid.front() << " <-> " << rOid.front() << std::endl;
    }
    return true;
  }

  return false;
}

void CircularJoinGraphTransformer::chooseEdgeToTransform(Cycle& cycle,
                                                         std::pair<JoinEdge, int64_t>& resultEdge)
{
  // Use statistics if possible.
  auto* statisticsManager = statistics::StatisticsManager::instance();
  for (auto& edgeForward : cycle)
  {
    // Check that `join edge` is aligned with our needs.
    if (isForeignKeyForeignKeyLink(edgeForward, statisticsManager))
    {
      const auto edgeBackward = std::make_pair(edgeForward.second, edgeForward.first);
      if (!jobInfo.joinEdgesToRestore.count(edgeForward) && !jobInfo.joinEdgesToRestore.count(edgeBackward))
      {
        resultEdge = std::make_pair(edgeForward, 0 /*Dummy weight*/);
        return;
      }
    }
  }

  if (jobInfo.trace)
    std::cout << "FK FK key not found, removing the last one inner join edge" << std::endl;

  // Take just a last one.
  resultEdge = std::make_pair(cycle.back(), 0 /*Dummy weight*/);
}

void CircularJoinGraphTransformer::removeAssociatedHashJoinStepFromJoinSteps(const JoinEdge& joinEdge)
{
  if (jobInfo.trace)
  {
    std::cout << "Join steps before transformation: " << std::endl;
    for (auto joinStepIt = joinSteps.begin(); joinStepIt < joinSteps.end(); joinStepIt++)

    {
      auto* tupleHashJoinStep = dynamic_cast<TupleHashJoinStep*>(joinStepIt->get());
      if (tupleHashJoinStep)
      {
        std::cout << "Tables for hash join: " << getTableKey(jobInfo, tupleHashJoinStep->tupleId1())
                  << " <-> " << getTableKey(jobInfo, tupleHashJoinStep->tupleId2()) << std::endl;
      }
    }
  }

  // Match the given `join edge` in `join steps` vector.
  auto end = joinSteps.end();
  auto joinStepIt = joinSteps.begin();
  // We have to remove all `TupleHashJoinSteps` with the given table keys from join steps.
  while (joinStepIt != end)
  {
    auto* tupleHashJoinStep = dynamic_cast<TupleHashJoinStep*>(joinStepIt->get());
    if (tupleHashJoinStep)
    {
      const auto tableKey1 = getTableKey(jobInfo, tupleHashJoinStep->tupleId1());
      const auto tableKey2 = getTableKey(jobInfo, tupleHashJoinStep->tupleId2());

      if ((tableKey1 == joinEdge.first && tableKey2 == joinEdge.second) ||
          (tableKey1 == joinEdge.second && tableKey2 == joinEdge.first))
      {
        if (jobInfo.trace)
          std::cout << "Erase matched join step with keys: " << tableKey1 << " <-> " << tableKey2
                    << std::endl;

        joinStepIt = joinSteps.erase(joinStepIt);
        end = joinSteps.end();
      }
      else
      {
        ++joinStepIt;
      }
    }
  }

  if (jobInfo.trace)
  {
    std::cout << "Join steps after transformation:  " << std::endl;
    for (auto joinStepIt = joinSteps.begin(); joinStepIt < joinSteps.end(); joinStepIt++)

    {
      auto* tupleHashJoinStep = dynamic_cast<TupleHashJoinStep*>(joinStepIt->get());
      if (tupleHashJoinStep)
      {
        std::cout << "Tables for hash join: " << getTableKey(jobInfo, tupleHashJoinStep->tupleId1())
                  << " <-> " << getTableKey(jobInfo, tupleHashJoinStep->tupleId2()) << std::endl;
      }
    }
  }
}

void CircularJoinGraphTransformer::breakCycleAndCollectJoinEdge(
    const std::pair<JoinEdge, int64_t>& joinEdgeWithWeight)
{
  // Add edge to be restored.
  jobInfo.joinEdgesToRestore.insert({joinEdgeWithWeight.first, joinEdgeWithWeight.second});
  const auto edgeForward = joinEdgeWithWeight.first;

  // Keep key columns in result rowgroups, to avoid columns elimination at the intermediate joins.
  auto tableInfoIt = jobInfo.tableJoinMap.find(edgeForward);
  auto& firstExp2 = infoMap[edgeForward.first].fColsInExp2;
  firstExp2.insert(firstExp2.end(), tableInfoIt->second.fLeftKeys.begin(),
                   tableInfoIt->second.fLeftKeys.end());
  auto& secondExp2 = infoMap[edgeForward.second].fColsInExp2;
  secondExp2.insert(secondExp2.end(), tableInfoIt->second.fRightKeys.begin(),
                    tableInfoIt->second.fRightKeys.end());

  // The edge is choosen on the previous step, we have to remove it from `adjacent list`.
  removeFromAdjacentList(edgeForward.first, infoMap[edgeForward.second].fAdjacentList);
  removeFromAdjacentList(edgeForward.second, infoMap[edgeForward.first].fAdjacentList);

  if (jobInfo.trace)
    std::cout << "Remove from cycle join edge: " << edgeForward.first << " <-> " << edgeForward.second
              << std::endl;

  // Remove all associated `TupleHashJoinSteps` from join steps.
  removeAssociatedHashJoinStepFromJoinSteps(edgeForward);
}

void CircularJoinGraphTransformer::breakCyclesAndCollectJoinEdges()
{
  if (jobInfo.trace)
    std::cout << "Collected cycles size: " << cycles.size() << std::endl;

  // For each cycle.
  for (auto& cycle : cycles)
  {
    std::pair<JoinEdge, int64_t> joinEdgeWithWeight;
    chooseEdgeToTransform(cycle, joinEdgeWithWeight);
    breakCycleAndCollectJoinEdge(joinEdgeWithWeight);
  }
}

void CircularJoinGraphTransformer::initializeJoinGraph()
{
  for (const auto& infoPair : infoMap)
  {
    JoinTableNode joinTableNode;
    // Copy adjacent list.
    joinTableNode.fAdjacentList = infoPair.second.fAdjacentList;
    joinGraph[infoPair.first] = joinTableNode;
  }

  // For inner join we can choose any table to be a head.
  headTable = joinGraph.begin()->first;
}

void CircularJoinGraphTransformer::transformJoinGraph()
{
  initializeJoinGraph();
  analyzeJoinGraph(/*currentTable=*/headTable, /*prevTable=*/std::numeric_limits<uint32_t>::max());
  edgesToTransform.clear();
  breakCyclesAndCollectJoinEdges();
}

// This class represents circular outer join graph transformer.
// It defines a weight for the particular edge in join graph from the prioriy of the joins defined by
// the user.
// In general lets a assume we have a cycle t1 -> t2 -> ... ti ... tn -> t1 (where n is natural number >= 3),
// the weighted graph will have 2 edges with maximum weights among other - (tn -> t1) and (tn - 1 -> tn)
// those are candidates for transformation.
// Adds a table with associated `join edge` to be on a large side.
class CircularOuterJoinGraphTransformer : public CircularJoinGraphTransformer
{
 public:
  // Ctor.
  CircularOuterJoinGraphTransformer(TableInfoMap& infoMap, JobInfo& jobInfo, JobStepVector& joinSteps)
   : CircularJoinGraphTransformer(infoMap, jobInfo, joinSteps)
  {
  }
  // Delete all other ctrs/dcts.
  CircularOuterJoinGraphTransformer() = delete;
  CircularOuterJoinGraphTransformer(const CircularOuterJoinGraphTransformer&) = delete;
  CircularOuterJoinGraphTransformer(CircularOuterJoinGraphTransformer&&) = delete;
  CircularOuterJoinGraphTransformer& operator=(const CircularOuterJoinGraphTransformer&) = delete;
  CircularOuterJoinGraphTransformer& operator=(CircularOuterJoinGraphTransformer&&) = delete;
  ~CircularOuterJoinGraphTransformer() override = default;

 private:
  // Analyzes the given `join graph`.
  void analyzeJoinGraph(uint32_t currentTable, uint32_t prevTable) override;
  // Chooses a join edge to transform from the given cycle based on the join edge weight,
  // the join edge for transformation has a maximum weight in a cycle.
  void chooseEdgeToTransform(Cycle& cycle, std::pair<JoinEdge, int64_t>& resultEdge) override;
  // Returns the min weight among all join weights related to the given `headTable`.
  int64_t getSublingsMinWeight(uint32_t headTable, uint32_t associatedTable);
  // Returns the max weight which is less than given `upperBoundWeight` among all join weights related to
  // the given `headTable`.
  int64_t getSublingsMaxWeightLessThan(uint32_t headTable, uint32_t associatedTable,
                                       int64_t upperBoundWeight);
  // Initializes `join graph` from the table connections.
  void initializeJoinGraph() override;

  // The map which represents a weight for each join edge in join graph.
  std::map<JoinEdge, int64_t> joinEdgesToWeights;
};

int64_t CircularOuterJoinGraphTransformer::getSublingsMinWeight(uint32_t headTable, uint32_t associatedTable)
{
  int64_t minWeight = std::numeric_limits<int64_t>::max();
  for (const auto adjNode : joinGraph[headTable].fAdjacentList)
  {
    if (adjNode != associatedTable)
    {
      JoinEdge joinEdge(adjNode, headTable);
      minWeight = std::min(joinEdgesToWeights[joinEdge], minWeight);
    }
  }
  return minWeight;
}

int64_t CircularOuterJoinGraphTransformer::getSublingsMaxWeightLessThan(uint32_t headTable,
                                                                        uint32_t associatedTable,
                                                                        int64_t upperBoundWeight)
{
  int64_t maxWeight = std::numeric_limits<int64_t>::min();
  for (const auto adjNode : joinGraph[headTable].fAdjacentList)
  {
    if (adjNode != associatedTable)
    {
      JoinEdge joinEdge(adjNode, headTable);
      const auto currentWeight = joinEdgesToWeights[joinEdge];
      if (currentWeight < upperBoundWeight)
        maxWeight = std::max(currentWeight, maxWeight);
    }
  }
  return maxWeight;
}

void CircularOuterJoinGraphTransformer::initializeJoinGraph()
{
  // Initialize a join graph at first.
  CircularJoinGraphTransformer::initializeJoinGraph();

  // Associate join weights.
  if (jobInfo.trace)
    std::cout << "Join edges with weights.\n";

  int64_t minWeightFullGraph = std::numeric_limits<int64_t>::max();
  JoinEdge joinEdgeWithMinWeight(0, 0);

  // For each join step we associate a `join id` with `join edge`.
  for (auto joinStepIt = joinSteps.begin(); joinStepIt < joinSteps.end(); joinStepIt++)
  {
    auto* tupleHashJoinStep = dynamic_cast<TupleHashJoinStep*>(joinStepIt->get());
    if (tupleHashJoinStep)
    {
      const int64_t weight = tupleHashJoinStep->joinId();
      const auto tableKey1 = getTableKey(jobInfo, tupleHashJoinStep->tupleId1());
      const auto tableKey2 = getTableKey(jobInfo, tupleHashJoinStep->tupleId2());

      // Edge forward.
      JoinEdge edgeForward{tableKey1, tableKey2};
      auto joinEdgeWeightIt = joinEdgesToWeights.find(edgeForward);
      if (joinEdgeWeightIt == joinEdgesToWeights.end())
        joinEdgesToWeights.insert({edgeForward, weight});
      else
        joinEdgeWeightIt->second = std::max(weight, joinEdgeWeightIt->second);

      // Edge backward.
      JoinEdge edgeBackward{tableKey2, tableKey1};
      joinEdgeWeightIt = joinEdgesToWeights.find(edgeBackward);
      if (joinEdgeWeightIt == joinEdgesToWeights.end())
        joinEdgesToWeights.insert({edgeBackward, weight});
      else
        joinEdgeWeightIt->second = std::max(weight, joinEdgeWeightIt->second);

      if (minWeightFullGraph > weight)
      {
        minWeightFullGraph = weight;
        joinEdgeWithMinWeight = edgeForward;
      }

      if (jobInfo.trace)
        std::cout << edgeForward.first << " <-> " << edgeForward.second << " : " << weight << std::endl;
    }
  }

  if (jobInfo.trace)
    std::cout << "Minimum weight edge is: " << joinEdgeWithMinWeight.first << " <-> "
              << joinEdgeWithMinWeight.second << std::endl;

  // Search for `head table` by the given join edge, we have 2 candidates.
  // The head table is opposite to the table which has a join edge with minimum weight among all edges related
  // to that table.
  if (getSublingsMinWeight(joinEdgeWithMinWeight.first, joinEdgeWithMinWeight.second) >
      getSublingsMinWeight(joinEdgeWithMinWeight.second, joinEdgeWithMinWeight.first))
    headTable = joinEdgeWithMinWeight.first;
  else
    headTable = joinEdgeWithMinWeight.second;

  if (jobInfo.trace)
    std::cout << "Head table is: " << headTable << std::endl;
}

void CircularOuterJoinGraphTransformer::analyzeJoinGraph(uint32_t currentTable, uint32_t prevTable)
{
  joinGraph[currentTable].fTableColor = JoinTableColor::GREY;
  joinGraph[currentTable].fParent = prevTable;

  std::vector<std::pair<uint32_t, int64_t>> adjacentListWeighted;
  // For each adjacent node.
  for (const auto adjNode : joinGraph[currentTable].fAdjacentList)
  {
    if (prevTable != adjNode)
    {
      const JoinEdge joinEdge{currentTable, adjNode};
      const auto weight = joinEdgesToWeights[joinEdge];
      adjacentListWeighted.push_back({adjNode, weight});
    }
  }

  // Sort vertices by weights.
  std::sort(adjacentListWeighted.begin(), adjacentListWeighted.end(),
            [](const std::pair<uint32_t, int64_t>& a, const std::pair<uint32_t, int64_t>& b)
            { return a.second < b.second; });

  // For each weighted adjacent node.
  for (const auto& adjNodeWeighted : adjacentListWeighted)
  {
    const auto adjNode = adjNodeWeighted.first;
    // If visited and not a back edge consider as a cycle.
    if (joinGraph[adjNode].fTableColor == JoinTableColor::GREY)
    {
      Cycle cycle;
      const auto edgeForward = make_pair(currentTable, adjNode);
      const auto edgeBackward = make_pair(adjNode, currentTable);

      if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
      {
        edgesToTransform.insert(edgeForward);
        cycle.push_back(edgeForward);
      }

      auto nodeIt = currentTable;
      auto nextNode = joinGraph[nodeIt].fParent;
      // Walk back until we find node `adjNode` we identified before.
      while (nextNode != std::numeric_limits<uint32_t>::max() && nextNode != adjNode)
      {
        const auto edgeForward = make_pair(nextNode, nodeIt);
        const auto edgeBackward = make_pair(nodeIt, nextNode);

        if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
        {
          edgesToTransform.insert(edgeForward);
          cycle.push_back(edgeForward);
        }

        nodeIt = nextNode;
        nextNode = joinGraph[nodeIt].fParent;
      }

      // Add the last edge.
      if (nextNode != std::numeric_limits<uint32_t>::max())
      {
        const auto edgeForward = make_pair(nextNode, nodeIt);
        const auto edgeBackward = make_pair(nodeIt, nextNode);

        if (!edgesToTransform.count(edgeForward) && !edgesToTransform.count(edgeBackward))
        {
          edgesToTransform.insert(edgeForward);
          cycle.push_back(edgeForward);
        }
      }

      // Collect the cycle.
      if (cycle.size())
        cycles.push_back(std::move(cycle));
    }
    else if (joinGraph[adjNode].fTableColor == JoinTableColor::WHITE)
    {
      analyzeJoinGraph(adjNode, currentTable);
    }
  }

  joinGraph[currentTable].fTableColor = JoinTableColor::BLACK;
}

void CircularOuterJoinGraphTransformer::chooseEdgeToTransform(Cycle& cycle,
                                                              std::pair<JoinEdge, int64_t>& resultEdge)
{
  int64_t maxWeightInCycle = std::numeric_limits<int64_t>::min();
  JoinEdge joinEdgeWithMaxWeight;

  if (jobInfo.trace)
    std::cout << "Collected cycle:\n";

  // Search for a join edge with max weight in a given cycle.
  for (const auto& edgeForward : cycle)
  {
    if (jobInfo.trace)
      std::cout << "Join edge: " << edgeForward.first << " <-> " << edgeForward.second
                << " with weight: " << joinEdgesToWeights[edgeForward] << "\n";

    if (joinEdgesToWeights[edgeForward] > maxWeightInCycle)
    {
      maxWeightInCycle = joinEdgesToWeights[edgeForward];
      joinEdgeWithMaxWeight = edgeForward;
    }
  }

  if (jobInfo.trace)
    std::cout << "Join edge with max weight in a cycle: " << joinEdgeWithMaxWeight.first << " <-> "
              << joinEdgeWithMaxWeight.second << " weight: " << maxWeightInCycle << "\n";

  // Search for `large side table`. The `large side table` is table related to the maximum join edge in a
  // cycle, it has a maximum weight among all join edges related to that table and less than maximum join edge
  // in a cycle.
  uint32_t largeSideTable = joinEdgeWithMaxWeight.first;
  if (getSublingsMaxWeightLessThan(joinEdgeWithMaxWeight.second, joinEdgeWithMaxWeight.first,
                                   maxWeightInCycle) >
      getSublingsMaxWeightLessThan(joinEdgeWithMaxWeight.first, joinEdgeWithMaxWeight.second,
                                   maxWeightInCycle))
    largeSideTable = joinEdgeWithMaxWeight.second;

  if (maxWeightInCycle < 0)
    maxWeightInCycle = std::numeric_limits<int64_t>::max() + maxWeightInCycle + 1;
  idbassert(maxWeightInCycle > 0);

  // Add large table to the map for the `join ordering` part.
  if (!jobInfo.tablesForLargeSide.count(largeSideTable))
    jobInfo.tablesForLargeSide.insert({largeSideTable, maxWeightInCycle});

  if (jobInfo.trace)
    std::cout << "Large side table: " << largeSideTable << std::endl;

  // Assign a result edge.
  resultEdge = std::make_pair(joinEdgeWithMaxWeight, maxWeightInCycle);
}

void spanningTreeCheck(TableInfoMap& tableInfoMap, JobStepVector& joinSteps, JobInfo& jobInfo)
{
  bool spanningTree = true;
  unsigned errcode = 0;
  Message::Args args;

  if (jobInfo.trace)
  {
    cout << "Table Connection:" << endl;

    for (TableInfoMap::iterator i = tableInfoMap.begin(); i != tableInfoMap.end(); i++)
    {
      cout << i->first << " :";
      vector<uint32_t>::iterator j = i->second.fAdjacentList.begin();

      while (j != i->second.fAdjacentList.end())
        cout << " " << *j++;

      cout << endl;
    }

    cout << endl;
  }

  if (tableInfoMap.size() < 1)
  {
    spanningTree = false;
    cerr << boldStart << "No table information." << boldStop << endl;
    throw logic_error("No table information.");
  }
  else if (tableInfoMap.size() > 1)
  {
    // 1a. make sure all tables are joined if not a single table query.
    set<uint32_t> nodeSet;
    set<pair<uint32_t, uint32_t>> pathSet;
    vector<uint32_t> joinedTables;
    joinedTables.push_back((tableInfoMap.begin())->first);

    for (size_t i = 0; i < joinedTables.size(); i++)
    {
      vector<uint32_t>& v = tableInfoMap[joinedTables[i]].fAdjacentList;
      nodeSet.insert(joinedTables[i]);

      for (vector<uint32_t>::iterator j = v.begin(); j != v.end(); j++)
      {
        if (nodeSet.find(*j) == nodeSet.end())
          joinedTables.push_back(*j);

        nodeSet.insert(*j);
        pathSet.insert(make_pair(joinedTables[i], *j));
      }
    }

    // 1b. convert expressions to function joins if not connected with simple column joins.
    bool fjAdded = false;

    while (joinedTables.size() < tableInfoMap.size() &&
           addFunctionJoin(joinedTables, joinSteps, nodeSet, pathSet, tableInfoMap, jobInfo))
    {
      fjAdded = true;

      for (size_t i = joinedTables.size() - 1; i < joinedTables.size(); i++)
      {
        vector<uint32_t>& v = tableInfoMap[joinedTables[i]].fAdjacentList;

        for (vector<uint32_t>::iterator j = v.begin(); j != v.end(); j++)
        {
          if (find(joinedTables.begin(), joinedTables.end(), *j) == joinedTables.end())
            joinedTables.push_back(*j);

          nodeSet.insert(*j);
          pathSet.insert(make_pair(joinedTables[i], *j));
        }
      }
    }

    if (jobInfo.trace && fjAdded)
    {
      cout << "Table Connection after adding function join:" << endl;

      for (TableInfoMap::iterator i = tableInfoMap.begin(); i != tableInfoMap.end(); i++)
      {
        cout << i->first << " :";
        vector<uint32_t>::iterator j = i->second.fAdjacentList.begin();

        while (j != i->second.fAdjacentList.end())
          cout << " " << *j++;

        cout << endl;
      }

      cout << endl;
    }

    // Check that join is compatible
    set<string> views1;
    set<string> tables1;
    string errStr;

    vector<uint32_t>::iterator k = joinedTables.begin();

    k = joinedTables.begin();

    for (; k != joinedTables.end(); k++)
    {
      if (jobInfo.keyInfo->tupleKeyVec[*k].fView.empty())
        tables1.insert(jobInfo.keyInfo->tupleKeyToName[*k]);
      else
        views1.insert(jobInfo.keyInfo->tupleKeyVec[*k].fView);

      if (jobInfo.incompatibleJoinMap.find(*k) != jobInfo.incompatibleJoinMap.end())
      {
        errcode = ERR_INCOMPATIBLE_JOIN;

        uint32_t key2 = jobInfo.incompatibleJoinMap[*k];

        if (jobInfo.keyInfo->tupleKeyVec[*k].fView.length() > 0)
        {
          string view2 = jobInfo.keyInfo->tupleKeyVec[key2].fView;

          if (jobInfo.keyInfo->tupleKeyVec[*k].fView == view2)
          {
            //  same view
            errStr += "Tables in '" + view2 + "' have";
          }
          else if (view2.empty())
          {
            // view and real table
            errStr += "'" + jobInfo.keyInfo->tupleKeyVec[*k].fView + "' and '" +
                      jobInfo.keyInfo->tupleKeyToName[key2] + "' have";
          }
          else
          {
            // two views
            errStr += "'" + jobInfo.keyInfo->tupleKeyVec[*k].fView + "' and '" + view2 + "' have";
          }
        }
        else
        {
          string view2 = jobInfo.keyInfo->tupleKeyVec[key2].fView;

          if (view2.empty())
          {
            // two real tables
            errStr += "'" + jobInfo.keyInfo->tupleKeyToName[*k] + "' and '" +
                      jobInfo.keyInfo->tupleKeyToName[key2] + "' have";
          }
          else
          {
            // real table and view
            errStr += "'" + jobInfo.keyInfo->tupleKeyToName[*k] + "' and '" + view2 + "' have";
          }
        }

        args.add(errStr);
        spanningTree = false;
        break;
      }
    }

    // 1c. check again if all tables are joined after pulling in function joins.
    if (joinedTables.size() < tableInfoMap.size())
    {
      vector<uint32_t> notJoinedTables;

      for (TableInfoMap::iterator i = tableInfoMap.begin(); i != tableInfoMap.end(); i++)
      {
        if (find(joinedTables.begin(), joinedTables.end(), i->first) == joinedTables.end())
          notJoinedTables.push_back(i->first);
      }

      set<string> views2;
      set<string> tables2;
      k = notJoinedTables.begin();

      for (; k != notJoinedTables.end(); k++)
      {
        if (jobInfo.keyInfo->tupleKeyVec[*k].fView.empty())
          tables2.insert(jobInfo.keyInfo->tupleKeyToName[*k]);
        else
          views2.insert(jobInfo.keyInfo->tupleKeyVec[*k].fView);
      }

      if (errStr.empty())
      {
        errcode = ERR_MISS_JOIN;

        // 1. check if all tables in a view are joined
        for (set<string>::iterator s = views1.begin(); s != views1.end(); s++)
        {
          if (views2.find(*s) != views2.end())
          {
            errStr = "Tables in '" + (*s) + "' are";
          }
        }

        // 2. tables and views are joined
        if (errStr.empty())
        {
          string set1;

          for (set<string>::iterator s = views1.begin(); s != views1.end(); s++)
          {
            if (set1.empty())
              set1 = "'";
            else
              set1 += ", ";

            set1 += (*s);
          }

          for (set<string>::iterator s = tables1.begin(); s != tables1.end(); s++)
          {
            if (set1.empty())
              set1 = "'";
            else
              set1 += ", ";

            set1 += (*s);
          }

          string set2;

          for (set<string>::iterator s = views2.begin(); s != views2.end(); s++)
          {
            if (set2.empty())
              set2 = "'";
            else
              set2 += ", ";

            set2 += (*s);
          }

          for (set<string>::iterator s = tables2.begin(); s != tables2.end(); s++)
          {
            if (set2.empty())
              set2 = "'";
            else
              set2 += ", ";

            set2 += (*s);
          }

          errStr = set1 + "' and " + set2 + "' are";
          args.add(errStr);
          spanningTree = false;
        }
      }
    }

    // 2. Cycles.
    if (spanningTree && (nodeSet.size() - pathSet.size() / 2 != 1))
    {
      std::unique_ptr<CircularJoinGraphTransformer> joinGraphTransformer;
      if (jobInfo.outerOnTable.size() == 0)
        joinGraphTransformer.reset(new CircularJoinGraphTransformer(tableInfoMap, jobInfo, joinSteps));
      else
        joinGraphTransformer.reset(new CircularOuterJoinGraphTransformer(tableInfoMap, jobInfo, joinSteps));

      joinGraphTransformer->transformJoinGraph();
    }
  }

  if (!spanningTree)
  {
    cerr << boldStart << IDBErrorInfo::instance()->errorMsg(errcode, args) << boldStop << endl;
    throw IDBExcept(IDBErrorInfo::instance()->errorMsg(errcode, args), errcode);
  }
}

void outjoinPredicateAdjust(TableInfoMap& tableInfoMap, JobInfo& jobInfo)
{
  std::set<uint32_t> tables = jobInfo.outerOnTable;
  if (!tables.size())
    return;

  // Mixed outer/inner joins and a table with a `null filter`.
  for (const auto tableId : jobInfo.innerOnTable)
  {
    if (jobInfo.tableHasIsNull.find(tableId) != jobInfo.tableHasIsNull.end())
      tables.insert(tableId);
  }

  for (const auto tableId : tables)
  {
    // resetTableFilters(tableInfoMap[tableId], jobInfo)
    TableInfo& tblInfo = tableInfoMap[tableId];

    if (tblInfo.fTableOid != CNX_VTABLE_ID)
    {
      JobStepVector::iterator k = tblInfo.fQuerySteps.begin();
      JobStepVector onClauseFilterSteps;  //@bug5887, 5311

      for (; k != tblInfo.fQuerySteps.end(); k++)
      {
        if ((*k)->onClauseFilter())
        {
          onClauseFilterSteps.push_back(*k);
          continue;
        }

        uint32_t colKey = -1;
        pColStep* pcs = dynamic_cast<pColStep*>(k->get());
        pColScanStep* pcss = dynamic_cast<pColScanStep*>(k->get());
        pDictionaryScan* pdss = dynamic_cast<pDictionaryScan*>(k->get());
        pDictionaryStep* pdsp = dynamic_cast<pDictionaryStep*>(k->get());
        vector<const execplan::Filter*>* filters = NULL;
        int8_t bop = -1;

        if (pcs != NULL)
        {
          filters = &(pcs->getFilters());
          bop = pcs->BOP();
          colKey = pcs->tupleId();
        }
        else if (pcss != NULL)
        {
          filters = &(pcss->getFilters());
          bop = pcss->BOP();
          colKey = pcss->tupleId();
        }
        else if (pdss != NULL)
        {
          filters = &(pdss->getFilters());
          bop = pdss->BOP();
          colKey = pdss->tupleId();
        }
        else if (pdsp != NULL)
        {
          filters = &(pdsp->getFilters());
          bop = pdsp->BOP();
          colKey = pdsp->tupleId();
        }

        if (filters != NULL && filters->size() > 0)
        {
          ParseTree* pt = new ParseTree((*filters)[0]->clone());

          for (size_t i = 1; i < filters->size(); i++)
          {
            ParseTree* left = pt;
            ParseTree* right = new ParseTree((*filters)[i]->clone());
            ParseTree* op = (BOP_OR == bop) ? new ParseTree(new LogicOperator("or"))
                                            : new ParseTree(new LogicOperator("and"));
            op->left(left);
            op->right(right);

            pt = op;
          }

          ExpressionStep* es = new ExpressionStep(jobInfo);

          if (es == NULL)
            throw runtime_error("Failed to new ExpressionStep 2");

          es->expressionFilter(pt, jobInfo);
          SJSTEP sjstep(es);
          jobInfo.outerJoinExpressions.push_back(sjstep);
          tblInfo.fColsInOuter.push_back(colKey);

          delete pt;
        }
      }

      // Do not apply the primitive filters if there is an "IS NULL" in where clause.
      if (jobInfo.tableHasIsNull.find(tableId) != jobInfo.tableHasIsNull.end())
        tblInfo.fQuerySteps = onClauseFilterSteps;
    }

    jobInfo.outerJoinExpressions.insert(jobInfo.outerJoinExpressions.end(), tblInfo.fOneTableExpSteps.begin(),
                                        tblInfo.fOneTableExpSteps.end());
    tblInfo.fOneTableExpSteps.clear();

    tblInfo.fColsInOuter.insert(tblInfo.fColsInOuter.end(), tblInfo.fColsInExp1.begin(),
                                tblInfo.fColsInExp1.end());
  }
}

uint32_t getLargestTable(JobInfo& jobInfo, TableInfoMap& tableInfoMap, bool overrideLargeSideEstimate)
{
  // Subquery in FROM clause assumptions:
  //   hint will be ignored, if the 1st table in FROM clause is a derived table.
  if (jobInfo.keyInfo->tupleKeyVec[jobInfo.tableList[0]].fId < 3000)
    overrideLargeSideEstimate = false;

  // Bug 2123. Added logic to dynamically determine the large side table unless the SQL statement
  // contained a hint saying to skip the estimation and use the FIRST table in the from clause.
  // Prior to this, we were using the LAST table in the from clause.  We switched it as there
  // were some outer join sqls that couldn't be written with the large table last.
  // Default to the first table in the from clause if:
  //   the user set the hint; or
  //   there is only one table in the query.
  uint32_t ret = jobInfo.tableList.front();

  if (jobInfo.tableList.size() <= 1)
  {
    return ret;
  }

  // Algorithm to dynamically determine the largest table.
  uint64_t largestCardinality = 0;
  uint64_t estimatedRowCount = 0;

  // Loop through the tables and find the one with the largest estimated cardinality.
  for (uint32_t i = 0; i < jobInfo.tableList.size(); i++)
  {
    jobInfo.tableSize[jobInfo.tableList[i]] = 0;
    TableInfoMap::iterator it = tableInfoMap.find(jobInfo.tableList[i]);

    if (it != tableInfoMap.end())
    {
      // @Bug 3771.  Loop through the query steps until the tupleBPS is found instead of
      // just looking at the first one.  Tables in the query that included a filter on a
      // dictionary column were not getting their row count estimated.
      for (JobStepVector::iterator jsIt = it->second.fQuerySteps.begin();
           jsIt != it->second.fQuerySteps.end(); jsIt++)
      {
        TupleBPS* tupleBPS = dynamic_cast<TupleBPS*>((*jsIt).get());

        if (tupleBPS != NULL)
        {
          estimatedRowCount = tupleBPS->getEstimatedRowCount();
          jobInfo.tableSize[jobInfo.tableList[i]] = estimatedRowCount;

          if (estimatedRowCount > largestCardinality)
          {
            ret = jobInfo.tableList[i];
            largestCardinality = estimatedRowCount;
          }

          break;
        }
      }
    }
  }

  // select /*! INFINIDB_ORDERED */
  if (overrideLargeSideEstimate)
  {
    ret = jobInfo.tableList.front();
    jobInfo.tableSize[ret] = numeric_limits<uint64_t>::max();
  }

  return ret;
}

uint32_t getPrevLarge(uint32_t n, TableInfoMap& tableInfoMap)
{
  // root node : no previous node;
  // other node: only one immediate previous node;
  int prev = -1;
  vector<uint32_t>& adjList = tableInfoMap[n].fAdjacentList;

  for (vector<uint32_t>::iterator i = adjList.begin(); i != adjList.end() && prev < 0; i++)
  {
    if (tableInfoMap[*i].fVisited == true)
      prev = *i;
  }

  return prev;
}

uint32_t getKeyIndex(uint32_t key, const RowGroup& rg)
{
  vector<uint32_t>::const_iterator i = rg.getKeys().begin();

  for (; i != rg.getKeys().end(); ++i)
    if (key == *i)
      break;

  if (i == rg.getKeys().end())
    throw runtime_error("No key found.");

  return std::distance(rg.getKeys().begin(), i);
}

bool joinInfoCompare(const SP_JoinInfo& a, const SP_JoinInfo& b)
{
  return (a->fJoinData.fJoinId < b->fJoinData.fJoinId);
}

string joinTypeToString(const JoinType& joinType)
{
  string ret;

  if (joinType & INNER)
    ret = "inner";
  else if (joinType & LARGEOUTER)
    ret = "largeOuter";
  else if (joinType & SMALLOUTER)
    ret = "smallOuter";

  if (joinType & SEMI)
    ret += "+semi";

  if (joinType & ANTI)
    ret += "+ant";

  if (joinType & SCALAR)
    ret += "+scalar";

  if (joinType & MATCHNULLS)
    ret += "+matchnulls";

  if (joinType & WITHFCNEXP)
    ret += "+exp";

  if (joinType & CORRELATED)
    ret += "+correlated";

  return ret;
}

bool matchKeys(const vector<uint32_t>& keysToSearch, const vector<uint32_t>& keysToMatch)
{
  std::unordered_set<uint32_t> keysMap;
  for (const auto key : keysToSearch)
    keysMap.insert(key);

  for (const auto key : keysToMatch)
  {
    if (!keysMap.count(key))
      return false;
  }

  return true;
}

void tryToRestoreJoinEdges(JobInfo& jobInfo, JoinInfo* joinInfo, const RowGroup& largeSideRG,
                           std::vector<uint32_t>& smallKeyIndices, std::vector<uint32_t>& largeKeyIndices,
                           std::vector<std::string>& traces, std::map<int64_t, uint32_t>& joinIndexIdMap,
                           uint32_t smallSideIndex)
{
  if (!jobInfo.joinEdgesToRestore.size())
    return;

  const RowGroup& smallSideRG = joinInfo->fRowGroup;
  ostringstream oss;
  if (jobInfo.trace)
    oss << "\n\nTry to match edges for the small and large sides rowgroups\n";

  std::vector<uint32_t> smallKeyIndicesToRestore;
  std::vector<uint32_t> largeKeyIndicesToRestore;
  std::vector<pair<JoinEdge, int64_t>> takenEdgesWithJoinIDs;
  auto& joinEdgesToRestore = jobInfo.joinEdgesToRestore;

  // We could have a multple join edges to restore from the same vertex e.g:
  // t1 -> t2 -> t3
  //   ^    ^     |
  //     \  |     V
  //       t5 <- t4
  // Edges to restore: {t5, t2}, {t5, t1}
  // Large side row group: {t5}
  // Small side row group: {t1, t2, t3, t4}
  // Large side join keys: {t5, t5}
  // Small side join keys: {t2, t1}
  for (const auto& [edge, joinId] : joinEdgesToRestore)
  {
    auto it = jobInfo.tableJoinMap.find(edge);
    // Edge keys.
    const auto& leftKeys = it->second.fLeftKeys;
    const auto& rightKeys = it->second.fRightKeys;
    // Keys for the given rowgroups.
    // Large side and small side.
    const auto& smallSideKeys = smallSideRG.getKeys();
    const auto& largeSideKeys = largeSideRG.getKeys();

    // Check if left in large and right in small.
    if (matchKeys(largeSideKeys, leftKeys) && matchKeys(smallSideKeys, rightKeys))
    {
      for (uint32_t i = 0, e = leftKeys.size(); i < e; ++i)
        largeKeyIndicesToRestore.push_back(getKeyIndex(leftKeys[i], largeSideRG));

      for (uint32_t i = 0, e = rightKeys.size(); i < e; ++i)
        smallKeyIndicesToRestore.push_back(getKeyIndex(rightKeys[i], smallSideRG));

      if (jobInfo.trace)
      {
        oss << "Keys matched.\n";
        oss << "Left keys:\n";
        for (const auto key : leftKeys)
          oss << key << " ";
        oss << "\nRight keys:\n";
        for (const auto key : rightKeys)
          oss << key << " ";
        oss << '\n';
      }

      takenEdgesWithJoinIDs.push_back({edge, joinId});
      continue;
    }

    // Otherwise check right in large and left in small.
    if (matchKeys(largeSideKeys, rightKeys) && matchKeys(smallSideKeys, leftKeys))
    {
      for (uint32_t i = 0, e = rightKeys.size(); i < e; ++i)
        largeKeyIndicesToRestore.push_back(getKeyIndex(rightKeys[i], largeSideRG));

      for (uint32_t i = 0, e = leftKeys.size(); i < e; ++i)
        smallKeyIndicesToRestore.push_back(getKeyIndex(leftKeys[i], smallSideRG));

      if (jobInfo.trace)
      {
        oss << "Keys matched.\n";
        oss << "Left keys:\n";
        for (const auto key : leftKeys)
          oss << key << " ";
        oss << "\nRight keys:\n";
        for (const auto key : rightKeys)
          oss << key << " ";
        oss << '\n';
      }

      takenEdgesWithJoinIDs.push_back({edge, joinId});
    }
  }

  // Check if keys were not matched.
  if (!smallKeyIndicesToRestore.size())
  {
    if (jobInfo.trace)
      oss << "Keys not matched.\n\n";

    traces.push_back(oss.str());
    return;
  }

  // Add keys.
  smallKeyIndices.insert(smallKeyIndices.end(), smallKeyIndicesToRestore.begin(),
                         smallKeyIndicesToRestore.end());
  largeKeyIndices.insert(largeKeyIndices.end(), largeKeyIndicesToRestore.begin(),
                         largeKeyIndicesToRestore.end());
  // Mark as tupeless for multiple keys join.
  joinInfo->fJoinData.fTypeless = true;

  // Associate a join id and small side index for the on clause filters and remove taken edges.
  for (const auto& [edge, joinId] : takenEdgesWithJoinIDs)
  {
    joinIndexIdMap[joinId] = smallSideIndex;
    auto it = joinEdgesToRestore.find(edge);
    joinEdgesToRestore.erase(it);
  }

  if (jobInfo.trace)
  {
    oss << "Keys restored.\n";
    oss << "Small side keys:\n";
    for (const auto key : smallKeyIndices)
      oss << key << " ";
    oss << "\nLarge side keys:\n";
    for (const auto key : largeKeyIndices)
      oss << key << " ";
    oss << "\n\n";
  }

  traces.push_back(oss.str());
}

void matchEdgesInResultRowGroup(const JobInfo& jobInfo, const RowGroup& rg,
                                std::map<JoinEdge, int64_t>& edgesToRestore,
                                PostJoinFilterKeys& postJoinFilterKeys)
{
  if (jobInfo.trace)
  {
    cout << "\nTrying to match the RowGroup to apply a post join "
            "filter\n";
  }

  std::vector<JoinEdge> takenEdges;
  for (const auto& [edge, joinId] : edgesToRestore)
  {
    std::vector<uint32_t> currentKeys;
    auto it = jobInfo.tableJoinMap.find(edge);

    // Combine keys.
    currentKeys = it->second.fLeftKeys;
    currentKeys.insert(currentKeys.end(), it->second.fRightKeys.begin(), it->second.fRightKeys.end());

    // Rowgroup keys.
    const auto& rgKeys = rg.getKeys();
    uint32_t keyIndex = 0;
    uint32_t keySize = currentKeys.size();

    // Search for keys in result rowgroup.
    while (keyIndex < keySize)
    {
      auto keyIt = std::find(rgKeys.begin(), rgKeys.end(), currentKeys[keyIndex]);
      // We have to match all keys.
      if (keyIt == rgKeys.end())
        break;

      ++keyIndex;
    }

    if (jobInfo.trace)
    {
      if (keyIndex == keySize)
        cout << "\nRowGroup matched\n";
      else
        cout << "\nRowGroup not matched\n";

      cout << rg.toString() << endl;
      cout << "For the following keys:\n";
      for (auto key : currentKeys)
        cout << key << " ";
      cout << endl;
    }

    // All keys matched in current Rowgroup.
    if (keyIndex == keySize)
    {
      // Add macthed keys.
      postJoinFilterKeys.push_back(make_pair(edge, currentKeys));
      takenEdges.push_back(edge);
    }
  }

  // Erase taken edges.
  for (const auto& edge : takenEdges)
  {
    auto it = edgesToRestore.find(edge);
    edgesToRestore.erase(it);
  }
}

void createPostJoinFilters(const JobInfo& jobInfo, TableInfoMap& tableInfoMap,
                           const PostJoinFilterKeys& postJoinFilterKeys,
                           const std::map<uint32_t, uint32_t>& keyToIndexMap,
                           std::vector<SimpleFilter*>& postJoinFilters)
{
  for (const auto& p : postJoinFilterKeys)
  {
    const auto& edge = p.first;
    const auto& keys = p.second;

    if (jobInfo.trace)
      cout << "\nRestore a cycle as a post join filter\n";

    uint32_t leftKeyIndex = 0;
    uint32_t rightKeyIndex = keys.size() / 2;
    // Left end is where right starts.
    const uint32_t leftSize = rightKeyIndex;

    while (leftKeyIndex < leftSize)
    {
      // Keys.
      auto leftKey = keys[leftKeyIndex];
      auto rightKey = keys[rightKeyIndex];

      // Column oids.
      auto leftOid = jobInfo.keyInfo->tupleKeyVec[leftKey].fId;
      auto rightOid = jobInfo.keyInfo->tupleKeyVec[rightKey].fId;

      // Column types.
      auto leftType = jobInfo.keyInfo->colType[keys[leftKeyIndex]];
      auto rightType = jobInfo.keyInfo->colType[keys[rightKeyIndex]];

      CalpontSystemCatalog::TableColName leftTableColName;
      CalpontSystemCatalog::TableColName rightTableColName;

      // Check for the dict.
      if (joblist::isDictCol(leftType) && joblist::isDictCol(rightType))
      {
        leftTableColName = jobInfo.csc->dictColName(leftOid);
        rightTableColName = jobInfo.csc->dictColName(rightOid);
      }
      else
      {
        leftTableColName = jobInfo.csc->colName(leftOid);
        rightTableColName = jobInfo.csc->colName(rightOid);
      }

      // Create columns.
      auto* leftColumn =
          new SimpleColumn(leftTableColName.schema, leftTableColName.table, leftTableColName.column);
      auto* rightColumn =
          new SimpleColumn(rightTableColName.schema, rightTableColName.table, rightTableColName.column);

      // Set column indices in the result Rowgroup.
      auto leftIndexIt = keyToIndexMap.find(leftKey);
      if (leftIndexIt != keyToIndexMap.end())
      {
        leftColumn->inputIndex(leftIndexIt->second);
      }
      else
      {
        std::cerr << "Cannot find key: " << leftKey << " in the IndexMap " << std::endl;
        throw logic_error("Post join filter: Cannot find key in the index map");
      }

      auto rightIndexIt = keyToIndexMap.find(rightKey);
      if (rightIndexIt != keyToIndexMap.end())
      {
        rightColumn->inputIndex(rightIndexIt->second);
      }
      else
      {
        std::cerr << "Cannot find key: " << rightKey << " in the IndexMap " << std::endl;
        throw logic_error("Post join filter: Cannot find key in the index map");
      }

      // Create an eq operator.
      SOP eqPredicateOperator(new PredicateOperator("="));
      // Set a type.
      eqPredicateOperator->setOpType(leftColumn->resultType(), rightColumn->resultType());
      // Create a post join filter.
      SimpleFilter* joinFilter = new SimpleFilter(eqPredicateOperator, leftColumn, rightColumn);
      postJoinFilters.push_back(joinFilter);

      // Erase keys from fColsInExp2.
      auto& firstExp2 = tableInfoMap[edge.first].fColsInExp2;
      auto keyItInExp2 = std::find(firstExp2.begin(), firstExp2.end(), leftKey);
      if (keyItInExp2 != firstExp2.end())
        firstExp2.erase(keyItInExp2);

      auto& secondExp2 = tableInfoMap[edge.second].fColsInExp2;
      keyItInExp2 = std::find(secondExp2.begin(), secondExp2.end(), rightKey);
      if (keyItInExp2 != secondExp2.end())
        secondExp2.erase(keyItInExp2);

      ++leftKeyIndex;
      ++rightKeyIndex;
    }
  }

  if (jobInfo.trace)
  {
    if (!postJoinFilters.empty())
    {
      cout << "Post join filters created." << endl;
      for (auto* filter : postJoinFilters)
        cout << filter->toString() << endl;
    }
    else
    {
      std::cout << "Post join filters were not created." << std::endl;
    }
  }
}

SP_JoinInfo joinToLargeTable(uint32_t large, TableInfoMap& tableInfoMap, JobInfo& jobInfo,
                             vector<uint32_t>& joinOrder, std::map<JoinEdge, int64_t>& joinEdgesToRestore)
{
  vector<SP_JoinInfo> smallSides;
  tableInfoMap[large].fVisited = true;
  tableInfoMap[large].fJoinedTables.insert(large);
  set<uint32_t>& tableSet = tableInfoMap[large].fJoinedTables;
  vector<uint32_t>& adjList = tableInfoMap[large].fAdjacentList;
  uint32_t prevLarge = (uint32_t)getPrevLarge(large, tableInfoMap);
  bool root = (prevLarge == (uint32_t)-1);
  uint32_t link = large;
  uint32_t cId = -1;

  // Get small sides ready.
  for (unsigned int& adj : adjList)
  {
    if (!tableInfoMap[adj].fVisited)
    {
      cId = adj;
      smallSides.push_back(joinToLargeTable(adj, tableInfoMap, jobInfo, joinOrder, joinEdgesToRestore));

      tableSet.insert(tableInfoMap[adj].fJoinedTables.begin(), tableInfoMap[adj].fJoinedTables.end());
    }
  }

  // Join with its small sides, if not a leaf node.
  if (!smallSides.empty())
  {
    // non-leaf node, need a join
    SJSTEP spjs = tableInfoMap[large].fQuerySteps.back();
    auto* bps = dynamic_cast<BatchPrimitive*>(spjs.get());
    auto* tsas = dynamic_cast<SubAdapterStep*>(spjs.get());
    auto* thjs = dynamic_cast<TupleHashJoinStep*>(spjs.get());

    // @bug6158, try to put BPS on large side if possible
    if (tsas && smallSides.size() == 1)
    {
      SJSTEP sspjs = tableInfoMap[cId].fQuerySteps.back();
      auto* sbps = dynamic_cast<BatchPrimitive*>(sspjs.get());
      auto* sthjs = dynamic_cast<TupleHashJoinStep*>(sspjs.get());

      if (sbps || (sthjs && sthjs->tokenJoin() == cId))
      {
        SP_JoinInfo largeJoinInfo(new JoinInfo);
        largeJoinInfo->fTableOid = tableInfoMap[large].fTableOid;
        largeJoinInfo->fAlias = tableInfoMap[large].fAlias;
        largeJoinInfo->fView = tableInfoMap[large].fView;
        largeJoinInfo->fSchema = tableInfoMap[large].fSchema;
        largeJoinInfo->fPartitions = tableInfoMap[large].fPartitions;

        largeJoinInfo->fDl = tableInfoMap[large].fDl;
        largeJoinInfo->fRowGroup = tableInfoMap[large].fRowGroup;

        auto mit = jobInfo.tableJoinMap.find(make_pair(large, cId));

        if (mit == jobInfo.tableJoinMap.end())
          throw runtime_error("Join step not found.");

        largeJoinInfo->fJoinData = mit->second;

        // switch large and small sides
        joinOrder.back() = large;
        large = cId;
        smallSides[0] = largeJoinInfo;
        tableInfoMap[large].fJoinedTables = tableSet;

        bps = sbps;
        thjs = sthjs;
        tsas = nullptr;
      }
    }

    if (!bps && !thjs && !tsas)
    {
      if (dynamic_cast<SubQueryStep*>(spjs.get()))
        throw IDBExcept(ERR_NON_SUPPORT_SUB_QUERY_TYPE);

      throw runtime_error("Not supported join.");
    }

    size_t dcf = 0;  // for dictionary column filters, 0 if thjs is null.
    RowGroup largeSideRG = tableInfoMap[large].fRowGroup;

    if (thjs && thjs->tokenJoin() == large && thjs->tupleId1() != thjs->tupleId2())
    {
      dcf = thjs->getLargeKeys().size();
      largeSideRG = thjs->getLargeRowGroup();
    }

    // info for debug trace
    vector<string> tableNames;
    vector<string> traces;

    // sort the smallsides base on the joinId
    sort(smallSides.begin(), smallSides.end(), joinInfoCompare);
    int64_t lastJoinId = smallSides.back()->fJoinData.fJoinId;

    // get info to config the TupleHashjoin
    DataListVec smallSideDLs;
    vector<RowGroup> smallSideRGs;
    vector<JoinType> jointypes;
    vector<bool> typeless;
    vector<vector<uint32_t>> smallKeyIndices;
    vector<vector<uint32_t>> largeKeyIndices;

    for (auto& smallSide : smallSides)
    {
      JoinInfo* info = smallSide.get();
      smallSideDLs.push_back(info->fDl);
      smallSideRGs.push_back(info->fRowGroup);
      jointypes.push_back(info->fJoinData.fTypes[0]);
      typeless.push_back(info->fJoinData.fTypeless);

      vector<uint32_t> smallIndices;
      vector<uint32_t> largeIndices;
      const vector<uint32_t>& keys1 = info->fJoinData.fLeftKeys;
      const vector<uint32_t>& keys2 = info->fJoinData.fRightKeys;
      auto k1 = keys1.begin();
      auto k2 = keys2.begin();
      uint32_t stid = getTableKey(jobInfo, *k1);
      tableNames.push_back(jobInfo.keyInfo->tupleKeyVec[stid].fTable);

      for (; k1 != keys1.end(); ++k1, ++k2)
      {
        smallIndices.push_back(getKeyIndex(*k1, info->fRowGroup));
        largeIndices.push_back(getKeyIndex(*k2, largeSideRG));
      }

      smallKeyIndices.push_back(smallIndices);
      largeKeyIndices.push_back(largeIndices);

      if (jobInfo.trace)
      {
        // small side column
        ostringstream smallKey, smallIndex;
        string alias1 = jobInfo.keyInfo->keyName[getTableKey(jobInfo, keys1.front())];
        smallKey << alias1 << "-";

        for (k1 = keys1.begin(); k1 != keys1.end(); ++k1)
        {
          CalpontSystemCatalog::OID oid1 = jobInfo.keyInfo->tupleKeyVec[*k1].fId;
          CalpontSystemCatalog::TableColName tcn1 = jobInfo.csc->colName(oid1);
          smallKey << "(" << tcn1.column << ":" << oid1 << ":" << *k1 << ")";
          smallIndex << " " << getKeyIndex(*k1, info->fRowGroup);
        }

        // large side column
        ostringstream largeKey, largeIndex;
        string alias2 = jobInfo.keyInfo->keyName[getTableKey(jobInfo, keys2.front())];
        largeKey << alias2 << "-";

        for (k2 = keys2.begin(); k2 != keys2.end(); ++k2)
        {
          CalpontSystemCatalog::OID oid2 = jobInfo.keyInfo->tupleKeyVec[*k2].fId;
          CalpontSystemCatalog::TableColName tcn2 = jobInfo.csc->colName(oid2);
          largeKey << "(" << tcn2.column << ":" << oid2 << ":" << *k2 << ")";
          largeIndex << " " << getKeyIndex(*k2, largeSideRG);
        }

        ostringstream oss;
        oss << smallKey.str() << " join on " << largeKey.str()
            << " joinType: " << info->fJoinData.fTypes.front() << "("
            << joinTypeToString(info->fJoinData.fTypes.front()) << ")"
            << (info->fJoinData.fTypeless ? " " : " !") << "typeless" << endl;
        oss << "smallSideIndex-largeSideIndex :" << smallIndex.str() << " --" << largeIndex.str() << endl;
        oss << "small side RG" << endl << info->fRowGroup.toString() << endl;
        traces.push_back(oss.str());
      }
    }

    if (jobInfo.trace)
    {
      ostringstream oss;
      oss << "large side RG" << endl << largeSideRG.toString() << endl;
      traces.push_back(oss.str());
    }

    // If the tupleIDs are the same it's not a join, so a new TupleHashJoinStep must be created
    if (bps || tsas || (thjs && thjs->tupleId1() == thjs->tupleId2()))
    {
      thjs = new TupleHashJoinStep(jobInfo);
      thjs->tableOid1(smallSides[0]->fTableOid);
      thjs->tableOid2(tableInfoMap[large].fTableOid);
      thjs->alias1(smallSides[0]->fAlias);
      thjs->alias2(tableInfoMap[large].fAlias);
      thjs->view1(smallSides[0]->fView);
      thjs->view2(tableInfoMap[large].fView);
      thjs->schema1(smallSides[0]->fSchema);
      thjs->schema2(tableInfoMap[large].fSchema);
      thjs->setLargeSideBPS(bps);
      thjs->joinId(lastJoinId);

      if (dynamic_cast<TupleBPS*>(bps) != NULL)
        bps->incWaitToRunStepCnt();

      SJSTEP spjs(thjs);

      JobStepAssociation inJsa;
      inJsa.outAdd(smallSideDLs, 0);
      inJsa.outAdd(tableInfoMap[large].fDl);
      thjs->inputAssociation(inJsa);
      thjs->setLargeSideDLIndex(inJsa.outSize() - 1);

      JobStepAssociation outJsa;
      AnyDataListSPtr spdl(new AnyDataList());
      RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
      spdl->rowGroupDL(dl);
      dl->OID(large);
      outJsa.outAdd(spdl);
      thjs->outputAssociation(outJsa);
      thjs->configSmallSideRG(smallSideRGs, tableNames);
      thjs->configLargeSideRG(tableInfoMap[large].fRowGroup);
      thjs->configJoinKeyIndex(jointypes, typeless, smallKeyIndices, largeKeyIndices);

      tableInfoMap[large].fQuerySteps.push_back(spjs);
      tableInfoMap[large].fDl = spdl;
    }
    else
    {
      JobStepAssociation inJsa = thjs->inputAssociation();

      if (inJsa.outSize() < 2)
        throw runtime_error("Not enough input to a hashjoin.");

      size_t last = inJsa.outSize() - 1;
      inJsa.outAdd(smallSideDLs, last);
      thjs->inputAssociation(inJsa);
      thjs->setLargeSideDLIndex(inJsa.outSize() - 1);

      thjs->addSmallSideRG(smallSideRGs, tableNames);
      thjs->addJoinKeyIndex(jointypes, typeless, smallKeyIndices, largeKeyIndices);
    }

    RowGroup rg;
    constructJoinedRowGroup(rg, link, prevLarge, root, tableSet, tableInfoMap, jobInfo);
    thjs->setOutputRowGroup(rg);
    tableInfoMap[large].fRowGroup = rg;

    if (jobInfo.trace)
    {
      cout << boldStart << "\n====== join info ======\n" << boldStop;

      for (vector<string>::iterator t = traces.begin(); t != traces.end(); ++t)
        cout << *t;

      cout << "RowGroup join result: " << endl << rg.toString() << endl << endl;
    }

    // check if any cross-table expressions can be evaluated after the join
    JobStepVector readyExpSteps;
    JobStepVector& expSteps = jobInfo.crossTableExpressions;
    JobStepVector::iterator eit = expSteps.begin();

    while (eit != expSteps.end())
    {
      ExpressionStep* exp = dynamic_cast<ExpressionStep*>(eit->get());

      if (exp == NULL)
        throw runtime_error("Not an expression.");

      if (exp->functionJoin())
      {
        eit++;
        continue;  // done as join
      }

      const vector<uint32_t>& tables = exp->tableKeys();
      uint64_t i = 0;

      for (; i < tables.size(); i++)
      {
        if (tableSet.find(tables[i]) == tableSet.end())
          break;
      }

      // all tables for this expression are joined
      if (tables.size() == i)
      {
        readyExpSteps.push_back(*eit);
        eit = expSteps.erase(eit);
      }
      else
      {
        eit++;
      }
    }

    // if root, handle the delayed outer join filters
    if (root && jobInfo.outerJoinExpressions.size() > 0)
      readyExpSteps.insert(readyExpSteps.end(), jobInfo.outerJoinExpressions.begin(),
                           jobInfo.outerJoinExpressions.end());

    // Check if we have a `join edges` to restore as post join filter for result rowgroup.
    PostJoinFilterKeys postJoinFilterKeys;
    if (joinEdgesToRestore.size())
      matchEdgesInResultRowGroup(jobInfo, rg, joinEdgesToRestore, postJoinFilterKeys);

    // check additional compares for semi-join.
    if (readyExpSteps.size() || postJoinFilterKeys.size())
    {
      // tables have additional comparisons
      map<uint32_t, int> correlateTables;          // index in thjs
      map<uint32_t, ParseTree*> correlateCompare;  // expression
      // map keys to the indices in the RG
      map<uint32_t, uint32_t> keyToIndexMap;

      const auto& rowGroupKeys = rg.getKeys();
      for (uint64_t i = 0, e = rowGroupKeys.size(); i < e; ++i)
        keyToIndexMap.insert(make_pair(rowGroupKeys[i], i));

      if (readyExpSteps.size() > 0)
      {
        for (size_t i = 0; i != smallSides.size(); i++)
        {
          if ((jointypes[i] & SEMI) || (jointypes[i] & ANTI) || (jointypes[i] & SCALAR))
          {
            uint32_t tid =
                getTableKey(jobInfo, smallSides[i]->fTableOid, smallSides[i]->fAlias, smallSides[i]->fSchema,
                            smallSides[i]->fView, smallSides[i]->fPartitions);
            correlateTables[tid] = i;
            correlateCompare[tid] = NULL;
          }
        }
      }

      if (readyExpSteps.size() && correlateTables.size())
      {
        // separate additional compare for each table pair
        JobStepVector::iterator eit = readyExpSteps.begin();

        while (eit != readyExpSteps.end())
        {
          ExpressionStep* e = dynamic_cast<ExpressionStep*>(eit->get());

          if (e->selectFilter())
          {
            // @bug3780, leave select filter to normal expression
            eit++;
            continue;
          }

          const vector<uint32_t>& tables = e->tableKeys();
          map<uint32_t, int>::iterator j = correlateTables.end();

          for (size_t i = 0; i < tables.size(); i++)
          {
            j = correlateTables.find(tables[i]);

            if (j != correlateTables.end())
              break;
          }

          if (j == correlateTables.end())
          {
            eit++;
            continue;
          }

          // map the input column index
          e->updateInputIndex(keyToIndexMap, jobInfo);
          ParseTree* pt = correlateCompare[j->first];

          if (pt == NULL)
          {
            // first expression
            pt = new ParseTree;
            pt->copyTree(*(e->expressionFilter()));
          }
          else
          {
            // combine the expressions
            ParseTree* left = pt;
            ParseTree* right = new ParseTree;
            right->copyTree(*(e->expressionFilter()));
            pt = new ParseTree(new LogicOperator("and"));
            pt->left(left);
            pt->right(right);
          }

          correlateCompare[j->first] = pt;
          eit = readyExpSteps.erase(eit);
        }

        map<uint32_t, int>::iterator k = correlateTables.begin();

        while (k != correlateTables.end())
        {
          ParseTree* pt = correlateCompare[k->first];

          if (pt != NULL)
          {
            boost::shared_ptr<ParseTree> sppt(pt);
            thjs->addJoinFilter(sppt, dcf + k->second);
          }

          k++;
        }

        thjs->setJoinFilterInputRG(rg);
      }

      // Normal expression or post join filters.
      if (readyExpSteps.size() || postJoinFilterKeys.size())
      {
        std::vector<SimpleFilter*> postJoinFilters;
        if (postJoinFilterKeys.size())
          createPostJoinFilters(jobInfo, tableInfoMap, postJoinFilterKeys, keyToIndexMap, postJoinFilters);

        // Add the expression steps in where clause can be solved by this join to bps.
        ParseTree* pt = NULL;
        for (auto* joinFilter : postJoinFilters)
        {
          if (pt == nullptr)
          {
            pt = new ParseTree(joinFilter);
          }
          else
          {
            ParseTree* left = pt;
            ParseTree* right = new ParseTree(joinFilter);
            pt = new ParseTree(new LogicOperator("and"));
            pt->left(left);
            pt->right(right);
          }
        }

        JobStepVector::iterator eit = readyExpSteps.begin();
        for (; eit != readyExpSteps.end(); eit++)
        {
          // map the input column index
          ExpressionStep* e = dynamic_cast<ExpressionStep*>(eit->get());
          e->updateInputIndex(keyToIndexMap, jobInfo);

          if (pt == NULL)
          {
            // first expression
            pt = new ParseTree;
            pt->copyTree(*(e->expressionFilter()));
          }
          else
          {
            // combine the expressions
            ParseTree* left = pt;
            ParseTree* right = new ParseTree;
            right->copyTree(*(e->expressionFilter()));
            pt = new ParseTree(new LogicOperator("and"));
            pt->left(left);
            pt->right(right);
          }
        }

        if (pt)
        {
          boost::shared_ptr<ParseTree> sppt(pt);
          thjs->addFcnExpGroup2(sppt);
        }
      }

      // update the fColsInExp2 and construct the output RG
      updateExp2Cols(readyExpSteps, tableInfoMap, jobInfo);
      constructJoinedRowGroup(rg, link, prevLarge, root, tableSet, tableInfoMap, jobInfo);

      if (thjs->hasFcnExpGroup2())
        thjs->setFE23Output(rg);
      else
        thjs->setOutputRowGroup(rg);

      tableInfoMap[large].fRowGroup = rg;

      if (jobInfo.trace)
      {
        cout << "RowGroup of " << tableInfoMap[large].fAlias << " after EXP G2: " << endl
             << rg.toString() << endl
             << endl;
      }
    }
  }

  // Prepare the current table info to join with its large side.
  SP_JoinInfo joinInfo(new JoinInfo);
  joinInfo->fTableOid = tableInfoMap[large].fTableOid;
  joinInfo->fAlias = tableInfoMap[large].fAlias;
  joinInfo->fView = tableInfoMap[large].fView;
  joinInfo->fSchema = tableInfoMap[large].fSchema;

  joinInfo->fDl = tableInfoMap[large].fDl;
  joinInfo->fRowGroup = tableInfoMap[large].fRowGroup;

  if (root == false)  // not root
  {
    TableJoinMap::iterator mit = jobInfo.tableJoinMap.find(make_pair(link, prevLarge));

    if (mit == jobInfo.tableJoinMap.end())
      throw runtime_error("Join step not found.");

    joinInfo->fJoinData = mit->second;
  }

  joinOrder.push_back(large);

  return joinInfo;
}

bool joinStepCompare(const SJSTEP& a, const SJSTEP& b)
{
  return (dynamic_cast<TupleHashJoinStep*>(a.get())->joinId() <
          dynamic_cast<TupleHashJoinStep*>(b.get())->joinId());
}

struct JoinOrderData
{
  uint32_t fTid1;
  uint32_t fTid2;
  int64_t fJoinId;
};

void getJoinOrder(vector<JoinOrderData>& joins, JobStepVector& joinSteps, JobInfo& jobInfo)
{
  sort(joinSteps.begin(), joinSteps.end(), joinStepCompare);

  for (JobStepVector::iterator i = joinSteps.begin(); i < joinSteps.end(); i++)
  {
    TupleHashJoinStep* thjs = dynamic_cast<TupleHashJoinStep*>(i->get());
    JoinOrderData jo;
    jo.fTid1 = getTableKey(jobInfo, thjs->tupleId1());
    jo.fTid2 = getTableKey(jobInfo, thjs->tupleId2());
    jo.fJoinId = thjs->joinId();

    // not fastest, but good for a small list
    vector<JoinOrderData>::iterator j;

    for (j = joins.begin(); j < joins.end(); j++)
    {
      if ((j->fTid1 == jo.fTid1 && j->fTid2 == jo.fTid2) || (j->fTid1 == jo.fTid2 && j->fTid2 == jo.fTid1))
      {
        j->fJoinId = jo.fJoinId;
        break;
      }
    }

    // insert unique join pair
    if (j == joins.end())
      joins.push_back(jo);
  }
}

inline void updateJoinSides(uint32_t small, uint32_t large, map<uint32_t, SP_JoinInfo>& joinInfoMap,
                            vector<SP_JoinInfo>& smallSides, TableInfoMap& tableInfoMap, JobInfo& jobInfo)
{
  TableJoinMap::iterator mit = jobInfo.tableJoinMap.find(make_pair(small, large));

  if (mit == jobInfo.tableJoinMap.end())
    throw runtime_error("Join step not found.");

  joinInfoMap[small]->fJoinData = mit->second;
  tableInfoMap[small].fJoinedTables.insert(small);
  smallSides.push_back(joinInfoMap[small]);

  tableInfoMap[large].fJoinedTables.insert(tableInfoMap[small].fJoinedTables.begin(),
                                           tableInfoMap[small].fJoinedTables.end());
  tableInfoMap[large].fJoinedTables.insert(large);
}

inline bool needsFeForSmallSides(const JobInfo& jobInfo, const std::vector<JoinOrderData>& joins,
                                 const std::set<uint32_t>& smallSideTid, uint32_t tableId)
{
  const auto it = jobInfo.joinFeTableMap.find(joins[tableId].fJoinId);
  if (it != jobInfo.joinFeTableMap.end())
  {
    const set<uint32_t>& tids = it->second;
    for (const auto si : smallSideTid)
    {
      if (tids.count(si))
        return true;
    }
  }
  return false;
}

// For OUTER JOIN bug @2422/2633/3437/3759, join table based on join order.
// The largest table will be always the streaming table, other tables are always on small side.
void joinTablesInOrder(uint32_t largest, JobStepVector& joinSteps, TableInfoMap& tableInfoMap,
                       JobInfo& jobInfo, vector<uint32_t>& joinOrder)
{
  // populate the tableInfo for join
  std::map<uint32_t, SP_JoinInfo> joinInfoMap;  // <table, JoinInfo>

  // <table,  <last step involved, large priority> >
  // large priority:
  //     -1 - must be on small side, like derived tables for semi join;
  //      0 - prefer to be on small side, like FROM subquery;
  //      1 - can be on either large or small side;
  //      2 - must be on large side.
  map<uint32_t, pair<SJSTEP, int64_t>> joinStepMap;
  BatchPrimitive* bps = NULL;
  SubAdapterStep* tsas = NULL;
  TupleHashJoinStep* thjs = NULL;

  for (vector<uint32_t>::iterator i = jobInfo.tableList.begin(); i < jobInfo.tableList.end(); i++)
  {
    SP_JoinInfo joinInfo(new JoinInfo);
    joinInfo->fTableOid = tableInfoMap[*i].fTableOid;
    joinInfo->fAlias = tableInfoMap[*i].fAlias;
    joinInfo->fView = tableInfoMap[*i].fView;
    joinInfo->fSchema = tableInfoMap[*i].fSchema;

    joinInfo->fDl = tableInfoMap[*i].fDl;
    joinInfo->fRowGroup = tableInfoMap[*i].fRowGroup;

    joinInfoMap[*i] = joinInfo;

    SJSTEP spjs = tableInfoMap[*i].fQuerySteps.back();
    bps = dynamic_cast<BatchPrimitive*>(spjs.get());
    tsas = dynamic_cast<SubAdapterStep*>(spjs.get());
    thjs = dynamic_cast<TupleHashJoinStep*>(spjs.get());
    TupleBPS* tbps = dynamic_cast<TupleBPS*>(spjs.get());

    /* We have to specify for `TupleHasJoinStep` associated with key to be restored, to be on a large side,
       to avoid situation with multiple small sides, when CS tries to merge join steps.

       1. Consider the following join graph:
           t2
          /  \
        t1 -- t3

       {t3, t1} has a max weight, we choose that edge to remove from join graph.

       2. Join graph after {t3, t1} edge was removed:
           t2
          /  \
        t1    t3

       a. It's legal for CS to schedule this joins as one step join as follow:
       join(small sides {t1, t3}, large side {t2})

       b. Instead of generating two steps join as follow:
       t1_t2 = join(small sides {t1}, large side {t2})
       join(small sides {t1_t2}, large side {t3)}

       In case of `a` we unable to implement a join with a multiple keys.
     */

    if (jobInfo.tablesForLargeSide.count(*i))
    {
      const auto tableWeight = jobInfo.tablesForLargeSide[*i];
      joinStepMap[*i] = make_pair(spjs, tableWeight);
    }
    else if (*i == largest)
    {
      joinStepMap[*i] = make_pair(spjs, 2);
    }
    else if (tbps || thjs)
    {
      joinStepMap[*i] = make_pair(spjs, 1);
    }
    else if (tsas)
    {
      joinStepMap[*i] = make_pair(spjs, 0);
    }
    else
    {
      joinStepMap[*i] = make_pair(spjs, -1);
    }
  }

  // sort the join steps based on join ID.
  std::vector<JoinOrderData> joins;
  getJoinOrder(joins, joinSteps, jobInfo);

  // join the steps
  int64_t lastJoinId = -1;
  uint32_t large = (uint32_t)-1;
  uint32_t small = (uint32_t)-1;
  uint32_t prevLarge = (uint32_t)-1;
  bool umstream = false;
  vector<uint32_t> joinedTable;
  uint32_t lastJoin = joins.size() - 1;
  bool isSemijoin = true;

  for (uint64_t js = 0; js < joins.size(); js++)
  {
    std::set<uint32_t> smallSideTid;

    if (joins[js].fJoinId != 0)
      isSemijoin = false;

    std::vector<SP_JoinInfo> smallSides;
    uint32_t tid1 = joins[js].fTid1;
    uint32_t tid2 = joins[js].fTid2;
    lastJoinId = joins[js].fJoinId;

    // largest has already joined, and this join cannot be merged.
    if (prevLarge == largest && tid1 != largest && tid2 != largest)
      umstream = true;

    if (joinStepMap[tid1].second > joinStepMap[tid2].second)  // high priority
    {
      large = tid1;
      small = tid2;
    }
    else if (joinStepMap[tid1].second == joinStepMap[tid2].second &&
             jobInfo.tableSize[tid1] >= jobInfo.tableSize[tid2])  // favor t1 for hint
    {
      large = tid1;
      small = tid2;
    }
    else
    {
      large = tid2;
      small = tid1;
    }

    // MCOL-5539. If the current large table involved in the previous join and it was not merged into
    // "single large side multiple small sides" optimization, it should be on a small side,
    // because it represents a intermediate join result and its rowgroup could be a combination of multiple
    // rowgroups, therefore it should be sent to BPP as a small side, we cannot read it from disk.
    if (find(joinedTable.begin(), joinedTable.end(), large) != joinedTable.end() &&
        joinStepMap[small].second > 0)
    {
      std::swap(large, small);
    }

    updateJoinSides(small, large, joinInfoMap, smallSides, tableInfoMap, jobInfo);

    // This is a table for multiple join edges, always a stream table.
    // If `largest` table is equal to the current `large` table - it's an umstream table.
    if (joinStepMap[large].second > 2 || large == largest)
      umstream = true;

    if (find(joinedTable.begin(), joinedTable.end(), small) == joinedTable.end())
      joinedTable.push_back(small);

    smallSideTid.insert(small);

    // merge in the next step if the large side is the same
    for (uint64_t ns = js + 1; ns < joins.size(); js++, ns++)
    {
      // Check if FE needs table in previous smallsides.
      if (needsFeForSmallSides(jobInfo, joins, smallSideTid, ns))
      {
        // Mark as `umstream` to prevent an second type of merge optimization, when CS merges smallside into
        // current `TupleHashJoinStep`.
        umstream = true;
        break;
      }

      uint32_t tid1 = joins[ns].fTid1;
      uint32_t tid2 = joins[ns].fTid2;
      uint32_t small = (uint32_t)-1;

      if ((tid1 == large) && ((joinStepMap[tid1].second > joinStepMap[tid2].second) ||
                              (joinStepMap[tid1].second == joinStepMap[tid2].second &&
                               jobInfo.tableSize[tid1] >= jobInfo.tableSize[tid2])))
      {
        small = tid2;
      }
      else if ((tid2 == large) && ((joinStepMap[tid2].second > joinStepMap[tid1].second) ||
                                   (joinStepMap[tid2].second == joinStepMap[tid1].second &&
                                    jobInfo.tableSize[tid2] >= jobInfo.tableSize[tid1])))
      {
        small = tid1;
      }
      else
      {
        break;
      }

      updateJoinSides(small, large, joinInfoMap, smallSides, tableInfoMap, jobInfo);
      lastJoinId = joins[ns].fJoinId;

      if (find(joinedTable.begin(), joinedTable.end(), small) == joinedTable.end())
        joinedTable.push_back(small);

      smallSideTid.insert(small);
    }

    joinedTable.push_back(large);

    SJSTEP spjs = joinStepMap[large].first;
    bps = dynamic_cast<BatchPrimitive*>(spjs.get());
    tsas = dynamic_cast<SubAdapterStep*>(spjs.get());
    thjs = dynamic_cast<TupleHashJoinStep*>(spjs.get());

    if (!bps && !thjs && !tsas)
    {
      if (dynamic_cast<SubQueryStep*>(spjs.get()))
        throw IDBExcept(ERR_NON_SUPPORT_SUB_QUERY_TYPE);

      throw runtime_error("Not supported join.");
    }

    size_t startPos = 0;  // start point to add new smallsides
    RowGroup largeSideRG = joinInfoMap[large]->fRowGroup;

    if (thjs && thjs->tokenJoin() == large)
      largeSideRG = thjs->getLargeRowGroup();

    // get info to config the TupleHashjoin
    vector<string> traces;
    vector<string> tableNames;
    DataListVec smallSideDLs;
    vector<RowGroup> smallSideRGs;
    vector<JoinType> jointypes;
    vector<bool> typeless;
    vector<vector<uint32_t>> smallKeyIndices;
    vector<vector<uint32_t>> largeKeyIndices;

    // bug5764, make sure semi joins acting as filter is after outer join.
    {
      // the inner table filters have to be performed after outer join
      vector<uint64_t> semijoins;
      vector<uint64_t> smallouts;

      for (size_t i = 0; i < smallSides.size(); i++)
      {
        // find the the small-outer and semi-join joins
        JoinType jt = smallSides[i]->fJoinData.fTypes[0];

        if (jt & SMALLOUTER)
          smallouts.push_back(i);
        else if (jt & (SEMI | ANTI | SCALAR | CORRELATED))
          semijoins.push_back(i);
      }

      // check the join order, re-arrange if necessary
      if (smallouts.size() > 0 && semijoins.size() > 0)
      {
        uint64_t lastSmallOut = smallouts.back();
        uint64_t lastSemijoin = semijoins.back();

        if (lastSmallOut > lastSemijoin)
        {
          vector<SP_JoinInfo> temp1;
          vector<SP_JoinInfo> temp2;
          size_t j = 0;

          for (size_t i = 0; i < smallSides.size(); i++)
          {
            if (j < semijoins.size() && i == semijoins[j])
            {
              temp1.push_back(smallSides[i]);
              j++;
            }
            else
            {
              temp2.push_back(smallSides[i]);
            }

            if (i == lastSmallOut)
              temp2.insert(temp2.end(), temp1.begin(), temp1.end());
          }

          smallSides = temp2;
        }
      }
    }

    uint32_t smallSideIndex = 0;
    // Join id to table id.
    std::map<int64_t, uint32_t> joinIdIndexMap;
    for (vector<SP_JoinInfo>::iterator i = smallSides.begin(); i != smallSides.end(); i++, smallSideIndex++)
    {
      JoinInfo* info = i->get();
      smallSideDLs.push_back(info->fDl);
      smallSideRGs.push_back(info->fRowGroup);
      jointypes.push_back(info->fJoinData.fTypes[0]);

      vector<uint32_t> smallIndices;
      vector<uint32_t> largeIndices;
      const vector<uint32_t>& keys1 = info->fJoinData.fLeftKeys;
      const vector<uint32_t>& keys2 = info->fJoinData.fRightKeys;
      vector<uint32_t>::const_iterator k1 = keys1.begin();
      vector<uint32_t>::const_iterator k2 = keys2.begin();
      uint32_t stid = getTableKey(jobInfo, *k1);
      tableNames.push_back(jobInfo.keyInfo->tupleKeyVec[stid].fTable);

      for (; k1 != keys1.end(); ++k1, ++k2)
      {
        smallIndices.push_back(getKeyIndex(*k1, info->fRowGroup));
        largeIndices.push_back(getKeyIndex(*k2, largeSideRG));
      }

      // Try to restore `circular join edge` if possible.
      tryToRestoreJoinEdges(jobInfo, info, largeSideRG, smallIndices, largeIndices, traces, joinIdIndexMap,
                            smallSideIndex);
      typeless.push_back(info->fJoinData.fTypeless);
      smallKeyIndices.push_back(smallIndices);
      largeKeyIndices.push_back(largeIndices);

      if (jobInfo.trace)
      {
        // small side column
        ostringstream smallKey, smallIndex;
        string alias1 = jobInfo.keyInfo->keyName[getTableKey(jobInfo, keys1.front())];
        smallKey << alias1 << "-";

        for (k1 = keys1.begin(); k1 != keys1.end(); ++k1)
        {
          CalpontSystemCatalog::OID oid1 = jobInfo.keyInfo->tupleKeyVec[*k1].fId;
          CalpontSystemCatalog::TableColName tcn1 = jobInfo.csc->colName(oid1);
          smallKey << "(" << tcn1.column << ":" << oid1 << ":" << *k1 << ")";
          smallIndex << " " << getKeyIndex(*k1, info->fRowGroup);
        }

        // large side column
        ostringstream largeKey, largeIndex;
        string alias2 = jobInfo.keyInfo->keyName[getTableKey(jobInfo, keys2.front())];
        largeKey << alias2 << "-";

        for (k2 = keys2.begin(); k2 != keys2.end(); ++k2)
        {
          CalpontSystemCatalog::OID oid2 = jobInfo.keyInfo->tupleKeyVec[*k2].fId;
          CalpontSystemCatalog::TableColName tcn2 = jobInfo.csc->colName(oid2);
          largeKey << "(" << tcn2.column << ":" << oid2 << ":" << *k2 << ")";
          largeIndex << " " << getKeyIndex(*k2, largeSideRG);
        }

        ostringstream oss;
        oss << smallKey.str() << " join on " << largeKey.str()
            << " joinType: " << info->fJoinData.fTypes.front() << "("
            << joinTypeToString(info->fJoinData.fTypes.front()) << ")"
            << (info->fJoinData.fTypeless ? " " : " !") << "typeless" << endl;
        oss << "smallSideIndex-largeSideIndex :" << smallIndex.str() << " --" << largeIndex.str() << endl;
        oss << "small side RG" << endl << info->fRowGroup.toString() << endl;
        traces.push_back(oss.str());
      }
    }

    if (jobInfo.trace)
    {
      ostringstream oss;
      oss << "large side RG" << endl << largeSideRG.toString() << endl;
      traces.push_back(oss.str());
    }

    if (bps || tsas || umstream || (thjs && joinStepMap[large].second < 1))
    {
      thjs = new TupleHashJoinStep(jobInfo);
      thjs->tableOid1(smallSides[0]->fTableOid);
      thjs->tableOid2(tableInfoMap[large].fTableOid);
      thjs->alias1(smallSides[0]->fAlias);
      thjs->alias2(tableInfoMap[large].fAlias);
      thjs->view1(smallSides[0]->fView);
      thjs->view2(tableInfoMap[large].fView);
      thjs->schema1(smallSides[0]->fSchema);
      thjs->schema2(tableInfoMap[large].fSchema);
      thjs->setLargeSideBPS(bps);
      thjs->joinId(lastJoinId);

      if (dynamic_cast<TupleBPS*>(bps) != NULL)
        bps->incWaitToRunStepCnt();

      spjs.reset(thjs);

      JobStepAssociation inJsa;
      inJsa.outAdd(smallSideDLs, 0);
      inJsa.outAdd(joinInfoMap[large]->fDl);
      thjs->inputAssociation(inJsa);
      thjs->setLargeSideDLIndex(inJsa.outSize() - 1);

      JobStepAssociation outJsa;
      AnyDataListSPtr spdl(new AnyDataList());
      RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
      spdl->rowGroupDL(dl);
      dl->OID(large);
      outJsa.outAdd(spdl);
      thjs->outputAssociation(outJsa);
      thjs->configSmallSideRG(smallSideRGs, tableNames);
      thjs->configLargeSideRG(joinInfoMap[large]->fRowGroup);
      thjs->configJoinKeyIndex(jointypes, typeless, smallKeyIndices, largeKeyIndices);

      tableInfoMap[large].fQuerySteps.push_back(spjs);
      tableInfoMap[large].fDl = spdl;
    }
    else  // thjs && joinStepMap[large].second >= 1
    {
      JobStepAssociation inJsa = thjs->inputAssociation();

      if (inJsa.outSize() < 2)
        throw runtime_error("Not enough input to a hashjoin.");

      startPos = inJsa.outSize() - 1;
      inJsa.outAdd(smallSideDLs, startPos);
      thjs->inputAssociation(inJsa);
      thjs->setLargeSideDLIndex(inJsa.outSize() - 1);
      thjs->addSmallSideRG(smallSideRGs, tableNames);
      thjs->addJoinKeyIndex(jointypes, typeless, smallKeyIndices, largeKeyIndices);
    }

    RowGroup rg;
    set<uint32_t>& tableSet = tableInfoMap[large].fJoinedTables;
    constructJoinedRowGroup(rg, tableSet, tableInfoMap, jobInfo);
    thjs->setOutputRowGroup(rg);
    tableInfoMap[large].fRowGroup = rg;
    tableSet.insert(large);

    if (jobInfo.trace)
    {
      cout << boldStart << "\n====== join info ======\n" << boldStop;

      for (vector<string>::iterator t = traces.begin(); t != traces.end(); ++t)
        cout << *t;

      cout << "RowGroup join result: " << endl << rg.toString() << endl << endl;
    }

    // The map for in clause filter.
    for (size_t i = 0; i < smallSides.size(); i++)
    {
      if (smallSides[i]->fJoinData.fJoinId != 0)
        joinIdIndexMap[smallSides[i]->fJoinData.fJoinId] = i;
    }

    // check if any cross-table expressions can be evaluated after the join
    JobStepVector readyExpSteps;
    JobStepVector& expSteps = jobInfo.crossTableExpressions;
    JobStepVector::iterator eit = expSteps.begin();

    while (eit != expSteps.end())
    {
      ExpressionStep* exp = dynamic_cast<ExpressionStep*>(eit->get());

      if (exp == NULL)
        throw runtime_error("Not an expression.");

      if (exp->functionJoin())
      {
        eit++;
        continue;  // done as join
      }

      const vector<uint32_t>& tables = exp->tableKeys();
      uint64_t i = 0;

      for (; i < tables.size(); i++)
      {
        if (tableInfoMap[large].fJoinedTables.find(tables[i]) == tableInfoMap[large].fJoinedTables.end())
          break;
      }

      // all tables for this expression are joined
      bool ready = (tables.size() == i);

      // for on clause condition, need check join ID
      if (ready && exp->associatedJoinId() != 0)
      {
        auto x = joinIdIndexMap.find(exp->associatedJoinId());
        ready = (x != joinIdIndexMap.end());
      }

      if (ready)
      {
        readyExpSteps.push_back(*eit);
        eit = expSteps.erase(eit);
      }
      else
      {
        eit++;
      }
    }

    // if last join step, handle the delayed outer join filters
    if (js == lastJoin && jobInfo.outerJoinExpressions.size() > 0)
      readyExpSteps.insert(readyExpSteps.end(), jobInfo.outerJoinExpressions.begin(),
                           jobInfo.outerJoinExpressions.end());

    // check additional compares for semi-join
    if (readyExpSteps.size() > 0)
    {
      map<uint32_t, uint32_t> keyToIndexMap;  // map keys to the indices in the RG

      const auto& rowGroupKeys = rg.getKeys();
      for (uint64_t i = 0, e = rowGroupKeys.size(); i < e; ++i)
        keyToIndexMap.insert(make_pair(rowGroupKeys[i], i));

      // tables have additional comparisons
      map<uint32_t, uint32_t> correlateTables;     // index in thjs
      map<uint32_t, ParseTree*> correlateCompare;  // expression

      for (uint32_t i = 0; i != smallSides.size(); i++)
      {
        if ((jointypes[i] & SEMI) || (jointypes[i] & ANTI) || (jointypes[i] & SCALAR))
        {
          uint32_t tid =
              getTableKey(jobInfo, smallSides[i]->fTableOid, smallSides[i]->fAlias, smallSides[i]->fSchema,
                          smallSides[i]->fView, smallSides[i]->fPartitions);
          correlateTables[tid] = i;
          correlateCompare[tid] = NULL;
        }
      }

      if (correlateTables.size() > 0)
      {
        // separate additional compare for each table pair
        JobStepVector::iterator eit = readyExpSteps.begin();

        while (eit != readyExpSteps.end())
        {
          ExpressionStep* e = dynamic_cast<ExpressionStep*>(eit->get());

          if (e->selectFilter())
          {
            // @bug3780, leave select filter to normal expression
            eit++;
            continue;
          }

          const vector<uint32_t>& tables = e->tableKeys();
          auto j = correlateTables.end();

          for (uint32_t i = 0; i < tables.size(); i++)
          {
            j = correlateTables.find(tables[i]);

            if (j != correlateTables.end())
              break;
          }

          if (j == correlateTables.end())
          {
            eit++;
            continue;
          }

          // map the input column index
          e->updateInputIndex(keyToIndexMap, jobInfo);
          ParseTree* pt = correlateCompare[j->first];

          if (pt == NULL)
          {
            // first expression
            pt = new ParseTree;
            pt->copyTree(*(e->expressionFilter()));
          }
          else
          {
            // combine the expressions
            ParseTree* left = pt;
            ParseTree* right = new ParseTree;
            right->copyTree(*(e->expressionFilter()));
            pt = new ParseTree(new LogicOperator("and"));
            pt->left(left);
            pt->right(right);
          }

          correlateCompare[j->first] = pt;
          eit = readyExpSteps.erase(eit);
        }

        auto k = correlateTables.begin();

        while (k != correlateTables.end())
        {
          ParseTree* pt = correlateCompare[k->first];

          if (pt != NULL)
          {
            boost::shared_ptr<ParseTree> sppt(pt);
            thjs->addJoinFilter(sppt, startPos + k->second);
          }

          k++;
        }

        thjs->setJoinFilterInputRG(rg);
      }

      // normal expression if any
      if (readyExpSteps.size() > 0)
      {
        // add the expression steps in where clause can be solved by this join to bps
        ParseTree* pt = NULL;
        JobStepVector::iterator eit = readyExpSteps.begin();

        for (; eit != readyExpSteps.end(); eit++)
        {
          // map the input column index
          ExpressionStep* e = dynamic_cast<ExpressionStep*>(eit->get());
          e->updateInputIndex(keyToIndexMap, jobInfo);

          // short circuit on clause expressions
          auto x = joinIdIndexMap.find(e->associatedJoinId());

          if (x != joinIdIndexMap.end())
          {
            ParseTree* joinFilter = new ParseTree;
            joinFilter->copyTree(*(e->expressionFilter()));
            boost::shared_ptr<ParseTree> sppt(joinFilter);
            thjs->addJoinFilter(sppt, startPos + x->second);
            thjs->setJoinFilterInputRG(rg);
            continue;
          }

          if (pt == NULL)
          {
            // first expression
            pt = new ParseTree;
            pt->copyTree(*(e->expressionFilter()));
          }
          else
          {
            // combine the expressions
            ParseTree* left = pt;
            ParseTree* right = new ParseTree;
            right->copyTree(*(e->expressionFilter()));
            pt = new ParseTree(new LogicOperator("and"));
            pt->left(left);
            pt->right(right);
          }
        }

        if (pt != NULL)
        {
          boost::shared_ptr<ParseTree> sppt(pt);
          thjs->addFcnExpGroup2(sppt);
        }
      }

      // update the fColsInExp2 and construct the output RG
      updateExp2Cols(readyExpSteps, tableInfoMap, jobInfo);
      constructJoinedRowGroup(rg, tableSet, tableInfoMap, jobInfo);

      if (thjs->hasFcnExpGroup2())
        thjs->setFE23Output(rg);
      else
        thjs->setOutputRowGroup(rg);

      tableInfoMap[large].fRowGroup = rg;

      if (jobInfo.trace)
      {
        cout << "RowGroup of " << tableInfoMap[large].fAlias << " after EXP G2: " << endl
             << rg.toString() << endl
             << endl;
      }
    }

    // update the info maps
    int l = (joinStepMap[large].second == 2) ? 2 : 0;

    if (isSemijoin)
      joinStepMap[large] = make_pair(spjs, joinStepMap[large].second);
    else
      joinStepMap[large] = make_pair(spjs, l);

    for (set<uint32_t>::iterator i = tableSet.begin(); i != tableSet.end(); i++)
    {
      joinInfoMap[*i]->fDl = tableInfoMap[large].fDl;
      joinInfoMap[*i]->fRowGroup = tableInfoMap[large].fRowGroup;

      if (*i != large)
      {
        //@bug6117, token should be done for small side tables.
        SJSTEP smallJs = joinStepMap[*i].first;
        TupleHashJoinStep* smallThjs = dynamic_cast<TupleHashJoinStep*>(smallJs.get());

        if (smallThjs && smallThjs->tokenJoin())
          smallThjs->tokenJoin(-1);

        // Set join priority for smallsides.
        joinStepMap[*i] = make_pair(spjs, l);

        // Mark joined tables, smalls and large, as a group.
        tableInfoMap[*i].fJoinedTables = tableInfoMap[large].fJoinedTables;
      }
    }

    prevLarge = large;
  }

  // Keep join order by the table last used for picking the right delivery step.
  {
    for (vector<uint32_t>::reverse_iterator i = joinedTable.rbegin(); i < joinedTable.rend(); i++)
    {
      if (find(joinOrder.begin(), joinOrder.end(), *i) == joinOrder.end())
        joinOrder.push_back(*i);
    }

    const uint64_t n = joinOrder.size();
    const uint64_t h = n / 2;
    const uint64_t e = n - 1;

    for (uint64_t i = 0; i < h; i++)
      std::swap(joinOrder[i], joinOrder[e - i]);
  }
}

inline void joinTables(JobStepVector& joinSteps, TableInfoMap& tableInfoMap, JobInfo& jobInfo,
                       vector<uint32_t>& joinOrder, const bool overrideLargeSideEstimate)
{
  uint32_t largestTable = getLargestTable(jobInfo, tableInfoMap, overrideLargeSideEstimate);

  if (jobInfo.outerOnTable.empty())
  {
    joinToLargeTable(largestTable, tableInfoMap, jobInfo, joinOrder, jobInfo.joinEdgesToRestore);
  }
  else
  {
    joinTablesInOrder(largestTable, joinSteps, tableInfoMap, jobInfo, joinOrder);
  }
}

void makeNoTableJobStep(JobStepVector& querySteps, JobStepVector& projectSteps,
                        DeliveredTableMap& deliverySteps, JobInfo& jobInfo)
{
  querySteps.clear();
  projectSteps.clear();
  deliverySteps.clear();
  querySteps.push_back(TupleConstantStep::addConstantStep(jobInfo));
  deliverySteps[CNX_VTABLE_ID] = querySteps.back();
}
}  // namespace

namespace joblist
{
void associateTupleJobSteps(JobStepVector& querySteps, JobStepVector& projectSteps,
                            DeliveredTableMap& deliverySteps, JobInfo& jobInfo,
                            const bool overrideLargeSideEstimate)
{
  if (jobInfo.trace)
  {
    const boost::shared_ptr<TupleKeyInfo>& keyInfo = jobInfo.keyInfo;
    cout << "query steps:" << endl;

    for (const auto& step : querySteps)
    {
      auto* thjs = dynamic_cast<TupleHashJoinStep*>(step.get());

      if (thjs == nullptr)
      {
        int64_t id = (step->tupleId() != (uint64_t)-1) ? step->tupleId() : -1;
        cout << typeid(step.get()).name() << ": " << step->oid() << " " << id << " "
             << (int)((id != -1) ? getTableKey(jobInfo, id) : -1) << endl;
      }
      else
      {
        int64_t id1 = (thjs->tupleId1() != (uint64_t)-1) ? thjs->tupleId1() : -1;
        int64_t id2 = (thjs->tupleId2() != (uint64_t)-1) ? thjs->tupleId2() : -1;
        cout << typeid(*thjs).name() << ": " << thjs->oid1() << " " << id1 << " "
             << (int)((id1 != -1) ? getTableKey(jobInfo, id1) : -1) << " - " << thjs->getJoinType() << " - "
             << thjs->oid2() << " " << id2 << " " << (int)((id2 != -1) ? getTableKey(jobInfo, id2) : -1)
             << endl;
      }
    }

    cout << "project steps:" << endl;

    for (const auto& prStep : projectSteps)
    {
      cout << typeid(prStep.get()).name() << ": " << prStep->oid() << " " << prStep->tupleId() << " "
           << getTableKey(jobInfo, prStep->tupleId()) << endl;
    }

    cout << "delivery steps:" << endl;

    for (const auto& [_, value] : deliverySteps)
    {
      cout << typeid(value.get()).name() << endl;
    }

    cout << "\nTable Info:  (key  oid  name alias view sub)" << endl;

    for (uint32_t i = 0; i < keyInfo->tupleKeyVec.size(); ++i)
    {
      int64_t tid = jobInfo.keyInfo->colKeyToTblKey[i];

      if (tid == i)
      {
        CalpontSystemCatalog::OID oid = keyInfo->tupleKeyVec[i].fId;
        string alias = keyInfo->tupleKeyVec[i].fTable;

        if (alias.empty())
          alias = "N/A";

        string name = keyInfo->keyName[i];

        if (name.empty())
          name = "unknown";

        string view = keyInfo->tupleKeyVec[i].fView;

        if (view.empty())
          view = "N/A";

        auto sid = keyInfo->tupleKeyVec[i].fSubId;
        cout << i << "\t" << oid << "\t" << name << "\t" << alias << "\t" << view << "\t" << hex << sid << dec
             << endl;
      }
    }

    cout << "\nTupleKey vector:  (tupleKey  oid  tableKey  name  alias view sub)" << endl;

    for (uint32_t i = 0; i < keyInfo->tupleKeyVec.size(); ++i)
    {
      CalpontSystemCatalog::OID oid = keyInfo->tupleKeyVec[i].fId;
      int64_t tid = jobInfo.keyInfo->colKeyToTblKey[i];
      string alias = keyInfo->tupleKeyVec[i].fTable;

      if (alias.empty())
        alias = "N/A";

      // Expression IDs are borrowed from systemcatalog IDs, which are not used in tuple.
      string name = keyInfo->keyName[i];

      if (keyInfo->dictOidToColOid.find(oid) != keyInfo->dictOidToColOid.end())
      {
        name += "[d]";  // indicate this is a dictionary column
      }

      if (jobInfo.keyInfo->pseudoType[i] > 0)
      {
        name += "[p]";  // indicate this is a pseudo column
      }

      if (name.empty())
      {
        name = "unknown";
      }

      string view = keyInfo->tupleKeyVec[i].fView;

      if (view.empty())
        view = "N/A";

      auto sid = keyInfo->tupleKeyVec[i].fSubId;
      cout << i << "\t" << oid << "\t" << tid << "\t" << name << "\t" << alias << "\t" << view << "\t" << hex
           << sid << dec << endl;
    }

    cout << endl;
  }

  // Strip constantbooleanquerySteps
  for (uint64_t i = 0; i < querySteps.size();)
  {
    auto* bs = dynamic_cast<TupleConstantBooleanStep*>(querySteps[i].get());
    auto* es = dynamic_cast<ExpressionStep*>(querySteps[i].get());

    if (bs != nullptr)
    {
      // cosntant step
      if (!bs->boolValue())
        jobInfo.constantFalse = true;

      querySteps.erase(querySteps.begin() + i);
    }
    else if (es != nullptr && es->tableKeys().empty())
    {
      // constant expression
      ParseTree* p = es->expressionFilter();  // filter

      if (p != nullptr)
      {
        Row r;  // dummy row

        if (!funcexp::FuncExp::instance()->evaluate(r, p))
          jobInfo.constantFalse = true;

        querySteps.erase(querySteps.begin() + i);
      }
    }
    else
    {
      i++;
    }
  }

  // @bug 2771, handle no table select query
  if (jobInfo.tableList.empty())
  {
    makeNoTableJobStep(querySteps, projectSteps, deliverySteps, jobInfo);
    return;
  }

  // Create a step vector for each table in the from clause.
  TableInfoMap tableInfoMap;

  for (uint64_t i = 0; i < jobInfo.tableList.size(); i++)
  {
    uint32_t tableUid = jobInfo.tableList[i];
    tableInfoMap[tableUid] = TableInfo();
    tableInfoMap[tableUid].fTableOid = jobInfo.keyInfo->tupleKeyVec[tableUid].fId;
    tableInfoMap[tableUid].fName = jobInfo.keyInfo->keyName[tableUid];
    tableInfoMap[tableUid].fAlias = jobInfo.keyInfo->tupleKeyVec[tableUid].fTable;
    tableInfoMap[tableUid].fView = jobInfo.keyInfo->tupleKeyVec[tableUid].fView;
    tableInfoMap[tableUid].fSchema = jobInfo.keyInfo->tupleKeyVec[tableUid].fSchema;
    tableInfoMap[tableUid].fSubId = jobInfo.keyInfo->tupleKeyVec[tableUid].fSubId;
    tableInfoMap[tableUid].fColsInColMap = jobInfo.columnMap[tableUid];
    tableInfoMap[tableUid].fPartitions = jobInfo.keyInfo->tupleKeyVec[tableUid].fPart;
  }

  // Set of the columns being projected.
  for (auto i = jobInfo.pjColList.begin(); i != jobInfo.pjColList.end(); i++)
    jobInfo.returnColSet.insert(i->key);

  // double check if the function join canditates are still there.
  JobStepVector steps = querySteps;

  for (int64_t i = jobInfo.functionJoins.size() - 1; i >= 0; i--)
  {
    bool exist = false;

    for (auto j = steps.begin(); j != steps.end() && !exist; ++j)
    {
      if (jobInfo.functionJoins[i] == j->get())
        exist = true;
    }

    if (!exist)
      jobInfo.functionJoins.erase(jobInfo.functionJoins.begin() + i);
  }

  // Concatenate query and project steps
  steps.insert(steps.end(), projectSteps.begin(), projectSteps.end());

  // Make sure each query step has an output DL
  // This is necessary for toString() method on most steps
  for (auto& step : steps)
  {
    // if (dynamic_cast<OrDelimiter*>(it->get()))
    //	continue;

    if (step->outputAssociation().outSize() == 0)
    {
      JobStepAssociation jsa;
      AnyDataListSPtr adl(new AnyDataList());
      auto* dl = new RowGroupDL(1, jobInfo.fifoSize);
      dl->OID(step->oid());
      adl->rowGroupDL(dl);
      jsa.outAdd(adl);
      step->outputAssociation(jsa);
    }
  }

  // Populate the TableInfo map with the job steps keyed by table ID.
  JobStepVector joinSteps;
  JobStepVector& expSteps = jobInfo.crossTableExpressions;
  auto it = querySteps.begin();
  auto end = querySteps.end();

  while (it != end)
  {
    // Separate table joins from other predicates.
    auto* thjs = dynamic_cast<TupleHashJoinStep*>(it->get());
    auto* exps = dynamic_cast<ExpressionStep*>(it->get());
    auto* subs = dynamic_cast<SubAdapterStep*>(it->get());

    if (thjs && thjs->tupleId1() != thjs->tupleId2())
    {
      // simple column and constant column semi join
      if (thjs->tableOid2() == 0 && thjs->schema2().empty())
      {
        jobInfo.correlateSteps.push_back(*it++);
        continue;
      }

      // check correlated join step
      JoinType joinType = thjs->getJoinType();

      if (joinType & CORRELATED)
      {
        // one of the tables is in outer query
        jobInfo.correlateSteps.push_back(*it++);
        continue;
      }

      // Save the join topology.
      uint32_t key1 = thjs->tupleId1();
      uint32_t key2 = thjs->tupleId2();
      uint32_t tid1 = getTableKey(jobInfo, key1);
      uint32_t tid2 = getTableKey(jobInfo, key2);

      if (thjs->dictOid1() > 0)
        key1 = jobInfo.keyInfo->dictKeyMap[key1];

      if (thjs->dictOid2() > 0)
        key2 = jobInfo.keyInfo->dictKeyMap[key2];

      // not correlated
      joinSteps.push_back(*it);
      tableInfoMap[tid1].fJoinKeys.push_back(key1);
      tableInfoMap[tid2].fJoinKeys.push_back(key2);

      // save the function join expressions
      boost::shared_ptr<FunctionJoinInfo> fji = thjs->funcJoinInfo();

      if (fji)
      {
        if (fji->fStep[0])
        {
          tableInfoMap[tid1].fFuncJoinExps.push_back(fji->fStep[0]);
          vector<uint32_t>& cols = tableInfoMap[tid1].fColsInFuncJoin;
          cols.insert(cols.end(), fji->fColumnKeys[0].begin(), fji->fColumnKeys[0].end());
        }

        if (fji->fStep[1])
        {
          tableInfoMap[tid2].fFuncJoinExps.push_back(fji->fStep[1]);
          vector<uint32_t>& cols = tableInfoMap[tid2].fColsInFuncJoin;
          cols.insert(cols.end(), fji->fColumnKeys[1].begin(), fji->fColumnKeys[1].end());
        }
      }

      // keep a join map
      pair<uint32_t, uint32_t> tablePair(tid1, tid2);
      auto m1 = jobInfo.tableJoinMap.find(tablePair);
      auto m2 = jobInfo.tableJoinMap.end();

      if (m1 == jobInfo.tableJoinMap.end())
      {
        tableInfoMap[tid1].fAdjacentList.push_back(tid2);
        tableInfoMap[tid2].fAdjacentList.push_back(tid1);

        m1 = jobInfo.tableJoinMap.insert(m1, make_pair(make_pair(tid1, tid2), JoinData()));
        m2 = jobInfo.tableJoinMap.insert(m1, make_pair(make_pair(tid2, tid1), JoinData()));

        TupleInfo ti1(getTupleInfo(key1, jobInfo));
        TupleInfo ti2(getTupleInfo(key2, jobInfo));

        if (ti1.width > 8 || ti2.width > 8)
        {
          if (ti1.dtype == execplan::CalpontSystemCatalog::LONGDOUBLE ||
              ti2.dtype == execplan::CalpontSystemCatalog::LONGDOUBLE)
          {
            m1->second.fTypeless = m2->second.fTypeless = false;
          }
          else
          {
            m1->second.fTypeless = m2->second.fTypeless = true;
          }
        }
        else
        {
          m1->second.fTypeless = m2->second.fTypeless = isCharType(ti1.dtype) || isCharType(ti2.dtype);
        }
      }
      else
      {
        m2 = jobInfo.tableJoinMap.find(make_pair(tid2, tid1));
        m1->second.fTypeless = m2->second.fTypeless = true;
      }

      if (m1 == jobInfo.tableJoinMap.end() || m2 == jobInfo.tableJoinMap.end())
        throw runtime_error("Bad table map.");

      // Keep a map of the join (table, key) pairs
      m1->second.fLeftKeys.push_back(key1);
      m1->second.fRightKeys.push_back(key2);

      m2->second.fLeftKeys.push_back(key2);
      m2->second.fRightKeys.push_back(key1);

      // Keep a map of the join type between the keys.
      // OUTER join and SEMI/ANTI join are mutually exclusive.
      if (joinType == LEFTOUTER)
      {
        m1->second.fTypes.push_back(SMALLOUTER);
        m2->second.fTypes.push_back(LARGEOUTER);
        jobInfo.outerOnTable.insert(tid2);
      }
      else if (joinType == RIGHTOUTER)
      {
        m1->second.fTypes.push_back(LARGEOUTER);
        m2->second.fTypes.push_back(SMALLOUTER);
        jobInfo.outerOnTable.insert(tid1);
      }
      else if ((joinType & SEMI) &&
               ((joinType & LEFTOUTER) == LEFTOUTER || (joinType & RIGHTOUTER) == RIGHTOUTER))
      {
        // @bug3998, DML UPDATE borrows "SEMI" flag,
        // allowing SEMI and LARGEOUTER combination to support update with outer join.
        if ((joinType & LEFTOUTER) == LEFTOUTER)
        {
          joinType ^= LEFTOUTER;
          m1->second.fTypes.push_back(joinType);
          m2->second.fTypes.push_back(joinType | LARGEOUTER);
          jobInfo.outerOnTable.insert(tid2);
        }
        else
        {
          joinType ^= RIGHTOUTER;
          m1->second.fTypes.push_back(joinType | LARGEOUTER);
          m2->second.fTypes.push_back(joinType);
          jobInfo.outerOnTable.insert(tid1);
        }
      }
      else
      {
        m1->second.fTypes.push_back(joinType);
        m2->second.fTypes.push_back(joinType);

        if (joinType == INNER)
        {
          jobInfo.innerOnTable.insert(tid1);
          jobInfo.innerOnTable.insert(tid2);
        }
      }

      // need id to keep the join order
      m1->second.fJoinId = m2->second.fJoinId = thjs->joinId();
    }
    // Separate the expressions
    else if (exps && !subs)
    {
      const vector<uint32_t>& tables = exps->tableKeys();
      const vector<uint32_t>& columns = exps->columnKeys();
      bool tableInOuterQuery = false;
      set<uint32_t> tableSet;  // involved unique tables

      for (unsigned int table : tables)
      {
        if (find(jobInfo.tableList.begin(), jobInfo.tableList.end(), table) != jobInfo.tableList.end())
          tableSet.insert(table);
        else
          tableInOuterQuery = true;
      }

      if (tableInOuterQuery)
      {
        // all columns in subquery scope to be projected
        for (uint64_t i = 0; i < tables.size(); ++i)
        {
          // outer-query columns
          if (tableSet.find(tables[i]) == tableSet.end())
            continue;

          // subquery columns
          uint32_t c = columns[i];

          if (jobInfo.returnColSet.find(c) == jobInfo.returnColSet.end())
          {
            tableInfoMap[tables[i]].fProjectCols.push_back(c);
            jobInfo.pjColList.push_back(getTupleInfo(c, jobInfo));
            jobInfo.returnColSet.insert(c);
            const auto* sc = dynamic_cast<const SimpleColumn*>(exps->columns()[i]);

            if (sc)
              jobInfo.deliveredCols.emplace_back(sc->clone());
          }
        }

        jobInfo.correlateSteps.push_back(*it++);
        continue;
      }

      // is the expression cross tables?
      if (tableSet.size() == 1 && exps->associatedJoinId() == 0)
      {
        // single table and not in join on clause
        uint32_t tid = tables[0];

        for (unsigned int column : columns)
          tableInfoMap[tid].fColsInExp1.push_back(column);

        tableInfoMap[tid].fOneTableExpSteps.push_back(*it);
      }
      else
      {
        // WORKAROUND for limitation on join with filter
        if (exps->associatedJoinId() != 0)
        {
          for (uint64_t i = 0; i < exps->columns().size(); ++i)
          {
            jobInfo.joinFeTableMap[exps->associatedJoinId()].insert(tables[i]);
          }
        }

        // resolve after join: cross table or on clause conditions
        for (unsigned int cid : columns)
        {
          uint32_t tid = getTableKey(jobInfo, cid);
          tableInfoMap[tid].fColsInExp2.push_back(cid);
        }

        expSteps.push_back(*it);
      }
    }
    // Separate the other steps by unique ID.
    else
    {
      uint32_t tid = -1;
      uint64_t cid = (*it)->tupleId();

      if (cid != (uint64_t)-1)
        tid = getTableKey(jobInfo, (*it)->tupleId());
      else
        tid = getTableKey(jobInfo, it->get());

      if (find(jobInfo.tableList.begin(), jobInfo.tableList.end(), tid) != jobInfo.tableList.end())
      {
        tableInfoMap[tid].fQuerySteps.push_back(*it);
      }
      else
      {
        jobInfo.correlateSteps.push_back(*it);
      }
    }

    it++;
  }

  // @bug2634, delay isNull filter on outerjoin key
  // @bug5374, delay predicates for outerjoin
  outjoinPredicateAdjust(tableInfoMap, jobInfo);

  // @bug4021, make sure there is real column to scan
  for (auto it = tableInfoMap.begin(); it != tableInfoMap.end(); it++)
  {
    uint32_t tableUid = it->first;

    if (jobInfo.pseudoColTable.find(tableUid) == jobInfo.pseudoColTable.end())
      continue;

    JobStepVector& steps = tableInfoMap[tableUid].fQuerySteps;
    auto s = steps.begin();
    auto p = steps.end();

    for (; s != steps.end(); s++)
    {
      if (typeid(*(s->get())) == typeid(pColScanStep) || typeid(*(s->get())) == typeid(pColStep))
        break;

      // @bug5893, iterator to the first pseudocolumn
      if (typeid(*(s->get())) == typeid(PseudoColStep) && p == steps.end())
        p = s;
    }

    if (s == steps.end())
    {
      auto t = jobInfo.tableColMap.find(tableUid);

      if (t == jobInfo.tableColMap.end())
      {
        string msg = jobInfo.keyInfo->tupleKeyToName[tableUid];
        msg += " has no column in column map.";
        throw runtime_error(msg);
      }

      auto* sc = dynamic_cast<SimpleColumn*>(t->second.get());
      CalpontSystemCatalog::OID oid = sc->oid();
      CalpontSystemCatalog::OID tblOid = tableOid(sc, jobInfo.csc);
      CalpontSystemCatalog::ColType ct = sc->colType();
      string alias(extractTableAlias(sc));
      SJSTEP sjs(new pColScanStep(oid, tblOid, ct, jobInfo));
      sjs->alias(alias);
      sjs->view(sc->viewName());
      sjs->schema(sc->schemaName());
      sjs->name(sc->columnName());
      TupleInfo ti(setTupleInfo(ct, oid, jobInfo, tblOid, sc, alias));
      sjs->tupleId(ti.key);
      steps.insert(steps.begin(), sjs);

      if (isDictCol(ct) && jobInfo.tokenOnly.find(ti.key) == jobInfo.tokenOnly.end())
        jobInfo.tokenOnly[ti.key] = true;
    }
    else if (s > p)
    {
      // @bug5893, make sure a pcol is in front of any pseudo step.
      SJSTEP t = *s;
      *s = *p;
      *p = t;
    }
  }

  // @bug3767, error out scalar subquery with aggregation and correlated additional comparison.
  if (jobInfo.hasAggregation && (!jobInfo.correlateSteps.empty()))
  {
    // expression filter
    ExpressionStep* exp = nullptr;

    for (it = jobInfo.correlateSteps.begin(); it != jobInfo.correlateSteps.end(); it++)
    {
      if (((exp = dynamic_cast<ExpressionStep*>(it->get())) != nullptr) && (!exp->functionJoin()))
        break;

      exp = nullptr;
    }

    // correlated join step
    TupleHashJoinStep* thjs = nullptr;

    for (it = jobInfo.correlateSteps.begin(); it != jobInfo.correlateSteps.end(); it++)
    {
      if ((thjs = dynamic_cast<TupleHashJoinStep*>(it->get())) != nullptr)
        break;
    }

    // @bug5202, error out not equal correlation and aggregation in subquery.
    if (exp && thjs && (thjs->getJoinType() & CORRELATED))
      throw IDBExcept(IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_NEQ_AGG_SUB),
                      ERR_NON_SUPPORT_NEQ_AGG_SUB);
  }

  it = projectSteps.begin();
  end = projectSteps.end();

  while (it != end)
  {
    uint32_t tid = getTableKey(jobInfo, (*it)->tupleId());
    tableInfoMap[tid].fProjectSteps.push_back(*it);
    tableInfoMap[tid].fProjectCols.push_back((*it)->tupleId());
    it++;
  }

  for (auto j = jobInfo.pjColList.begin(); j != jobInfo.pjColList.end(); j++)
  {
    if (jobInfo.keyInfo->tupleKeyVec[j->tkey].fId == CNX_EXP_TABLE_ID)
      continue;

    vector<uint32_t>& projectCols = tableInfoMap[j->tkey].fProjectCols;

    if (find(projectCols.begin(), projectCols.end(), j->key) == projectCols.end())
      projectCols.push_back(j->key);
  }

  JobStepVector& retExp = jobInfo.returnedExpressions;

  for (it = retExp.begin(); it != retExp.end(); ++it)
  {
    auto* exp = dynamic_cast<ExpressionStep*>(it->get());

    if (exp == nullptr)
      throw runtime_error("Not an expression.");

    for (uint64_t i = 0; i < exp->columnKeys().size(); ++i)
    {
      tableInfoMap[exp->tableKeys()[i]].fColsInRetExp.push_back(exp->columnKeys()[i]);
    }
  }

  // reset all step vector
  querySteps.clear();
  projectSteps.clear();
  deliverySteps.clear();

  // Check if the tables and joins can be used to construct a spanning tree.
  spanningTreeCheck(tableInfoMap, joinSteps, jobInfo);

  // 1. combine job steps for each table
  TableInfoMap::iterator mit;

  for (mit = tableInfoMap.begin(); mit != tableInfoMap.end(); mit++)
    if (!combineJobStepsByTable(mit, jobInfo))
      throw runtime_error("combineJobStepsByTable failed.");

  // 2. join the combined steps together to form the spanning tree
  vector<uint32_t> joinOrder;
  joinTables(joinSteps, tableInfoMap, jobInfo, joinOrder, overrideLargeSideEstimate);

  // 3. put the steps together
  for (uint32_t i : joinOrder)
    querySteps.insert(querySteps.end(), tableInfoMap[i].fQuerySteps.begin(),
                      tableInfoMap[i].fQuerySteps.end());

  adjustLastStep(querySteps, deliverySteps, jobInfo);  // to match the select clause
}

SJSTEP unionQueries(JobStepVector& queries, uint64_t distinctUnionNum, JobInfo& jobInfo)
{
  vector<RowGroup> inputRGs;
  vector<bool> distinct;
  uint64_t colCount = jobInfo.deliveredCols.size();

  vector<uint32_t> oids;
  vector<uint32_t> keys;
  vector<uint32_t> scale;
  vector<uint32_t> precision;
  vector<uint32_t> width;
  vector<CalpontSystemCatalog::ColDataType> types;
  vector<uint32_t> csNums;
  JobStepAssociation jsaToUnion;

  // bug4388, share code with connector for column type coversion
  vector<vector<CalpontSystemCatalog::ColType>> queryColTypes;

  for (uint64_t j = 0; j < colCount; ++j)
    queryColTypes.push_back(vector<CalpontSystemCatalog::ColType>(queries.size()));

  for (uint64_t i = 0; i < queries.size(); i++)
  {
    SJSTEP& spjs = queries[i];
    TupleDeliveryStep* tds = dynamic_cast<TupleDeliveryStep*>(spjs.get());

    if (tds == NULL)
    {
      throw runtime_error("Not a deliverable step.");
    }

    const RowGroup& rg = tds->getDeliveredRowGroup();
    inputRGs.push_back(rg);

    const vector<uint32_t>& scaleIn = rg.getScale();
    const vector<uint32_t>& precisionIn = rg.getPrecision();
    const vector<CalpontSystemCatalog::ColDataType>& typesIn = rg.getColTypes();
    const vector<uint32_t>& csNumsIn = rg.getCharsetNumbers();

    for (uint64_t j = 0; j < colCount; ++j)
    {
      queryColTypes[j][i].colDataType = typesIn[j];
      queryColTypes[j][i].charsetNumber = csNumsIn[j];
      queryColTypes[j][i].scale = scaleIn[j];
      queryColTypes[j][i].precision = precisionIn[j];
      queryColTypes[j][i].colWidth = rg.getColumnWidth(j);
    }

    if (i == 0)
    {
      const vector<uint32_t>& oidsIn = rg.getOIDs();
      const vector<uint32_t>& keysIn = rg.getKeys();
      oids.insert(oids.end(), oidsIn.begin(), oidsIn.begin() + colCount);
      keys.insert(keys.end(), keysIn.begin(), keysIn.begin() + colCount);
    }

    // if all union types are UNION_ALL, distinctUnionNum is 0.
    distinct.push_back(distinctUnionNum > i);

    AnyDataListSPtr spdl(new AnyDataList());
    RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
    spdl->rowGroupDL(dl);
    dl->OID(CNX_VTABLE_ID);
    JobStepAssociation jsa;
    jsa.outAdd(spdl);
    spjs->outputAssociation(jsa);
    jsaToUnion.outAdd(spdl);
  }

  AnyDataListSPtr spdl(new AnyDataList());
  RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
  spdl->rowGroupDL(dl);
  dl->OID(CNX_VTABLE_ID);
  JobStepAssociation jsa;
  jsa.outAdd(spdl);
  TupleUnion* unionStep = new TupleUnion(CNX_VTABLE_ID, jobInfo);
  unionStep->inputAssociation(jsaToUnion);
  unionStep->outputAssociation(jsa);

  // This return code in the call to convertUnionColType() below would
  // always be 0. This is because convertUnionColType() is also called
  // in the connector code in getSelectPlan() which handle
  // the non-zero return code scenarios from this function call and error
  // out, in which case, the execution does not even get to ExeMgr.
  unsigned int dummyUnionedTypeRc = 0;

  // get unioned column types
  for (uint64_t j = 0; j < colCount; ++j)
  {
    CalpontSystemCatalog::ColType colType =
        CalpontSystemCatalog::ColType::convertUnionColType(queryColTypes[j], dummyUnionedTypeRc);
    types.push_back(colType.colDataType);
    csNums.push_back(colType.charsetNumber);
    scale.push_back(colType.scale);
    precision.push_back(colType.precision);
    width.push_back(colType.colWidth);
  }

  vector<uint32_t> pos;
  pos.push_back(2);

  for (uint64_t i = 0; i < oids.size(); ++i)
    pos.push_back(pos[i] + width[i]);

  unionStep->setInputRowGroups(inputRGs);
  unionStep->setDistinctFlags(distinct);
  unionStep->setOutputRowGroup(
      RowGroup(oids.size(), pos, oids, keys, types, csNums, scale, precision, jobInfo.stringTableThreshold));

  // Fix for bug 4388 adjusts the result type at connector side, this workaround is obsolete.
  // bug 3067, update the returned column types.
  // This is a workaround as the connector always uses the first query' returned columns.
  // ct.colDataType = types[i];
  // ct.scale = scale[i];
  // ct.colWidth = width[i];

  for (size_t i = 0; i < jobInfo.deliveredCols.size(); i++)
  {
    CalpontSystemCatalog::ColType ct = jobInfo.deliveredCols[i]->resultType();
    // XXX remove after connector change
    ct.colDataType = types[i];
    ct.scale = scale[i];
    ct.colWidth = width[i];

    // varchar/varbinary column width has been fudged, see fudgeWidth in jlf_common.cpp.
    if (ct.colDataType == CalpontSystemCatalog::VARCHAR)
      ct.colWidth--;
    else if (ct.colDataType == CalpontSystemCatalog::VARBINARY)
      ct.colWidth -= 2;

    jobInfo.deliveredCols[i]->resultType(ct);
  }

  if (jobInfo.trace)
  {
    cout << boldStart << "\ninput RGs: (distinct=" << distinctUnionNum << ")\n" << boldStop;

    for (vector<RowGroup>::iterator i = inputRGs.begin(); i != inputRGs.end(); i++)
      cout << i->toString() << endl << endl;

    cout << boldStart << "output RG:\n" << boldStop << unionStep->getDeliveredRowGroup().toString() << endl;
  }

  return SJSTEP(unionStep);
}

}  // namespace joblist

#ifdef __clang__
#pragma clang diagnostic pop
#endif

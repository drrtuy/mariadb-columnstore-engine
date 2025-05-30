/* Copyright (C) 2014 InfiniDB, Inc.

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

//  $Id: jlf_tuplejoblist.h 9655 2013-06-25 23:08:13Z xlou $

/** @file */

#pragma once

#include "calpontsystemcatalog.h"
#include "joblist.h"
#include "jlf_common.h"

namespace joblist
{
// data to construct and config a tuple hashjoin [used by small side table]
struct JoinInfo
{
  execplan::CalpontSystemCatalog::OID fTableOid;
  std::string fAlias;
  std::string fSchema;
  std::string fView;
  execplan::Partitions fPartitions;
  AnyDataListSPtr fDl;           // output data list
  rowgroup::RowGroup fRowGroup;  // rowgroup meta data for the data list
  // colOid and alias can be retrieved from JobInfo.tupleKeyVec using join key.

  // @bug 1495 compound join
  JoinData fJoinData;

  JoinInfo() : fTableOid(-1)
  {
  }
};
typedef boost::shared_ptr<JoinInfo> SP_JoinInfo;

// data to construct and config a token hashjoin for string access predicates
struct DictionaryScanInfo
{
  uint32_t fTokenId;             // token unique id
  AnyDataListSPtr fDl;           // data list
  rowgroup::RowGroup fRowGroup;  // rowgroup meta data for the data list

  DictionaryScanInfo() : fTokenId(-1)
  {
  }
};

// steps and joins of a table in a query
struct TableInfo
{
  execplan::CalpontSystemCatalog::OID fTableOid;
  std::string fName;
  std::string fAlias;
  std::string fSchema;
  execplan::Partitions fPartitions;
  std::string fView;
  uint64_t fSubId;
  JobStepVector fQuerySteps;
  JobStepVector fProjectSteps;
  JobStepVector fFuncJoinExps;
  JobStepVector fOneTableExpSteps;
  std::vector<uint32_t> fProjectCols;
  std::vector<uint32_t> fColsInExp1;      // 1 table expression
  std::vector<uint32_t> fColsInExp2;      // 2 or more tables in expression
  std::vector<uint32_t> fColsInRetExp;    // expression in selection/group by clause
  std::vector<uint32_t> fColsInOuter;     // delayed outer join filter
  std::vector<uint32_t> fColsInFuncJoin;  // expression for function join
  std::vector<uint32_t> fColsInColMap;    // columns in column map
  std::vector<uint32_t> fJoinKeys;
  std::vector<uint32_t> fAdjacentList;  // tables joined with
  bool fVisited;

  AnyDataListSPtr fDl;               // output data list
  rowgroup::RowGroup fRowGroup;      // output rowgroup meta data
  std::set<uint32_t> fJoinedTables;  // tables directly/indirectly joined to this table

  TableInfo() : fTableOid(-1), fVisited(false)
  {
  }
};
typedef std::map<uint32_t, TableInfo> TableInfoMap;

struct FunctionJoinInfo
{
  std::vector<uint32_t> fTableKey;
  std::vector<uint32_t> fJoinKey;
  std::vector<int32_t> fTableOid;
  std::vector<int32_t> fOid;
  std::vector<int32_t> fSequence;
  std::vector<std::string> fAlias;
  std::vector<std::string> fView;
  std::vector<std::string> fSchema;
  std::vector<execplan::Partitions> fPartitionss;
  JobStepVector fStep;
  JoinType fJoinType;
  int64_t fJoinId;
  int64_t fCorrelatedSide;
  std::vector<std::set<uint32_t>> fColumnKeys;
  std::vector<execplan::ReturnedColumn*> fExpression;

  FunctionJoinInfo() : fJoinType(0), fJoinId(0), fCorrelatedSide(0)
  {
  }
};
typedef boost::shared_ptr<FunctionJoinInfo> SP_FuncJoinInfo;
typedef std::pair<uint32_t, uint32_t> KeyPair;

// combine and associate tuple job steps
void associateTupleJobSteps(JobStepVector& querySteps, JobStepVector& projectSteps,
                            DeliveredTableMap& deliverySteps, JobInfo& jobInfo,
                            const bool overrideLargeSideEstimate);

// make BOP_OR an expression step
void orExpresssion(const execplan::Operator* op, JobInfo& jobInfo);

// union the queries and return the tuple union step
SJSTEP unionQueries(JobStepVector& queries, uint64_t distinctUnionNum, JobInfo& jobInfo);

// Used for join graph analysis.
// WHITE - node is not processed.
// GREY - node is in process.
// BLACK - node is done.
enum class JoinTableColor
{
  WHITE,
  GREY,
  BLACK
};

struct JoinTableNode
{
  JoinTableColor fTableColor;
  uint32_t fParent;
  std::vector<uint32_t> fAdjacentList;
  JoinTableNode() : fTableColor(JoinTableColor::WHITE), fParent(UINT_MAX)
  {
  }
};

using JoinGraph = std::map<uint32_t, JoinTableNode>;
using JoinEdge = std::pair<uint32_t, uint32_t>;
using JoinEdges = std::set<JoinEdge>;
using Cycle = std::vector<JoinEdge>;
using Cycles = std::vector<std::vector<JoinEdge>>;
using PostJoinFilterKeys = std::vector<std::pair<JoinEdge, std::vector<uint32_t>>>;
}  // namespace joblist

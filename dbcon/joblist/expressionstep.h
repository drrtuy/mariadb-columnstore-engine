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

//  $Id: expressionstep.h 9632 2013-06-18 22:18:20Z xlou $

/** @file
 * class ExpStep interface
 */

#pragma once

// #define NDEBUG
#include "jobstep.h"
#include "filter.h"

typedef boost::shared_ptr<SimpleColumn> SSC;

namespace execplan
{
// forward reference
class ReturnedColumn;
class SimpleColumn;
class SimpleFilter;
class WindowFunctionColumn;
};  // namespace execplan

namespace joblist
{
struct JobInfo;
struct FunctionJoinInfo;

class ExpressionStep : public JobStep
{
 public:
  // constructors
  ExpressionStep();
  explicit ExpressionStep(const JobInfo&);
  // destructor constructors
  ~ExpressionStep() override;

  // inherited methods
  void run() override;
  void join() override;
  const std::string toString() const override;

  execplan::CalpontSystemCatalog::OID oid() const override
  {
    return 0;
  }
  execplan::CalpontSystemCatalog::OID tableOid() const override
  {
    return fTableOids.empty() ? 0 : fTableOids.front();
  }
  using JobStep::alias;
  std::string alias() const override
  {
    return fAliases.empty() ? "" : fAliases.front();
  }
  using JobStep::view;
  std::string view() const override
  {
    return fViews.empty() ? "" : fViews.front();
  }
  using JobStep::partitions;
  execplan::Partitions partitions() const override
  {
    return fPartitions;
  }
  using JobStep::schema;
  std::string schema() const override
  {
    return fSchemas.empty() ? "" : fSchemas.front();
  }
  uint32_t tableKey() const
  {
    return fTableKeys.empty() ? -1 : fTableKeys.front();
  }
  uint32_t columnKey() const
  {
    return fColumnKeys.empty() ? -1 : fColumnKeys.front();
  }

  void expression(const execplan::SRCP exp, JobInfo& jobInfo);
  execplan::SRCP expression() const
  {
    return fExpression;
  }

  virtual void expressionFilter(const execplan::Filter* filter, JobInfo& jobInfo);
  virtual void expressionFilter(const execplan::ParseTree* filter, JobInfo& jobInfo);
  execplan::ParseTree* expressionFilter() const
  {
    return fExpressionFilter;
  }

  void expressionId(uint64_t eid)
  {
    fExpressionId = eid;
  }
  uint64_t expressionId() const
  {
    return fExpressionId;
  }

  const std::vector<execplan::CalpontSystemCatalog::OID>& tableOids() const
  {
    return fTableOids;
  }
  const std::vector<std::string>& aliases() const
  {
    return fAliases;
  }
  const std::vector<std::string>& views() const
  {
    return fViews;
  }
  const std::vector<execplan::Partitions>& partitionss() const
  {
    return fPartitionss;
  }
  const std::vector<std::string>& schemas() const
  {
    return fSchemas;
  }
  const std::vector<uint32_t>& tableKeys() const
  {
    return fTableKeys;
  }
  const std::vector<uint32_t>& columnKeys() const
  {
    return fColumnKeys;
  }

  std::vector<execplan::CalpontSystemCatalog::OID>& tableOids()
  {
    return fTableOids;
  }
  std::vector<std::string>& aliases()
  {
    return fAliases;
  }
  std::vector<std::string>& views()
  {
    return fViews;
  }
  std::vector<execplan::Partitions>& partitionss()
  {
    return fPartitionss;
  }
  std::vector<std::string>& schemas()
  {
    return fSchemas;
  }
  std::vector<uint32_t>& tableKeys()
  {
    return fTableKeys;
  }
  std::vector<uint32_t>& columnKeys()
  {
    return fColumnKeys;
  }

  virtual void updateInputIndex(std::map<uint32_t, uint32_t>& indexMap, const JobInfo& jobInfo);
  virtual void updateOutputIndex(std::map<uint32_t, uint32_t>& indexMap, const JobInfo& jobInfo);
  virtual void updateColumnOidAlias(JobInfo& jobInfo);

  std::vector<execplan::ReturnedColumn*>& columns()
  {
    return fColumns;
  }
  void substitute(uint64_t, const SSC&);

  void selectFilter(bool b)
  {
    fSelectFilter = b;
  }
  bool selectFilter() const
  {
    return fSelectFilter;
  }

  void associatedJoinId(uint64_t i)
  {
    fAssociatedJoinId = i;
  }
  uint64_t associatedJoinId() const
  {
    return fAssociatedJoinId;
  }

  void functionJoin(bool b)
  {
    fDoJoin = b;
  }
  bool functionJoin() const
  {
    return fDoJoin;
  }

  void virtualStep(bool b)
  {
    fVirtual = b;
  }
  bool virtualStep() const
  {
    return fVirtual;
  }

  boost::shared_ptr<FunctionJoinInfo>& functionJoinInfo()
  {
    return fFunctionJoinInfo;
  }
  void resetJoinInfo()
  {
    fFunctionJoinInfo.reset();
  }

 protected:
  virtual void addColumn(execplan::ReturnedColumn* rc, JobInfo& jobInfo);
  virtual void addFilter(execplan::ParseTree* filter, JobInfo& jobInfo);
  virtual void addSimpleFilter(execplan::SimpleFilter* sf, JobInfo& jobInfo);
  virtual void populateColumnInfo(execplan::ReturnedColumn* rc, JobInfo& jobInfo);
  virtual void populateColumnInfo(execplan::SimpleColumn* sc, JobInfo& jobInfo);
  virtual void populateColumnInfo(execplan::WindowFunctionColumn* wc, JobInfo& jobInfo);
  virtual void populateColumnInfo(execplan::AggregateColumn* ac, JobInfo& jobInfo);
  virtual void functionJoinCheck(execplan::SimpleFilter* sf, JobInfo& jobInfo);
  virtual bool parseFuncJoinColumn(ReturnedColumn* rc, JobInfo& jobInfo);

  // expression
  execplan::SRCP fExpression;
  execplan::ParseTree* fExpressionFilter;
  uint64_t fExpressionId;

  // columns accessed
  std::vector<execplan::CalpontSystemCatalog::OID> fTableOids;
  std::vector<std::string> fAliases;
  std::vector<std::string> fViews;
  std::vector<std::string> fSchemas;
  std::vector<execplan::Partitions> fPartitionss;
  std::vector<uint32_t> fTableKeys;
  std::vector<uint32_t> fColumnKeys;
  std::vector<execplan::ReturnedColumn*> fColumns;

 private:
  // disable copy constructor
  // Cannot copy fColumns, which depends on fExpressionFilter.
  ExpressionStep(const ExpressionStep&);

  // for VARBINARY, only support limited number of functions: like hex(), length(), etc.
  bool fVarBinOK;

  // @bug 3780, for select filter
  bool fSelectFilter;

  // @bug 3037, outer join with additional comparison
  uint64_t fAssociatedJoinId;

  // @bug 4531, for window function in IN/EXISTS sub-query.
  std::map<execplan::SimpleColumn*, execplan::ReturnedColumn*> fSubMap;
  std::set<SSC> fVsc;  // for substitute wc with vsc

  // @bug 3683, function join
  boost::shared_ptr<FunctionJoinInfo> fFunctionJoinInfo;
  bool fDoJoin;
  bool fVirtual;
};

}  // namespace joblist

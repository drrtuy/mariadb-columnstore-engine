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

//  $Id: subquerystep.h 6370 2010-03-18 02:58:09Z xlou $

/** @file */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

#include <boost/thread.hpp>

#include "jobstep.h"
#include "joblist.h"
#include "funcexpwrapper.h"
#include "threadnaming.h"

namespace joblist
{
struct JobInfo;

class SubQueryStep : public JobStep
{
 public:
  /** @brief SubQueryStep constructor
   */
  explicit SubQueryStep(const JobInfo&);

  /** @brief SubQueryStep destructor
   */
  ~SubQueryStep() override;

  /** @brief virtual void run method
   */
  void run() override;

  /** @brief virtual void join method
   */
  void join() override;

  /** @brief virtual void abort method
   */
  void abort() override;

  /** @brief virtual get table OID
   *  @returns OID
   */
  execplan::CalpontSystemCatalog::OID tableOid() const override
  {
    return fTableOid;
  }

  /** @brief virtual set table OID
   */
  void tableOid(const execplan::CalpontSystemCatalog::OID& id)
  {
    fTableOid = id;
  }

  /** @brief virtual output info to a string
   *  @returns string
   */
  const std::string toString() const override;

  /** @brief virtual set the output rowgroup
   */
  virtual void setOutputRowGroup(const rowgroup::RowGroup& rg)
  {
    fOutputRowGroup = rg;
  }

  /** @brief virtual get the output rowgroup
   *  @returns RowGroup
   */
  virtual const rowgroup::RowGroup& getOutputRowGroup() const
  {
    return fOutputRowGroup;
  }

  /** @brief virtual set the sub-query's joblist
   */
  virtual void subJoblist(const STJLP& sjl)
  {
    fSubJobList = sjl;
  }

  /** @brief virtual get the sub-query's joblist
   *  @returns boost::shared_ptr<TupleJobList>
   */
  virtual const STJLP& subJoblist() const
  {
    return fSubJobList;
  }

 protected:
  uint64_t fRowsReturned;

  execplan::CalpontSystemCatalog::OID fTableOid;
  std::vector<execplan::CalpontSystemCatalog::OID> fColumnOids;
  rowgroup::RowGroup fOutputRowGroup;
  STJLP fSubJobList;

  boost::scoped_ptr<boost::thread> fRunner;
};

class SubAdapterStep : public JobStep, public TupleDeliveryStep
{
 public:
  /** @brief SubAdapterStep constructor
   */
  SubAdapterStep(SJSTEP& s, const JobInfo&);

  /** @brief SubAdapterStep destructor
   */
  ~SubAdapterStep() override;

  /** @brief virtual void run method
   */
  void run() override;

  /** @brief virtual void join method
   */
  void join() override;

  /** @brief virtual void abort method
   */
  void abort() override;

  /** @brief virtual get table OID
   *  @returns OID
   */
  execplan::CalpontSystemCatalog::OID tableOid() const override
  {
    return fTableOid;
  }

  /** @brief virtual set table OID
   */
  void tableOid(const execplan::CalpontSystemCatalog::OID& id)
  {
    fTableOid = id;
  }

  /** @brief virtual output info to a string
   *  @returns string
   */
  const std::string toString() const override;

  /** @brief virtual set the output rowgroup
   */
  void setOutputRowGroup(const rowgroup::RowGroup& rg) override;

  /** @brief virtual get the output rowgroup
   *  @returns RowGroup
   */
  const rowgroup::RowGroup& getOutputRowGroup() const override
  {
    return fRowGroupOut;
  }

  /** @brief TupleDeliveryStep's pure virtual methods nextBand
   *  @returns row count
   */
  uint32_t nextBand(messageqcpp::ByteStream& bs) override;

  /** @brief Delivered Row Group
   *  @returns RowGroup
   */
  const rowgroup::RowGroup& getDeliveredRowGroup() const override
  {
    return fRowGroupDeliver;
  }

  /** @brief Turn on/off string table delivery
   */
  void deliverStringTableRowGroup(bool b) override;

  /** @brief Check useStringTable flag on delivered RowGroup
   *  @returns boolean
   */
  bool deliverStringTableRowGroup() const override;

  /** @brief set the rowgroup for FE to work on
   */
  void setFeRowGroup(const rowgroup::RowGroup& rg);

  /** @brief get the rowgroup for FE
   *  @returns RowGroup
   */
  const rowgroup::RowGroup& getFeRowGroup() const
  {
    return fRowGroupFe;
  }

  /** @brief get subquery step
   */
  const SJSTEP& subStep() const
  {
    return fSubStep;
  }

  /** @brief add filters (expression steps)
   */
  void addExpression(const JobStepVector&, JobInfo&);

  /** @brief add function columns (returned columns)
   */
  void addExpression(const std::vector<execplan::SRCP>&);

  /** @brief add function join expresssion
   */
  void addFcnJoinExp(const std::vector<execplan::SRCP>&);

 protected:
  void execute();
  void outputRow(rowgroup::Row&, rowgroup::Row&);
  void checkDupOutputColumns();
  void dupOutputColumns(rowgroup::Row&);
  void printCalTrace();
  void formatMiniStats();

  execplan::CalpontSystemCatalog::OID fTableOid;
  rowgroup::RowGroup fRowGroupIn;
  rowgroup::RowGroup fRowGroupOut;
  rowgroup::RowGroup fRowGroupFe;
  rowgroup::RowGroup fRowGroupDeliver;
  SJSTEP fSubStep;
  uint64_t fRowsInput;
  uint64_t fRowsReturned;
  bool fEndOfResult;
  std::shared_ptr<int[]> fIndexMap;
  std::vector<std::pair<uint32_t, uint32_t> > fDupColumns;

  RowGroupDL* fInputDL;
  RowGroupDL* fOutputDL;
  uint64_t fInputIterator;
  uint64_t fOutputIterator;

  class Runner
  {
   public:
    explicit Runner(SubAdapterStep* step) : fStep(step)
    {
    }
    void operator()()
    {
      utils::setThreadName("SQSRunner");
      fStep->execute();
    }

    SubAdapterStep* fStep;
  };
  uint64_t fRunner;  // thread pool handle

  boost::scoped_ptr<funcexp::FuncExpWrapper> fExpression;
};

}  // namespace joblist

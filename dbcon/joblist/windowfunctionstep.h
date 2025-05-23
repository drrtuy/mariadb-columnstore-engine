/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (c) 2016-2020 MariaDB

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

//  $Id: windowfunctionstep.h 9722 2013-07-25 21:29:10Z pleblanc $

#pragma once

#include "../../utils/windowfunction/idborderby.h"
#include "jobstep.h"
#include "rowgroup.h"
#include "windowfunctioncolumn.h"
#include "threadnaming.h"

namespace execplan
{
// forward reference
class CalpontSelectExecutionPlan;
class ParseTree;
class ReturnedColumn;
};  // namespace execplan

namespace windowfunction
{
// forward reference
class WindowFunction;
class FrameBound;
};  // namespace windowfunction

namespace ordering
{
// forward reference
class EqualCompData;
};  // namespace ordering

namespace joblist
{
// forward reference
struct JobInfo;
class ResourceManager;

struct RowPosition
{
  uint64_t fGroupId : 48;
  uint64_t fRowId : 16;

  inline explicit RowPosition(uint64_t i = 0, uint64_t j = 0) : fGroupId(i), fRowId(j){};
};

/** @brief class WindowFunctionStep
 *
 */
class WindowFunctionStep : public JobStep, public TupleDeliveryStep
{
 public:
  /** @brief WindowFunctionStep constructor
   * @param
   * @param
   */
  explicit WindowFunctionStep(const JobInfo&);

  /** @brief WindowFunctionStep destructor
   */
  ~WindowFunctionStep() override;

  /** @brief virtual methods
   */
  void run() override;
  void join() override;
  const std::string toString() const override;
  void setOutputRowGroup(const rowgroup::RowGroup&) override;
  const rowgroup::RowGroup& getOutputRowGroup() const override;
  const rowgroup::RowGroup& getDeliveredRowGroup() const override;
  void deliverStringTableRowGroup(bool b) override;
  bool deliverStringTableRowGroup() const override;
  uint32_t nextBand(messageqcpp::ByteStream& bs) override;

  /** @brief initialize methods
   */
  void initialize(const rowgroup::RowGroup& rg, JobInfo& jobInfo);

  static void checkWindowFunction(execplan::CalpontSelectExecutionPlan*, JobInfo&);
  static SJSTEP makeWindowFunctionStep(SJSTEP&, JobInfo&);

  // for WindowFunction and WindowFunctionWrapper callback
  const std::vector<RowPosition>& getRowData() const
  {
    return fRows;
  }
  // for string table
  rowgroup::Row::Pointer getPointer(RowPosition& pos)
  {
    fRowGroupIn.setData(&(fInRowGroupData[pos.fGroupId]));
    fRowGroupIn.getRow(pos.fRowId, &fRowIn);
    return fRowIn.getPointer();
  }

  rowgroup::Row::Pointer getPointer(RowPosition& pos, rowgroup::RowGroup& rg, rowgroup::Row& row)
  {
    rg.setData(&(fInRowGroupData[pos.fGroupId]));
    rg.getRow(pos.fRowId, &row);
    return row.getPointer();
  }

 private:
  void execute();
  void doFunction();
  void doPostProcessForSelect();
  void doPostProcessForDml();

  uint64_t nextFunctionIndex();

  boost::shared_ptr<windowfunction::FrameBound> parseFrameBound(
      const execplan::WF_Boundary&, const std::map<uint64_t, uint64_t>&, const std::vector<execplan::SRCP>&,
      const boost::shared_ptr<ordering::EqualCompData>&, JobInfo&, bool, bool);
  boost::shared_ptr<windowfunction::FrameBound> parseFrameBoundRows(const execplan::WF_Boundary&,
                                                                    const std::map<uint64_t, uint64_t>&,
                                                                    JobInfo&);
  boost::shared_ptr<windowfunction::FrameBound> parseFrameBoundRange(const execplan::WF_Boundary&,
                                                                     const std::map<uint64_t, uint64_t>&,
                                                                     const std::vector<execplan::SRCP>&,
                                                                     JobInfo&);
  void updateWindowCols(execplan::ParseTree*, const std::map<uint64_t, uint64_t>&, JobInfo&);
  void updateWindowCols(execplan::ReturnedColumn*, const std::map<uint64_t, uint64_t>&, JobInfo&);
  void sort(std::vector<joblist::RowPosition>::iterator, uint64_t);

  void formatMiniStats();
  void printCalTrace();

  static void AddSimplColumn(const std::vector<execplan::SimpleColumn*>& scs, JobInfo& jobInfo);

  class Runner
  {
   public:
    explicit Runner(WindowFunctionStep* step) : fStep(step)
    {
    }
    void operator()()
    {
      utils::setThreadName("WFSRunner");
      fStep->execute();
    }

    WindowFunctionStep* fStep;
  };

  uint64_t fRunner;  // thread pool handle

  boost::shared_ptr<execplan::CalpontSystemCatalog> fCatalog;
  uint64_t fRowsReturned;
  bool fEndOfResult;
  bool fIsSelect;
  bool fUseSSMutex;  //@bug6065, mutex for setStringField
  bool fUseUFMutex;  // To ensure thread safety of User Data (UDAnF)
  // for input/output datalist
  RowGroupDL* fInputDL;
  RowGroupDL* fOutputDL;
  int fInputIterator;
  int fOutputIterator;

  // rowgroups
  rowgroup::RowGroup fRowGroupIn;
  rowgroup::RowGroup fRowGroupOut;
  rowgroup::RowGroup fRowGroupDelivered;
  rowgroup::Row fRowIn;

  // data storage
  std::vector<rowgroup::RGData> fInRowGroupData;

  //	for funciton/expression taking window function as parameter
  std::vector<execplan::SRCP> fExpression;

  // for threads computing window functions and partitions
  class WFunction
  {
   public:
    explicit WFunction(WindowFunctionStep* step) : fStep(step)
    {
    }
    void operator()()
    {
      fStep->doFunction();
    }

    WindowFunctionStep* fStep;
  };
  std::vector<uint64_t> fFunctionThreads;

  std::vector<RowPosition> fRows;
  std::vector<boost::shared_ptr<windowfunction::WindowFunction> > fFunctions;
  uint64_t fFunctionCount;
  uint64_t fTotalThreads;
  int fNextIndex;

  // query order by
  boost::shared_ptr<ordering::OrderByData> fQueryOrderBy;
  uint64_t fQueryLimitStart;
  uint64_t fQueryLimitCount;

  // for resource management
  uint64_t fMemUsage;
  ResourceManager* fRm;
  boost::shared_ptr<int64_t> fSessionMemLimit;

  friend class windowfunction::WindowFunction;
};

}  // namespace joblist

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

/***********************************************************************
 *   $Id: returnedcolumn.h 9679 2013-07-11 22:32:03Z zzhu $
 *
 *
 ***********************************************************************/
/** @file */

#pragma once
#include <optional>
#include <string>
#include <iosfwd>
#include <vector>
#include <cmath>
#include <boost/shared_ptr.hpp>

#include "treenode.h"
#include "calpontsystemcatalog.h"

namespace messageqcpp
{
class ByteStream;
}

namespace rowgroup
{
class Row;
}

/**
 * @brief Namespace
 */
namespace execplan
{
// Join info bit mask
// const uint64_t JOIN_OUTER = 0x0001;
const uint64_t JOIN_SEMI = 0x0002;
const uint64_t JOIN_ANTI = 0x0004;
const uint64_t JOIN_SCALAR = 0x0008;
const uint64_t JOIN_NULL_MATCH = 0x0010;
const uint64_t JOIN_CORRELATED = 0x0020;
const uint64_t JOIN_NULLMATCH_CANDIDATE = 0x0040;
const uint64_t JOIN_OUTER_SELECT = 0x0080;

// column source bit mask
const uint64_t FROM_SUB = 0x0002;
const uint64_t SELECT_SUB = 0x0004;
const uint64_t CORRELATED_JOIN = 0x0008;

/**
 * @brief fwd ref
 */
class SimpleColumn;
class AggregateColumn;
class WindowFunctionColumn;
class ReturnedColumn;
class CalpontSelectExecutionPlan;

typedef boost::shared_ptr<ReturnedColumn> SRCP;

/**
 * @brief class ReturnedColumn
 */
class ReturnedColumn : public TreeNode
{
 public:
  /**
   * Constructors
   */
  ReturnedColumn();
  explicit ReturnedColumn(const std::string& sql);
  explicit ReturnedColumn(const uint32_t sessionID, const bool returnAll = false);
  ReturnedColumn(const ReturnedColumn& rhs, const uint32_t sessionID = 0);

  /**
   * Destructors
   */
  ~ReturnedColumn() override;

  /**
   * Accessor Methods
   */
  const std::string data() const override;
  void data(const std::string data) override
  {
    fData = data;
  }

  virtual bool returnAll() const
  {
    return fReturnAll;
  }
  virtual void returnAll(const bool returnAll)
  {
    fReturnAll = returnAll;
  }

  uint32_t sessionID() const
  {
    return fSessionID;
  }
  void sessionID(const uint32_t sessionID)
  {
    fSessionID = sessionID;
  }

  inline int32_t sequence() const
  {
    return fSequence;
  }
  inline void sequence(const int32_t sequence)
  {
    fSequence = sequence;
  }

  inline const std::string& alias() const
  {
    return fAlias;
  }
  inline void alias(const std::string& alias)
  {
    fAlias = alias;
  }

  virtual uint64_t cardinality() const
  {
    return fCardinality;
  }
  virtual void cardinality(const uint64_t cardinality)
  {
    fCardinality = cardinality;
  }

  virtual bool distinct() const
  {
    return fDistinct;
  }
  virtual void distinct(const bool distinct)
  {
    fDistinct = distinct;
  }

  uint32_t expressionId() const
  {
    return fExpressionId;
  }
  void expressionId(const uint32_t expressionId)
  {
    fExpressionId = expressionId;
  }

  virtual uint64_t joinInfo() const
  {
    return fJoinInfo;
  }
  virtual void joinInfo(const uint64_t joinInfo)
  {
    fJoinInfo = joinInfo;
  }

  virtual bool asc() const
  {
    return fAsc;
  }
  virtual void asc(const bool asc)
  {
    fAsc = asc;
  }

  virtual bool nullsFirst() const
  {
    return fNullsFirst;
  }
  virtual void nullsFirst(const bool nullsFirst)
  {
    fNullsFirst = nullsFirst;
  }

  virtual uint64_t orderPos() const
  {
    return fOrderPos;
  }
  virtual void orderPos(const uint64_t orderPos)
  {
    fOrderPos = orderPos;
  }

  virtual uint64_t colSource() const
  {
    return fColSource;
  }
  virtual void colSource(const uint64_t colSource)
  {
    fColSource = colSource;
  }

  virtual int64_t colPosition() const
  {
    return fColPosition;
  }
  virtual void colPosition(const int64_t colPosition)
  {
    fColPosition = colPosition;
  }

  // Columns that may have aggregate column involved should implement this interface.
  virtual bool hasAggregate()
  {
    return fHasAggregate;
  }
  virtual void hasAggregate(bool hasAgg)
  {
    fHasAggregate = hasAgg;
  }

  /**
   * Operations
   */
  ReturnedColumn* clone() const override = 0;

  /**
   * The serialize interface
   */
  void serialize(messageqcpp::ByteStream&) const override;
  void unserialize(messageqcpp::ByteStream&) override;

  const std::string toString() const override;
  std::string toCppCode(IncludeSet& includes) const override;
  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true iff every member of t is a duplicate copy of every member of this; false otherwise
   */
  bool operator==(const TreeNode* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true iff every member of t is a duplicate copy of every member of this; false otherwise
   * @warning this member function is NOT virtual (why?)
   */
  bool operator==(const ReturnedColumn& t) const;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false iff every member of t is a duplicate copy of every member of this; true otherwise
   */
  bool operator!=(const TreeNode* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false iff every member of t is a duplicate copy of every member of this; true otherwise
   * @warning this member function is NOT virtual (why?)
   */
  bool operator!=(const ReturnedColumn& t) const;

  /** @brief check if this column is the same as the argument
   *
   * @note Why can't operator==() be used?
   */
  virtual bool sameColumn(const ReturnedColumn* rc) const
  {
    return (fData.compare(rc->data()) == 0);
  }

  virtual const std::vector<SimpleColumn*>& simpleColumnList() const
  {
    return fSimpleColumnList;
  }

  /* @brief traverse this ReturnedColumn and re-populate fSimpleColumnList.
   *
   * @note all ReturnedColumns that may have simple column arguments added
   * to the list need to implement this function.
   */
  virtual void setSimpleColumnList();

  // get all aggregate column list in this expression
  const std::vector<AggregateColumn*>& aggColumnList() const
  {
    return fAggColumnList;
  }

  // get all window function column list in this expression
  const std::vector<WindowFunctionColumn*>& windowfunctionColumnList() const
  {
    return fWindowFunctionColumnList;
  }

  /* @brief if this column is or contains window function column
   *
   * @note after this function call fWindowFunctionColumnList is populated
   */
  virtual bool hasWindowFunc() = 0;

  virtual void replaceRealCol(std::vector<SRCP>&)
  {
  }

  /**
   * Return the tableAlias name of the table that the column arguments belong to.
   *
   * @param TableAliasName that will be set in the function
   * @return true, if all arguments belong to one table
   *         false, if multiple tables are involved in the function
   */
  virtual std::optional<CalpontSystemCatalog::TableAliasName> singleTable()
  {
    return std::nullopt;
  }

 protected:
  // return all flag set if the other column is outer join column (+)
  bool fReturnAll;
  uint32_t fSessionID;
  int32_t fSequence;  /// column sequence on the SELECT mapped to the correlated joins
  uint64_t fCardinality;
  std::string fAlias;  /// column alias
  bool fDistinct;
  uint64_t fJoinInfo;
  bool fAsc;             /// for order by column
  bool fNullsFirst;      /// for window function
  uint64_t fOrderPos;    /// for order by and group by column
  uint64_t fColSource;   /// from which subquery
  int64_t fColPosition;  /// the column position in the source subquery
  std::vector<SimpleColumn*> fSimpleColumnList;
  std::vector<AggregateColumn*> fAggColumnList;
  std::vector<WindowFunctionColumn*> fWindowFunctionColumnList;
  bool fHasAggregate;  /// connector internal use. no need to serialize

 private:
  std::string fData;

  /******************************************************************
   *                   F&E framework                                *
   ******************************************************************/
 public:
  uint32_t inputIndex() const
  {
    return fInputIndex;
  }
  void inputIndex(const uint32_t inputIndex)
  {
    fInputIndex = inputIndex;
  }
  uint32_t outputIndex() const
  {
    return fOutputIndex;
  }
  void outputIndex(const uint32_t outputIndex)
  {
    fOutputIndex = outputIndex;
  }

 protected:
  std::string fErrMsg;     /// error occurred in evaluation
  uint32_t fInputIndex;    /// index to the input rowgroup
  uint32_t fOutputIndex;   /// index to the output rowgroup
  uint32_t fExpressionId;  /// unique id for this expression
};

std::ostream& operator<<(std::ostream& os, const ReturnedColumn& rhs);

}  // namespace execplan

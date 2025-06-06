/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2019 MariaDB Corporation

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
 *   $Id: simplefilter.h 9679 2013-07-11 22:32:03Z zzhu $
 *
 *
 ***********************************************************************/
/** @file */

#pragma once
#include <string>
#include <iosfwd>
#include <boost/shared_ptr.hpp>

#include "filter.h"
#include "predicateoperator.h"
#include "returnedcolumn.h"

/**
 * Namespace
 */
namespace execplan
{
class AggregateColumn;
class WindowFunctionColumn;

/**
 * @brief A class to represent a simple WHERE clause predicate
 *
 * This class is a specialization of class Filter that handles simple
 * binary operation WHERE clauses, e.g. last_name = ‘SMITH’.
 */
class SimpleFilter : public Filter
{
 public:
  /** index flag */
  enum IndexFlag
  {
    NOINDEX = 0,
    LEFT,
    RIGHT,
    BOTH
  };

  enum JoinFlag
  {
    EQUA = 0,
    ANTI,
    SEMI
  };

  struct ForTestPurposesWithoutColumnsOIDS
  {
  };

  SimpleFilter();
  explicit SimpleFilter(const std::string& sql);
  SimpleFilter(const std::string& sql, ForTestPurposesWithoutColumnsOIDS);
  SimpleFilter(const SOP& op, ReturnedColumn* lhs, ReturnedColumn* rhs, const long timeZone = 0);
  SimpleFilter(const SimpleFilter& rhs);

  ~SimpleFilter() override;

  inline SimpleFilter* clone() const override
  {
    return new SimpleFilter(*this);
  }

  const SOP& op() const
  {
    return fOp;
  }

  void op(const SOP& op)
  {
    fOp = op;
  }

  inline ReturnedColumn* lhs() const
  {
    return fLhs;
  }

  inline long timeZone() const
  {
    return fTimeZone;
  }

  inline void timeZone(const long timeZone)
  {
    fTimeZone = timeZone;
  }

  using Filter::data;
  const std::string data() const override;

  /** assign fLhs
   *
   * this call takes over the ownership of the input pointer. Caller needs
   * to put the input pointer to 0 after the call; or to remember not to
   * delete the pointer accidentally.
   */
  void lhs(ReturnedColumn* lhs);

  inline ReturnedColumn* rhs() const
  {
    return fRhs;
  }

  /** assign fRhs
   *
   * this call takes over the ownership of the pointer rhs. Called need
   * to put the input pointer to 0 after the all; or remember not to
   * delete the pointer accidentally.
   */
  void rhs(ReturnedColumn* rhs);

  const std::string toString() const override;

  /**
   * The serialization interface
   */
  void serialize(messageqcpp::ByteStream&) const override;
  void unserialize(messageqcpp::ByteStream&) override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true if every member of t is a duplicate copy of every member of this; false otherwise
   */
  bool operator==(const TreeNode* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true if every member of t is a duplicate copy of every member of this; false otherwise
   */
  bool operator==(const SimpleFilter& t) const;

  /** @brief Do a semantic equivalence test
   *
   * Do a semantic equivalence test.
   * @return true if filter operation are the same and
   * the sets of arguments are the same; false otherwise
   */

  bool operator<(const SimpleFilter& t) const;

  bool semanticEq(const SimpleFilter& t) const;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false if every member of t is a duplicate copy of every member of this; true otherwise
   */
  bool operator!=(const TreeNode* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false if every member of t is a duplicate copy of every member of this; true otherwise
   */
  bool operator!=(const SimpleFilter& t) const;

  /** @brief determin if this simple filter is a pure filter (col/const)
   */
  bool pureFilter();

  /** @brief test if this filter can be combined with the argument filter
   *  This is for operation combine optimization
   *  @param f the filter that this fiter tries to combine with
   *  @param op the operator that connects the two filters. if one or both of the
   *  two filters is constantFilter, need to make sure operator is consistant.
   *  @return a filter(constantfilter) if successfully combined. otherwise
   *	 	 return NULL
   * For Oracle Frontend use. Deprecated now.
   */
  // virtual Filter* combinable(Filter* f, Operator* op);

  /** @brief assign indexflag to indicate which side col is index */
  void indexFlag(const int indexFlag)
  {
    fIndexFlag = indexFlag;
  }
  int indexFlag() const
  {
    return fIndexFlag;
  }

  /** @brief assign joinflag to indicate which join type */
  void joinFlag(const int joinFlag)
  {
    fJoinFlag = joinFlag;
  }
  int joinFlag() const
  {
    return fJoinFlag;
  }

  /** @brief this function is called by the connector to set constant values according to the compare type */
  void convertConstant();

  void setDerivedTable() override;

  virtual void replaceRealCol(std::vector<SRCP>&);

  static std::string escapeString(const std::string& input);

  string toCppCode(IncludeSet& includes) const override;

 private:
  SOP fOp;               /// operator
  ReturnedColumn* fLhs;  /// left operand
  ReturnedColumn* fRhs;  /// right operand
  int fIndexFlag;        /// which side col is index
  int fJoinFlag;         /// hash join type
  long fTimeZone;

  void parse(std::string, std::optional<ForTestPurposesWithoutColumnsOIDS> testFlag = std::nullopt);

  /***********************************************************
   *                      F&E framework                      *
   ***********************************************************/
 public:
  inline bool getBoolVal(rowgroup::Row& row, bool& isNull) override;
  inline int64_t getIntVal(rowgroup::Row& row, bool& isNull) override;
  inline double getDoubleVal(rowgroup::Row& row, bool& isNull) override;
  inline long double getLongDoubleVal(rowgroup::Row& row, bool& isNull) override;

  // get all simple columns involved in this column
  const std::vector<SimpleColumn*>& simpleColumnList();
  // walk through the simple filter operands to re-populate fSimpleColumnList
  void setSimpleColumnList();
  // walk through the simple filter operands to check existence of aggregate
  bool hasAggregate();

  // get all aggregate columns involved in this column
  const std::vector<AggregateColumn*>& aggColumnList() const
  {
    return fAggColumnList;
  }

  // get all window function columns involved in this column
  const std::vector<WindowFunctionColumn*>& windowfunctionColumnList() const
  {
    return fWindowFunctionColumnList;
  }

 private:
  std::vector<SimpleColumn*> fSimpleColumnList;
  std::vector<AggregateColumn*> fAggColumnList;
  std::vector<WindowFunctionColumn*> fWindowFunctionColumnList;
};

inline bool SimpleFilter::getBoolVal(rowgroup::Row& row, bool& isNull)
{
  return (reinterpret_cast<PredicateOperator*>(fOp.get())->getBoolVal(row, isNull, fLhs, fRhs));
}

inline int64_t SimpleFilter::getIntVal(rowgroup::Row& row, bool& isNull)
{
  return (reinterpret_cast<PredicateOperator*>(fOp.get())->getBoolVal(row, isNull, fLhs, fRhs) ? 1 : 0);
}

inline double SimpleFilter::getDoubleVal(rowgroup::Row& row, bool& isNull)
{
  return getIntVal(row, isNull);
}

inline long double SimpleFilter::getLongDoubleVal(rowgroup::Row& row, bool& isNull)
{
  return getIntVal(row, isNull);
}

typedef boost::shared_ptr<SimpleFilter> SSFP;

std::ostream& operator<<(std::ostream& output, const SimpleFilter& rhs);

}  // namespace execplan

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
 *   $Id: existsfilter.h 9210 2013-01-21 14:10:42Z rdempsey $
 *
 *
 ***********************************************************************/
/** @file */

#pragma once
#include <string>

#include "filter.h"
#include "calpontselectexecutionplan.h"

/**
 * Namespace
 */
namespace execplan
{
/**
 * @brief A class to represent a exists predicate
 *
 * This class is a specialization of class Filter that handles a
 * exists predicate like "exists (select col2 from table2)"
 */
class ExistsFilter : public Filter
{
  /**
   * Public stuff
   */
 public:
  /**
   * Constructors
   */
  ExistsFilter();
  explicit ExistsFilter(const SCSEP& sub, const bool existsFlag = false, const bool correlated = false);
  ExistsFilter(const ExistsFilter& rhs);
  ~ExistsFilter() override;

  /**
   * Accessor Methods
   */
  const SCSEP& sub() const
  {
    return fSub;
  }
  void sub(SCSEP& sub)
  {
    fSub = sub;
  }

  bool notExists() const
  {
    return fNotExists;
  }
  void notExists(const bool notExists)
  {
    fNotExists = notExists;
  }

  bool correlated() const
  {
    return fCorrelated;
  }
  void correlated(const bool correlated)
  {
    fCorrelated = correlated;
  }

  /**
   * Overloaded stream operator
   */
  // virtual std::ostream& operator<< (std::ostream& output);
  const std::string toString() const override;
  std::string toCppCode(IncludeSet& includes) const override;
  /**
   * The serialization interface
   */
  void serialize(messageqcpp::ByteStream&) const override;
  void unserialize(messageqcpp::ByteStream&) override;

  /** return a copy of this pointer
   *
   * deep copy of this pointer and return the copy
   */
  inline ExistsFilter* clone() const override
  {
    return new ExistsFilter(*this);
  }

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
   */
  bool operator==(const ExistsFilter& t) const;

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
   */
  bool operator!=(const ExistsFilter& t) const;

 private:
  // default okay?
  // ExistsFilter& operator=(const ExistsFilter& rhs);

  SCSEP fSub;
  bool fNotExists;  /// if this is not exist?
  bool fCorrelated;
  std::string fData;
};

std::ostream& operator<<(std::ostream& output, const ExistsFilter& rhs);

}  // namespace execplan

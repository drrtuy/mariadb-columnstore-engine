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
 *   $Id: calpontselectexecutionplan.h 9576 2013-05-29 21:02:11Z zzhu $
 *
 *
 ***********************************************************************/
/** @file */

#pragma once
#include <vector>
#include <map>
#include <iosfwd>

#include <boost/uuid/uuid.hpp>

#include "exp_templates.h"
#include "calpontexecutionplan.h"
#include "returnedcolumn.h"
#include "filter.h"
#include "expressionparser.h"
#include "calpontsystemcatalog.h"
#include "brmtypes.h"
#include <boost/algorithm/string/case_conv.hpp>

#ifndef __GNUC__
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

/**
 * Namespace
 */
namespace execplan
{
enum RM_PARMS
{
  PMSMALLSIDEMEMORY,
  UMSMALLSIDEMEMORY,
};

struct RMParam
{
  RMParam(uint32_t s, uint16_t i, uint64_t v) : sessionId(s), id(i), value(v)
  {
  }
  RMParam() : sessionId(0), id(0), value(0)
  {
  }
  uint32_t sessionId;
  uint16_t id;
  uint64_t value;
};

typedef boost::shared_ptr<CalpontSelectExecutionPlan> SCSEP;

/**
 * @brief A class to represent calpont select execution plan
 *
 * This class is a concrete implementation of a CalpontExecutionPlan
 * specifically describing a SQL select activity.
 */
class CalpontSelectExecutionPlan : public CalpontExecutionPlan
{
  /**
   * Public stuff
   */
 public:
  /**
   * Types and constants
   */
  typedef std::vector<SRCP> ReturnedColumnList;
  typedef std::vector<SRCP> GroupByColumnList;
  typedef std::vector<SRCP> OrderByColumnList;
  typedef std::multimap<std::string, SRCP> ColumnMap;
  typedef std::vector<TreeNode*> FilterTokenList;

  typedef expression::expression_parser<Token, ParseTree*, TreeNode*, ExpressionParser> Parser;
  typedef std::vector<CalpontSystemCatalog::TableAliasName> TableList;
  typedef std::vector<SCEP> SelectList;

  typedef std::vector<RMParam> RMParmVec;

  // query type of this select plan.
#undef DELETE  // Windows defines this...
  enum IDB_QUERYTYPE
  {
    SELECT,
    UPDATE,
    DELETE,
    INSERT_SELECT,
    CREATE_TABLE,
    DROP_TABLE,
    ALTER_TABLE,
    INSERT,
    LOAD_DATA_INFILE
  };

  enum SE_LOCATION
  {
    MAIN,
    FROM,
    WHERE,
    HAVING
  };

  /** subselect type */
  enum SE_SubSelectType
  {
    MAIN_SELECT,
    SINGLEROW_SUBS,
    EXISTS_SUBS,
    NOT_EXISTS_SUBS,
    IN_SUBS,
    NOT_IN_SUBS,
    ALL_SUBS,
    ANY_SUBS,
    FROM_SUBS,
    SELECT_SUBS,
    UNKNOWN_SUBS
  };

  enum IDB_LOCAL_QUERY
  {
    GLOBAL_QUERY = 0, /*!< Standard processing */
    LOCAL_QUERY = 1,  /*!< Use local ExeMgr and local PrimProc */
    LOCAL_UM = 2,     /*!< Use local ExeMgr with global PrimProc*/
  };

  /**
   * Constructors
   */
  explicit CalpontSelectExecutionPlan(int location = MAIN);

  CalpontSelectExecutionPlan(ReturnedColumnList returnedCols, ParseTree* filters, SelectList subSelects,
                             GroupByColumnList groupByCols, ParseTree* having, OrderByColumnList orderByCols,
                             std::string alias, int location, bool dependent, bool withRollup);

  explicit CalpontSelectExecutionPlan(std::string data);

  /**
   * Destructors
   */
  ~CalpontSelectExecutionPlan() override;

  /**
   * Clones this CSEP without recursive selects for optimizer purposes
   */
  execplan::SCSEP cloneWORecursiveSelects();

  execplan::SCSEP cloneForTableWORecursiveSelects(const execplan::CalpontSystemCatalog::TableAliasName& targetTableAlias);

  /**
   * Access and mutator methods
   */

  /**
   * returned column list
   */
  const ReturnedColumnList& returnedCols() const
  {
    return fReturnedCols;
  }
  ReturnedColumnList& returnedCols()
  {
    return fReturnedCols;
  }
  void returnedCols(const ReturnedColumnList& returnedCols)
  {
    fReturnedCols = returnedCols;
  }

  /**
   * Are we in local PM only query mode?
   */
  uint32_t localQuery() const
  {
    return fLocalQuery;
  }
  void localQuery(const uint32_t localQuery)
  {
    fLocalQuery = localQuery;
  }

  /**
   * filters parse tree
   */
  const ParseTree* filters() const
  {
    return fFilters;
  }
  ParseTree* filters()
  {
    return fFilters;
  }
  void filters(ParseTree* filters)
  {
    fFilters = filters;
  }

  /**  filter token list
   * Set filter list field and build filters tree from the filter list
   * This is an alternative way to build filter tree. Hide the parser
   * from the user. filterTokenList will be destroyed after the parsing
   */
  // const FilterTokenList& filterTokenList() const { return fFilterTokenList;}
  void filterTokenList(FilterTokenList& filterTokenList);

  /**
   * sub select list
   */
  const SelectList& subSelects() const
  {
    return fSubSelects;
  }
  void subSelects(const SelectList& subSelects)
  {
    fSubSelects = subSelects;
  }

  /**
   * group by column list
   */
  const GroupByColumnList& groupByCols() const
  {
    return fGroupByCols;
  }
  GroupByColumnList& groupByCols()
  {
    return fGroupByCols;
  }
  void groupByCols(const GroupByColumnList& groupByCols)
  {
    fGroupByCols = groupByCols;
  }

  /**
   * Subtotals.
   */
  bool withRollup() const
  {
    return fWithRollup;
  }
  void withRollup(bool withRollup)
  {
    fWithRollup = withRollup;
  }

  /**
   * order by column list
   */
  const OrderByColumnList& orderByCols() const
  {
    return fOrderByCols;
  }
  OrderByColumnList& orderByCols()
  {
    return fOrderByCols;
  }
  void orderByCols(const OrderByColumnList& orderByCols)
  {
    fOrderByCols = orderByCols;
  }

  /**
   * table alias
   */
  const std::string& tableAlias() const
  {
    return fTableAlias;
  }
  void tableAlias(const std::string& tableAlias, int lower_case_table_names)
  {
    fTableAlias = tableAlias;
    if (lower_case_table_names)
      boost::algorithm::to_lower(fTableAlias);
  }

  /**
   * location of this select
   */
  int location() const
  {
    return fLocation;
  }
  void location(const int location)
  {
    fLocation = location;
  }

  /**
   * dependence of this select
   */
  bool dependent() const
  {
    return fDependent;
  }
  void dependent(const bool dependent)
  {
    fDependent = dependent;
  }

  /**
   * having filter parse tree
   */
  inline const ParseTree* having() const
  {
    return fHaving;
  }
  inline ParseTree* having()
  {
    return fHaving;
  }
  inline void having(ParseTree* having)
  {
    fHaving = having;
  }

  /** having filter token list
   * Set filter list field and build filters tree from the filter list
   * This is an alternative way to build filter tree. Hide the parser
   * from the user
   */
  const FilterTokenList& havingTokenList() const
  {
    return fHavingTokenList;
  }
  void havingTokenList(const FilterTokenList& havingTokenList);

  /** column map
   * all the columns appeared on query
   */
  const ColumnMap& columnMap() const
  {
    return fColumnMap;
  }

  /** column map
   * all the columns appeared on query
   */
  ColumnMap& columnMap()
  {
    return fColumnMap;
  }

  /** assign the static fColMap to non-static fColumnMap. map-wise copy */
  void columnMap(const ColumnMap& columnMap);

  /** assign a regular map object to non-static fColumnMap. pure assignment */
  // @note this is to fix memory leak in CSC, becasue no static map is needed there.
  inline void columnMapNonStatic(const ColumnMap& columnMap)
  {
    fColumnMap = columnMap;
  }

  /** sql representation of this select query
   *
   */
  const std::string& data() const
  {
    return fData;
  }
  void data(std::string data)
  {
    fData = std::move(data);
  }

  /** session id
   *
   */
  uint32_t sessionID() const
  {
    return fSessionID;
  }
  void sessionID(const uint32_t sessionID)
  {
    fSessionID = sessionID;
  }

  /** transaction id
   *
   */
  int txnID() const
  {
    return fTxnID;
  }
  void txnID(const int txnID)
  {
    fTxnID = txnID;
  }

  /** version id
   *
   */
  const BRM::QueryContext verID() const
  {
    return fVerID;
  }
  void verID(const BRM::QueryContext verID)
  {
    fVerID = verID;
  }

  inline static ColumnMap& colMap()
  {
    return fColMap;
  }

  inline std::string& schemaName()
  {
    return fSchemaName;
  }
  inline void schemaName(const std::string& schemaName, int lower_case_table_names)
  {
    fSchemaName = schemaName;
    if (lower_case_table_names)
      boost::algorithm::to_lower(fSchemaName);
  }

  inline std::string& tableName()
  {
    return fTableName;
  }
  inline void tableName(const std::string& tableName, int lower_case_table_names)
  {
    fTableName = tableName;
    if (lower_case_table_names)
      boost::algorithm::to_lower(fTableName);
  }

  inline void traceOn(bool traceOn) __attribute__((deprecated))
  {
    if (traceOn)
      fTraceFlags |= TRACE_LOG;
    else
      fTraceFlags &= ~TRACE_LOG;
  }
  inline bool traceOn() const
  {
    return (traceFlags() & TRACE_LOG);
  }

  inline uint32_t traceFlags() const
  {
    return fTraceFlags;
  }
  inline void traceFlags(uint32_t traceFlags)
  {
    fTraceFlags = traceFlags;
  }

  uint32_t statementID() const
  {
    return fStatementID;
  }
  void statementID(const uint32_t statementID)
  {
    fStatementID = statementID;
  }

  const RMParmVec& rmParms()
  {
    return frmParms;
  }
  void rmParms(const RMParmVec& parms);

  const TableList& tableList() const
  {
    return fTableList;
  }
  void tableList(const TableList& tableList)
  {
    fTableList = tableList;
  }

  const SelectList& derivedTableList() const
  {
    return fDerivedTableList;
  }
  void derivedTableList(SelectList& derivedTableList)
  {
    fDerivedTableList = derivedTableList;
  }

  bool distinct() const
  {
    return fDistinct;
  }
  void distinct(const bool distinct)
  {
    fDistinct = distinct;
  }

  void overrideLargeSideEstimate(const bool over)
  {
    fOverrideLargeSideEstimate = over;
  }
  bool overrideLargeSideEstimate() const
  {
    return fOverrideLargeSideEstimate;
  }

  void unionVec(const SelectList& unionVec)
  {
    fUnionVec = unionVec;
  }
  const SelectList& unionVec() const
  {
    return fUnionVec;
  }
  SelectList& unionVec()
  {
    return fUnionVec;
  }

  void distinctUnionNum(const uint8_t distinctUnionNum)
  {
    fDistinctUnionNum = distinctUnionNum;
  }
  uint8_t distinctUnionNum() const
  {
    return fDistinctUnionNum;
  }

  void subType(const uint64_t subType)
  {
    fSubType = subType;
  }
  uint64_t subType() const
  {
    return fSubType;
  }

  void derivedTbAlias(const std::string derivedTbAlias, int lower_case_table_names = 0)
  {
    fDerivedTbAlias = derivedTbAlias;
    if (lower_case_table_names)
      boost::algorithm::to_lower(fDerivedTbAlias);
  }
  const std::string derivedTbAlias() const
  {
    return fDerivedTbAlias;
  }

  void derivedTbView(const std::string derivedTbView, int lower_case_table_names)
  {
    fDerivedTbView = derivedTbView;
    if (lower_case_table_names)
      boost::algorithm::to_lower(fDerivedTbView);
  }
  const std::string derivedTbView() const
  {
    return fDerivedTbView;
  }

  void limitStart(const uint64_t limitStart)
  {
    fLimitStart = limitStart;
  }
  uint64_t limitStart() const
  {
    return fLimitStart;
  }

  void limitNum(const uint64_t limitNum)
  {
    fLimitNum = limitNum;
  }
  uint64_t limitNum() const
  {
    return fLimitNum;
  }

  void hasOrderBy(const bool hasOrderBy)
  {
    fHasOrderBy = hasOrderBy;
  }
  bool hasOrderBy() const
  {
    return fHasOrderBy;
  }

  void specHandlerProcessed(const bool hand)
  {
    fSpecHandlerProcessed = hand;
  }
  bool specHandlerProcessed() const
  {
    return fSpecHandlerProcessed;
  }

  void orderByThreads(const uint32_t number)
  {
    fOrderByThreads = number;
  }
  uint32_t orderByThreads() const
  {
    return fOrderByThreads;
  }

  void pron(std::string&& pron);

  void selectSubList(const SelectList& selectSubList)
  {
    fSelectSubList = selectSubList;
  }
  const SelectList& selectSubList() const
  {
    return fSelectSubList;
  }

  void subSelectList(const std::vector<execplan::SCSEP>& subSelectList)
  {
    fSubSelectList = subSelectList;
  }
  const std::vector<execplan::SCSEP>& subSelectList() const
  {
    return fSubSelectList;
  }

  void stringScanThreshold(uint64_t n)
  {
    fStringScanThreshold = n;
  }
  uint64_t stringScanThreshold() const
  {
    return fStringScanThreshold;
  }

  // query type. return string for easy stats insert
  void queryType(const uint32_t queryType)
  {
    fQueryType = queryType;
  }
  const std::string queryType() const
  {
    return queryTypeToString(fQueryType);
  }
  static std::string queryTypeToString(const uint32_t queryType);

  void priority(uint32_t p)
  {
    fPriority = p;
  }
  uint32_t priority() const
  {
    return fPriority;
  }

  void stringTableThreshold(uint32_t t)
  {
    fStringTableThreshold = t;
  }
  uint32_t stringTableThreshold() const
  {
    return fStringTableThreshold;
  }

  void uuid(const boost::uuids::uuid& uuid)
  {
    fUuid = uuid;
  }
  const boost::uuids::uuid& uuid() const
  {
    return fUuid;
  }

  void djsSmallSideLimit(uint64_t l)
  {
    fDJSSmallSideLimit = l;
  }
  uint64_t djsSmallSideLimit() const
  {
    return fDJSSmallSideLimit;
  }

  void djsLargeSideLimit(uint64_t l)
  {
    fDJSLargeSideLimit = l;
  }
  uint64_t djsLargeSideLimit() const
  {
    return fDJSLargeSideLimit;
  }

  void djsPartitionSize(uint64_t l)
  {
    fDJSPartitionSize = l;
  }
  uint64_t djsPartitionSize() const
  {
    return fDJSPartitionSize;
  }

  void djsMaxPartitionTreeDepth(uint32_t value)
  {
    fDJSMaxPartitionTreeDepth = value;
  }
  uint64_t djsMaxPartitionTreeDepth()
  {
    return fDJSMaxPartitionTreeDepth;
  }

  void djsForceRun(bool b)
  {
    fDJSForceRun = b;
  }
  bool djsForceRun() const
  {
    return fDJSForceRun;
  }

  void maxPmJoinResultCount(uint32_t value)
  {
    fMaxPmJoinResultCount = value;
  }
  uint32_t maxPmJoinResultCount() const
  {
    return fMaxPmJoinResultCount;
  }

  void umMemLimit(uint64_t l)
  {
    fUMMemLimit = l;
  }
  int64_t umMemLimit() const
  {
    return fUMMemLimit;
  }

  void isDML(bool b)
  {
    fIsDML = b;
  }
  bool isDML() const
  {
    return fIsDML;
  }

  void timeZone(const long timezone)
  {
    fTimeZone = timezone;
  }
  long timeZone() const
  {
    return fTimeZone;
  }

  /**
   * The serialization interface
   */
  /**
   * @note Serialize() assumes that none of the vectors contain NULL pointers.
   */
  void serialize(messageqcpp::ByteStream&) const override;
  void unserialize(messageqcpp::ByteStream&) override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true iff every member of t is a duplicate copy of every member of this; false otherwise
   */
  bool operator==(const CalpontExecutionPlan* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return true iff every member of t is a duplicate copy of every member of this; false otherwise
   */
  virtual bool operator==(const CalpontSelectExecutionPlan& t) const;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false iff every member of t is a duplicate copy of every member of this; true otherwise
   */
  bool operator!=(const CalpontExecutionPlan* t) const override;

  /** @brief Do a deep, strict (as opposed to semantic) equivalence test
   *
   * Do a deep, strict (as opposed to semantic) equivalence test.
   * @return false iff every member of t is a duplicate copy of every member of this; true otherwise
   */
  virtual bool operator!=(const CalpontSelectExecutionPlan& t) const;

  /** @brief Return a string rep of the CSEP
   *
   * Return a string rep of the CSEP
   * @return a string
   */
  void printSubCSEP(const size_t& ident, ostringstream& output, CalpontSelectExecutionPlan*& plan) const;
  virtual std::string toString(const size_t ident = 0) const;

  /** @brief Is this an internal query?
   *
   * Is this an internal query (a syscat query performed on behalf of another query)
   * FIXME: add a setter and make this work for really big session ids
   * @return true/false
   */
  virtual bool isInternal() const
  {
    return ((fSessionID & 0x80000000) != 0);
  }

  virtual void setDynamicParseTreeVec(const std::vector<execplan::ParseTree*>& dynamicParseTreeVec)
  {
    fDynamicParseTreeVec = dynamicParseTreeVec;
  }

  /**
   * Protected stuff
   */
 protected:
  /**
   * Fields
   */
  /**
   *
   */
  /**
   * Constructors
   */
  /**
   * Accessor Methods
   */
  /**
   * Operations
   */
  /**
   * Private stuff
   */
 private:
  /**
   * If set, then the local PM only option is turned on
   */
  uint32_t fLocalQuery = GLOBAL_QUERY;

  /**
   * A list of ReturnedColumn objects
   */
  ReturnedColumnList fReturnedCols;
  /**
   * A list of filter tokens including filters and operators
   * This is a helper data structure, it holds the tokens of
   * the filter parse tree and will be built into a parse tree
   * in the mutator method.
   * @note the elements in this list will be deleted when fFilters
   * parse tree is deleted. So this list should not be deleted
   * again in destructor.
   */
  FilterTokenList fFilterTokenList;
  FilterTokenList fHavingTokenList;

  /**
   * A tree of Filter objects
   */
  ParseTree* fFilters{nullptr};
  /**
   * A list of CalpontExecutionPlan objects
   */
  SelectList fSubSelects;
  /**
   * A list of group by columns
   */
  GroupByColumnList fGroupByCols;
  /**
   * A tree of having clause condition associated with group by clause
   */
  ParseTree* fHaving{nullptr};
  /**
   * A list of order by columns
   */
  OrderByColumnList fOrderByCols;
  /**
   * Table or alias name for subselect in FROM clause
   */
  std::string fTableAlias;
  /**
   * An enum indicating the location of this select statement in the enclosing select statement
   */
  int fLocation = 0;
  /**
   * A flag indicating if this sub-select is dependent on the enclosing query or is constant
   */
  bool fDependent = false;

  /**
   * SQL representation of this execution plan
   */
  std::string fData;
  static ColumnMap fColMap;  // for getplan to use. class-wise map
  ColumnMap fColumnMap;      // for ExeMgr to use. not shared between objects

  uint32_t fSessionID = 0;
  int fTxnID = -1;  // SQLEngine only needs the ID value
  BRM::QueryContext fVerID;
  // @bug5316. remove static
  std::string fSchemaName;
  std::string fTableName;
  uint32_t fTraceFlags = TRACE_NONE;

  /**
   * One-up statementID number for this session (fSessionID)
   */
  uint32_t fStatementID = 0;

  RMParmVec frmParms;
  TableList fTableList;
  SelectList fDerivedTableList;

  bool fDistinct = false;
  bool fOverrideLargeSideEstimate = false;

  // for union
  SelectList fUnionVec;
  uint8_t fDistinctUnionNum = 0;

  // for subselect
  uint64_t fSubType = MAIN_SELECT;
  std::string fDerivedTbAlias;
  std::string fDerivedTbView;

  // for limit
  uint64_t fLimitStart = 0;
  uint64_t fLimitNum = -1;

  // for parent select order by
  bool fHasOrderBy = false;

  // for Select clause subquery
  SelectList fSelectSubList;

  // @bug3321, for string scan blocks
  uint64_t fStringScanThreshold = ULONG_MAX;

  // query type
  uint32_t fQueryType = SELECT;

  uint32_t fPriority;
  uint32_t fStringTableThreshold = 20;

  // for specific handlers processing, e.g. GROUP BY
  bool fSpecHandlerProcessed = false;
  uint32_t fOrderByThreads = 1;

  // Derived table involved in the query. For derived table optimization
  std::vector<SCSEP> fSubSelectList;

  boost::uuids::uuid fUuid{};

  /* Disk-based join vars */
  uint64_t fDJSSmallSideLimit = 0;
  uint64_t fDJSLargeSideLimit = 0;
  uint64_t fDJSPartitionSize = 100 * 1024 * 1024;
  uint32_t fDJSMaxPartitionTreeDepth = 8;
  bool fDJSForceRun = false;
  uint32_t fMaxPmJoinResultCount = 1048576;
  int64_t fUMMemLimit = numeric_limits<int64_t>::max();
  bool fIsDML = false;
  long fTimeZone = 0;
  std::vector<execplan::ParseTree*> fDynamicParseTreeVec;
  std::string fPron;
  /**
   * A flag to compute subtotals, related to GROUP BY operation.
   */
  bool fWithRollup;
};

/**
 *	Output stream operator
 */

inline std::ostream& operator<<(std::ostream& os, const CalpontSelectExecutionPlan& rhs)
{
  os << rhs.toString();
  return os;
}

}  // namespace execplan

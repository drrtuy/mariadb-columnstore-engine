/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2016 MariaDB Corporation
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

#pragma once
#include <bitset>
#include <string>
#include <stdint.h>
#include <tr1/unordered_map>
#include <iosfwd>
#include <boost/shared_ptr.hpp>
#include <stack>
#include <vector>
#include "basic/string_utils.h"
#include "idb_mysql.h"
#include "ha_mcs_sysvars.h"

#include "dmlpkg.h"

struct st_ha_create_information;
class ha_columnstore_select_handler;
class ha_columnstore_derived_handler;

#include "basic/string_utils.h"

#include "configcpp.h"
#include "idberrorinfo.h"
#include "calpontselectexecutionplan.h"
#include "windowfunctioncolumn.h"
#include "pseudocolumn.h"
#include "querystats.h"
#include "sm.h"
#include "functor.h"

/** Debug macro */
#ifdef INFINIDB_DEBUG
#define IDEBUG(x) \
  {               \
    x;            \
  }
#else
#define IDEBUG(x) \
  {               \
  }
#endif

namespace execplan
{
class ReturnedColumn;
class SimpleColumn;
class SimpleFilter;
class AggregateColumn;
class FunctionColumn;
class ArithmeticColumn;
class ParseTree;
class ConstantColumn;
class RowColumn;
}  // namespace execplan

namespace cal_impl_if
{
class SubQuery;
class View;

struct JoinInfo
{
  execplan::CalpontSystemCatalog::TableAliasName tn;
  uint32_t joinTimes;
  std::vector<uint32_t> IDs;
};

enum ClauseType
{
  INIT = 0,
  SELECT,
  FROM,
  WHERE,
  HAVING,
  GROUP_BY,
  ORDER_BY
};

struct SchemaAndTableName {
  std::string schema;
  std::string table;
  bool operator==(const SchemaAndTableName& other) const {
    return schema == other.schema && table == other.table;
  }
};

struct SchemaAndTableNameHash {
  std::size_t operator()(const SchemaAndTableName& k) const {
    return std::hash<std::string>()(k.schema + k.table);
  }
};

typedef std::vector<JoinInfo> JoinInfoVec;
typedef dmlpackage::ColValuesList ColValuesList;
typedef dmlpackage::TableValuesMap TableValuesMap;
typedef std::map<execplan::CalpontSystemCatalog::TableAliasName, std::pair<int, TABLE_LIST*>> TableMap;
typedef std::tr1::unordered_map<TABLE_LIST*, std::vector<COND*>> TableOnExprList;
typedef std::tr1::unordered_map<TABLE_LIST*, uint> TableOuterJoinMap;
using ColumnName = std::string;
using ColumnStatisticsMap = std::unordered_map<ColumnName, std::pair<execplan::SimpleColumn, std::vector<Histogram_json_hb>>>;
using TableStatisticsMap = std::unordered_map<SchemaAndTableName, ColumnStatisticsMap, SchemaAndTableNameHash>;

// This structure is used to store MDB AST -> CSEP translation context.
// There is a column statistics for some columns in a query.
// As per 23.10.5 "some" means first column of the index in projection list of CSEP
// satisfies the condition of applyParallelCSEP RBO rule.
// Note that statistics must be merged from subquery/derived table
// to the statistics of the outer query.
struct gp_walk_info
{
  execplan::CalpontSelectExecutionPlan::ReturnedColumnList returnedCols;
  execplan::CalpontSelectExecutionPlan::ReturnedColumnList groupByCols;
  execplan::CalpontSelectExecutionPlan::ReturnedColumnList subGroupByCols;
  execplan::CalpontSelectExecutionPlan::ReturnedColumnList orderByCols;
  std::vector<Item*> extSelAggColsItems;
  execplan::CalpontSelectExecutionPlan::ColumnMap columnMap;
  TableStatisticsMap tableStatisticsMap;
  // This vector temporarily hold the projection columns to be added
  // to the returnedCols vector for subquery processing. It will be appended
  // to the end of returnedCols when the processing is finished.
  execplan::CalpontSelectExecutionPlan::ReturnedColumnList additionalRetCols;
  // the sequence # of the subselect local columns need to be set after
  // the additionRetCols are merged to the returnedCols.
  std::vector<execplan::ReturnedColumn*> localCols;
  std::stack<execplan::ReturnedColumn*> rcWorkStack;
  std::stack<execplan::ParseTree*> ptWorkStack;
  boost::shared_ptr<execplan::SimpleColumn> scsp;  // while defined as SSCP, it is used as SRCP, nothing
                                                   // specific to SimpleColumn is used in use sites.
  uint32_t sessionid;
  bool fatalParseError;
  std::string parseErrorText;
  // for outer join walk. the column that is not of the outerTable has the returnAll flag set.
  std::set<execplan::CalpontSystemCatalog::TableAliasName> innerTables;
  // the followinig members are used for table mode
  bool condPush;
  bool dropCond;
  std::string funcName;
  std::vector<execplan::AggregateColumn*> count_asterisk_list;
  std::vector<execplan::FunctionColumn*> no_parm_func_list;
  std::vector<execplan::ReturnedColumn*> windowFuncList;
  TableMap tableMap;
  boost::shared_ptr<execplan::CalpontSystemCatalog> csc;
  int8_t internalDecimalScale;
  THD* thd;
  uint64_t subSelectType;  // the type of sub select filter that owns the gwi
  SubQuery* subQuery;
  execplan::CalpontSelectExecutionPlan::SelectList derivedTbList;
  execplan::CalpontSelectExecutionPlan::TableList tbList;
  std::vector<execplan::CalpontSystemCatalog::TableAliasName> correlatedTbNameVec;
  ClauseType clauseType;
  execplan::CalpontSystemCatalog::TableAliasName viewName;
  // we can have explicit GROUP BY and implicit one, triggered by aggregate in pojection or ORDER BY.
  // this flag tells us whether we have either case.
  bool implicitExplicitGroupBy;
  bool disableWrapping;
  bool aggOnSelect;
  bool hasWindowFunc;
  bool hasSubSelect;
  SubQuery* lastSub;
  std::vector<View*> viewList;
  std::map<std::string, execplan::ParseTree*> derivedTbFilterMap;
  uint32_t derivedTbCnt;
  std::vector<execplan::SCSEP> subselectList;
  // Workaround for duplicate equi-JOIN predicates
  // See isDuplicateSF() for more info.
  List<execplan::SimpleFilter> equiCondSFList;

  // Kludge for Bug 750
  int32_t recursionLevel;
  int32_t recursionHWM;
  std::stack<int32_t> rcBookMarkStack;

  // Kludge for MCOL-1472
  bool inCaseStmt;
  bool cs_vtable_is_update_with_derive;
  bool cs_vtable_impossible_where_on_union;

  long timeZone;

  // MCOL-4617 The below 2 fields are used for in-to-exists
  // predicate creation and injection. See usage in InSub::transform()
  // and buildInToExistsFilter()
  execplan::ReturnedColumn* inSubQueryLHS;
  Item* inSubQueryLHSItem;

  // The below 2 fields are required for MCOL-4525.
  TableOnExprList tableOnExprList;
  std::vector<COND*> condList;

  // Item* associated with returnedCols.
  std::vector<std::pair<Item*, uint32_t>> processed;

  // SELECT_LEX is needed for aggergate wrapping
  SELECT_LEX* select_lex;

  // we are processing HAVING despite having (pun not intented) clauseType equal to SELECT.
  bool havingDespiteSelect;

  // All SubQuery allocations are single-linked into this chain.
  // At the end of gp_walk_info processing we can free whole chain at once.
  // This is done so because the juggling of SubQuery pointers in the
  // ha_mcs_execplan code.
  // There is a struct SubQueryChainHolder down below to hold chain root and free
  // the chain using sorta kinda RAII.
  SubQuery** subQueriesChain;

  gp_walk_info(long timeZone_, SubQuery** subQueriesChain_)
   : tableStatisticsMap({})
   , sessionid(0)
   , fatalParseError(false)
   , condPush(false)
   , dropCond(false)
   , internalDecimalScale(4)
   , thd(nullptr)
   , subSelectType(uint64_t(-1))
   , subQuery(nullptr)
   , clauseType(INIT)
   , implicitExplicitGroupBy(false)
   , disableWrapping(false)
   , aggOnSelect(false)
   , hasWindowFunc(false)
   , hasSubSelect(false)
   , lastSub(nullptr)
   , derivedTbCnt(0)
   , recursionLevel(-1)
   , recursionHWM(0)
   , inCaseStmt(false)
   , cs_vtable_is_update_with_derive(false)
   , cs_vtable_impossible_where_on_union(false)
   , timeZone(timeZone_)
   , inSubQueryLHS(nullptr)
   , inSubQueryLHSItem(nullptr)
   , select_lex(nullptr)
   , havingDespiteSelect(false)
   , subQueriesChain(subQueriesChain_)
  {
  }
  ~gp_walk_info();

  void mergeTableStatistics(const TableStatisticsMap& tableStatisticsMap);
  std::optional<ColumnStatisticsMap> findStatisticsForATable(SchemaAndTableName& schemaAndTableName);
};

struct SubQueryChainHolder;
struct ext_cond_info
{
  // having this as a direct field would introduce
  // circular dependency on header inclusion with ha_subquery.h.
  boost::shared_ptr<SubQueryChainHolder> chainHolder;
  gp_walk_info gwi;
  ext_cond_info(long timeZone);  // needs knowledge on SubQueryChainHolder, will be defined elsewhere
};

struct cal_table_info
{
  enum RowSources
  {
    FROM_ENGINE,
    FROM_FILE
  };

  cal_table_info()
   : tpl_ctx(nullptr), c(0), msTablePtr(nullptr), conn_hndl(nullptr), condInfo(nullptr), moreRows(false)
  {
  }
  sm::sp_cpsm_tplh_t tpl_ctx;
  std::stack<sm::sp_cpsm_tplh_t> tpl_ctx_st;
  sm::sp_cpsm_tplsch_t tpl_scan_ctx;
  std::stack<sm::sp_cpsm_tplsch_t> tpl_scan_ctx_st;
  unsigned c;         // for debug purpose
  TABLE* msTablePtr;  // no ownership
  sm::cpsm_conhdl_t* conn_hndl;
  ext_cond_info* condInfo;
  execplan::SCSEP csep;
  bool moreRows;  // are there more rows to consume (b/c of limit)
};

typedef std::tr1::unordered_map<TABLE*, cal_table_info> CalTableMap;
typedef std::vector<std::string> ColNameList;
typedef std::bitset<4096> NullValuesBitset;
struct cal_connection_info
{
  enum AlterTableState
  {
    NOT_ALTER,
    ALTER_SECOND_RENAME,
    ALTER_FIRST_RENAME
  };
  cal_connection_info()
   : cal_conn_hndl(nullptr)
   , queryState(0)
   , currentTable(0)
   , traceFlags(0)
   , alterTableState(NOT_ALTER)
   , isAlter(false)
   , bulkInsertRows(0)
   , singleInsert(true)
   , isLoaddataInfile(false)
   , isCacheInsert(false)
   , dmlProc(nullptr)
   , rowsHaveInserted(0)
   , rc(0)
   , tableOid(0)
   , localPm(-1)
   , isSlaveNode(false)
   , expressionId(0)
   , mysqld_pid(getpid())
   , cpimport_pid(0)
   , filePtr(nullptr)
   , headerLength(0)
   , useXbit(false)
   , useCpimport(mcs_use_import_for_batchinsert_mode_t::ON)
   , delimiter('\7')
   , affectedRows(0)
  {
    auto* cf = config::Config::makeConfig();
    if (checkQueryStats(cf))
      traceFlags |= execplan::CalpontSelectExecutionPlan::TRACE_LOG;
    // check if this is a slave mysql daemon
    isSlaveNode = checkSlave(cf);
  }

  bool checkQueryStats(config::Config* cfg)
  {
    std::string qsVal = cfg->getConfig("QueryStats", "Enabled");
    if (qsVal == "Y" || qsVal == "Y")
      return true;

    return false;
  }

  static bool checkSlave(config::Config* cf = nullptr)
  {
    if (!cf)
      cf = config::Config::makeConfig();
    std::string configVal = cf->getConfig("Installation", "MySQLRep");
    bool isMysqlRep = (configVal == "y" || configVal == "Y");

    if (!isMysqlRep)
      return false;

    configVal = cf->getConfig("SystemConfig", "PrimaryUMModuleName");
    std::string module = execplan::ClientRotator::getModule();

    if (datatypes::ASCIIStringCaseInsensetiveEquals(configVal, module))
      return false;

    return true;
  }

  sm::cpsm_conhdl_t* cal_conn_hndl;
  std::stack<sm::cpsm_conhdl_t*> cal_conn_hndl_st;
  int queryState;
  CalTableMap tableMap;
  std::set<TABLE*> physTablesList;
  sm::tableid_t currentTable;
  uint32_t traceFlags;
  std::string queryStats;
  AlterTableState alterTableState;
  bool isAlter;
  ha_rows bulkInsertRows;
  bool singleInsert;
  bool isLoaddataInfile;
  bool isCacheInsert;
  std::string extendedStats;
  std::string miniStats;
  messageqcpp::MessageQueueClient* dmlProc;
  ha_rows rowsHaveInserted;
  ColNameList colNameList;
  TableValuesMap tableValuesMap;
  NullValuesBitset nullValuesBitset;
  int rc;
  uint32_t tableOid;
  querystats::QueryStats stats;
  std::string warningMsg;
  int64_t localPm;
  bool isSlaveNode;
  uint32_t expressionId;  // for F&E
  pid_t mysqld_pid;
  pid_t cpimport_pid;
  int fdt[2];
  FILE* filePtr;
  uint8_t headerLength;
  bool useXbit;
  mcs_use_import_for_batchinsert_mode_t useCpimport;
  char delimiter;
  char enclosed_by;
  std::vector<execplan::CalpontSystemCatalog::ColType> columnTypes;
  // MCOL-1101 remove compilation unit variable rmParms
  std::vector<execplan::RMParam> rmParms;
  long long affectedRows;
};

const std::string infinidb_err_msg =
    "\nThe query includes syntax that is not supported by MariaDB Columnstore. Use 'show warnings;' to get "
    "more information. Review the MariaDB Columnstore Syntax guide for additional information on supported "
    "distributed syntax or consider changing the MariaDB Columnstore Operating Mode (infinidb_vtable_mode).";

int cp_get_plan(THD* thd, execplan::SCSEP& csep);
int cp_get_table_plan(THD* thd, execplan::SCSEP& csep, cal_impl_if::cal_table_info& ti, long timeZone);
int cs_get_derived_plan(ha_columnstore_derived_handler* handler, THD* thd, execplan::SCSEP& csep,
                        gp_walk_info& gwi);
int cs_get_select_plan(ha_columnstore_select_handler* handler, THD* thd, execplan::SCSEP& csep,
                       gp_walk_info& gwi, bool isSelectLexUnit);
int getSelectPlan(gp_walk_info& gwi, SELECT_LEX& select_lex, execplan::SCSEP& csep, bool isUnion = false,
                  bool isSelectHandlerTop = false, bool isSelectLexUnit = false,
                  const std::vector<COND*>& condStack = std::vector<COND*>());
void setError(THD* thd, uint32_t errcode, const std::string errmsg, gp_walk_info* gwi);
void setError(THD* thd, uint32_t errcode, const std::string errmsg);
void gp_walk(const Item* item, void* arg);
void clearDeleteStacks(gp_walk_info& gwi);
void parse_item(Item* item, std::vector<Item_field*>& field_vec, bool& hasNonSupportItem, uint16& parseInfo,
                gp_walk_info* gwip = nullptr);
const std::string bestTableName(const Item_field* ifp);

// execution plan util functions prototypes
execplan::ReturnedColumn* buildReturnedColumn(Item* item, gp_walk_info& gwi, bool& nonSupport,
                                              bool isRefItem = false);
execplan::ReturnedColumn* buildFunctionColumn(Item_func* item, gp_walk_info& gwi, bool& nonSupport,
                                              bool selectBetweenIn = false);
execplan::ReturnedColumn* buildArithmeticColumn(Item_func* item, gp_walk_info& gwi, bool& nonSupport);
execplan::ConstantColumn* buildDecimalColumn(const Item* item, const std::string& str, gp_walk_info& gwi);
execplan::SimpleColumn* buildSimpleColumn(Item_field* item, gp_walk_info& gwi);
execplan::FunctionColumn* buildCaseFunction(Item_func* item, gp_walk_info& gwi, bool& nonSupport);
execplan::ParseTree* buildParseTree(Item* item, gp_walk_info& gwi, bool& nonSupport);
execplan::ReturnedColumn* buildAggregateColumn(Item* item, gp_walk_info& gwi);
execplan::ReturnedColumn* buildWindowFunctionColumn(Item* item, gp_walk_info& gwi, bool& nonSupport);
execplan::ReturnedColumn* buildPseudoColumn(Item* item, gp_walk_info& gwi, bool& nonSupport,
                                            uint32_t pseudoType);
void addIntervalArgs(gp_walk_info* gwip, Item_func* ifp, funcexp::FunctionParm& functionParms);
void castCharArgs(gp_walk_info* gwip, Item_func* ifp, funcexp::FunctionParm& functionParms);
void castDecimalArgs(gp_walk_info* gwip, Item_func* ifp, funcexp::FunctionParm& functionParms);
void castTypeArgs(gp_walk_info* gwip, Item_func* ifp, funcexp::FunctionParm& functionParms);
// void parse_item (Item* item, std::vector<Item_field*>& field_vec, bool& hasNonSupportItem, uint16&
// parseInfo);
bool isPredicateFunction(Item* item, gp_walk_info* gwip);
execplan::ParseTree* buildRowPredicate(gp_walk_info* gwip, execplan::RowColumn* lhs, execplan::RowColumn* rhs,
                                       std::string predicateOp);
bool buildRowColumnFilter(gp_walk_info* gwip, execplan::RowColumn* rhs, execplan::RowColumn* lhs,
                          Item_func* ifp);
bool buildPredicateItem(Item_func* ifp, gp_walk_info* gwip);
void collectAllCols(gp_walk_info& gwi, Item_field* ifp);
void buildSubselectFunc(Item_func* ifp, gp_walk_info* gwip);
uint32_t buildJoin(gp_walk_info& gwi, List<TABLE_LIST>& join_list,
                   std::stack<execplan::ParseTree*>& outerJoinStack);
std::string getViewName(TABLE_LIST* table_ptr);
bool buildConstPredicate(Item_func* ifp, execplan::ReturnedColumn* rhs, gp_walk_info* gwip);
execplan::CalpontSystemCatalog::ColType fieldType_MysqlToIDB(const Field* field);
execplan::CalpontSystemCatalog::ColType colType_MysqlToIDB(const Item* item);
execplan::SPTP getIntervalType(gp_walk_info* gwip, int interval_type);
uint32_t isPseudoColumn(std::string funcName);
void setDerivedTable(execplan::ParseTree* n);
execplan::ParseTree* setDerivedFilter(gp_walk_info* gwip, execplan::ParseTree*& n,
                                      std::map<std::string, execplan::ParseTree*>& obj,
                                      execplan::CalpontSelectExecutionPlan::SelectList& derivedTbList);
void derivedTableOptimization(gp_walk_info* gwip, execplan::SCSEP& csep);
bool buildEqualityPredicate(execplan::ReturnedColumn* lhs, execplan::ReturnedColumn* rhs, gp_walk_info* gwip,
                            boost::shared_ptr<execplan::Operator>& sop, const Item_func::Functype& funcType,
                            const std::vector<Item*>& itemList, bool isInSubs = false);

#ifdef DEBUG_WALK_COND
void debug_walk(const Item* item, void* arg);
#endif
}  // namespace cal_impl_if

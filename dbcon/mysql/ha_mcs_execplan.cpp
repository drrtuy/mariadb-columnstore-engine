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

#include <strings.h>

#include <string>
#include <iostream>
#include <stack>
#include <tr1/unordered_map>
#include <tr1/unordered_set>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <time.h>
#include <cassert>
#include <vector>
#include <map>
#include <limits>
#include "messagelog.h"

#include <string.h>

using namespace std;

#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/thread.hpp>

#include "errorids.h"
using namespace logging;

#define PREFER_MY_CONFIG_H
#include <my_config.h>
#include "idb_mysql.h"

#include "partition_element.h"
#include "partition_info.h"

#include "mcsv1_udaf.h"

#include "ha_mcs_execplan_walks.h"
#include "ha_mcs_execplan_parseinfo_bits.h"
#include "ha_mcs_execplan_helpers.h"
#include "ha_mcs_impl_if.h"
#include "ha_mcs_sysvars.h"
#include "ha_subquery.h"
#include "ha_mcs_pushdown.h"
#include "ha_tzinfo.h"
#include "ha_mcs_logging.h"
using namespace cal_impl_if;

#include "aggregatecolumn.h"
#include "arithmeticcolumn.h"
#include "arithmeticoperator.h"
#include "calpontselectexecutionplan.h"
#include "calpontsystemcatalog.h"
#include "constantcolumn.h"
#include "constantfilter.h"
#include "existsfilter.h"
#include "functioncolumn.h"
#include "groupconcatcolumn.h"
#include "intervalcolumn.h"
#include "logicoperator.h"
#include "outerjoinonfilter.h"
#include "predicateoperator.h"
#include "rewrites.h"
#include "rowcolumn.h"
#include "rulebased_optimizer.h"
#include "selectfilter.h"
#include "simplecolumn_decimal.h"
#include "simplecolumn_int.h"
#include "simplecolumn_uint.h"
#include "simplefilter.h"
#include "udafcolumn.h"
using namespace execplan;

#include "funcexp.h"
#include "functor.h"
using namespace funcexp;

#include "vlarray.h"

#include "ha_view.h"

namespace cal_impl_if
{
// This is taken from Item_cond::fix_fields in sql/item_cmpfunc.cc.
void calculateNotNullTables(const std::vector<COND*>& condList, table_map& not_null_tables)
{
  for (Item* item : condList)
  {
    if (item->can_eval_in_optimize() && !item->with_sp_var() && !cond_has_datetime_is_null(item))
    {
      if (item->eval_const_cond())
      {
      }
      else
      {
        not_null_tables = (table_map)0;
      }
    }
    else
    {
      not_null_tables |= item->not_null_tables();
    }
  }
}

bool itemDisablesWrapping(Item* item, gp_walk_info& gwi);

void pushReturnedCol(gp_walk_info& gwi, Item* from, SRCP rc)
{
  uint32_t i;
  for (i = 0; i < gwi.processed.size(); i++)
  {
    Item* ith = gwi.processed[i].first;

    // made within MCOL-5776 produced bug MCOL-5932 so, the check of equal columns is disabled
    // FIXME: enable the check of equal columns
    // bool same = ith->eq(from, false);
    bool same = false;

    if (same && ith->type() == Item::FUNC_ITEM)
    {
      // an exception for cast(column as decimal(X,Y)) - they are equal (in the eq() call sense)
      // even if Xs and Ys are different.
      string funcName = ((Item_func*)ith)->func_name();

      if (funcName == "decimal_typecast")
      {
        Item_decimal_typecast* ithdtc = (Item_decimal_typecast*)ith;
        Item_decimal_typecast* fromdtc = (Item_decimal_typecast*)from;
        same = ithdtc->decimals == fromdtc->decimals && ithdtc->max_length == fromdtc->max_length;
      }
    }
    if (same)
    {
      break;
    }
  }
  bool needChange = true;
  if (dynamic_cast<SimpleColumn*>(rc.get()) == nullptr && gwi.select_lex && !itemDisablesWrapping(from, gwi))
  {
    needChange = false;
  }
  if (needChange && i < gwi.processed.size())
  {
    rc->expressionId(gwi.processed[i].second);
  }
  else
  {
    gwi.processed.push_back(std::make_pair(from, rc->expressionId()));
  }
  gwi.returnedCols.push_back(rc);
}

// Recursively iterate through the join_list and store all non-null
// TABLE_LIST::on_expr items to a hash map keyed by the TABLE_LIST ptr.
// This is then used by convertOuterJoinToInnerJoin().
void buildTableOnExprList(List<TABLE_LIST>* join_list, TableOnExprList& tableOnExprList)
{
  TABLE_LIST* table;
  NESTED_JOIN* nested_join;
  List_iterator<TABLE_LIST> li(*join_list);

  while ((table = li++))
  {
    if ((nested_join = table->nested_join))
    {
      buildTableOnExprList(&nested_join->join_list, tableOnExprList);
    }

    if (table->on_expr)
      tableOnExprList[table].push_back(table->on_expr);
  }
}

// This is a trimmed down version of simplify_joins() in sql/sql_select.cc.
// Refer to that function for more details. But we have customized the
// original implementation in simplify_joins() to avoid any changes to
// the SELECT_LEX::JOIN::conds. Specifically, in some cases, simplify_joins()
// would create new Item_cond_and objects. We want to avoid such changes to
// the SELECT_LEX in a scenario where the select_handler execution has failed
// and we want to fallback to the server execution (MCOL-4525). Here, we mimick
// the creation of Item_cond_and using tableOnExprList and condList.
void convertOuterJoinToInnerJoin(List<TABLE_LIST>* join_list, TableOnExprList& tableOnExprList,
                                 std::vector<COND*>& condList, TableOuterJoinMap& tableOuterJoinMap)
{
  TABLE_LIST* table;
  NESTED_JOIN* nested_join;
  List_iterator<TABLE_LIST> li(*join_list);

  while ((table = li++))
  {
    table_map used_tables;
    table_map not_null_tables = (table_map)0;

    if ((nested_join = table->nested_join))
    {
      auto iter = tableOnExprList.find(table);

      if ((iter != tableOnExprList.end()) && !iter->second.empty())
      {
        convertOuterJoinToInnerJoin(&nested_join->join_list, tableOnExprList, tableOnExprList[table],
                                    tableOuterJoinMap);
      }

      nested_join->used_tables = (table_map)0;
      nested_join->not_null_tables = (table_map)0;

      convertOuterJoinToInnerJoin(&nested_join->join_list, tableOnExprList, condList, tableOuterJoinMap);

      used_tables = nested_join->used_tables;
      not_null_tables = nested_join->not_null_tables;
    }
    else
    {
      used_tables = table->get_map();

      if (!condList.empty())
      {
        if (condList.size() == 1)
        {
          not_null_tables = condList[0]->not_null_tables();
        }
        else
        {
          calculateNotNullTables(condList, not_null_tables);
        }
      }
    }

    if (table->embedding)
    {
      table->embedding->nested_join->used_tables |= used_tables;
      table->embedding->nested_join->not_null_tables |= not_null_tables;
    }

    if (!(table->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT)) || (used_tables & not_null_tables))
    {
      if (table->outer_join)
      {
        tableOuterJoinMap[table] = table->outer_join;
      }

      table->outer_join = 0;

      auto iter = tableOnExprList.find(table);

      if (iter != tableOnExprList.end() && !iter->second.empty())
      {
        // The original implementation in simplify_joins() creates
        // an Item_cond_and object here. Instead of doing so, we
        // append the table->on_expr to the condList and hence avoid
        // making any permanent changes to SELECT_LEX::JOIN::conds.
        condList.insert(condList.end(), tableOnExprList[table].begin(), tableOnExprList[table].end());
        iter->second.clear();
      }
    }
  }
}

static execplan::Partitions getPartitions(TABLE* table)
{
  execplan::Partitions result;

  if (table->part_info)
  {
    List_iterator<partition_element> part_el_it(table->part_info->partitions);

    partition_element* pe;

    while ((pe = part_el_it++))  // this is how server does it.
    {
// TODO: partition names are not just strings in
#if MYSQL_VERSION_ID >= 110501
      result.fPartNames.emplace_back(pe->partition_name.str);
#else
      result.fPartNames.emplace_back(pe->partition_name);
#endif
    }
  }
  return result;
}
static execplan::Partitions getPartitions(TABLE_LIST* table)
{
  execplan::Partitions result;

  if (table->partition_names)
  {
    List_iterator<String> part_name_it(*(table->partition_names));

    String* n;

    while ((n = part_name_it++))  // this is how server does it.
    {
      std::string pn(n->ptr(), n->length());
      result.fPartNames.push_back(pn);
    }
  }
  return result;
}

CalpontSystemCatalog::TableAliasName makeTableAliasName_(TABLE_LIST* table)
{
  return make_aliasview(
      (table->db.length ? table->db.str : ""), (table->table_name.length ? table->table_name.str : ""),
      (table->alias.length ? table->alias.str : ""), getViewName(table), true, lower_case_table_names);
}

CalpontSystemCatalog::TableAliasName makeTableAliasName(TABLE_LIST* table)
{
  CalpontSystemCatalog::TableAliasName result = makeTableAliasName_(table);
  result.partitions = getPartitions(table);
  return result;
}

//@bug5228. need to escape backtick `
string escapeBackTick(const char* str)
{
  if (!str)
    return "";

  string ret;

  for (uint32_t i = 0; str[i] != 0; i++)
  {
    if (str[i] == '`')
      ret.append("``");
    else
      ret.append(1, str[i]);
  }

  return ret;
}

cal_impl_if::gp_walk_info::~gp_walk_info()
{
  while (!rcWorkStack.empty())
  {
    delete rcWorkStack.top();
    rcWorkStack.pop();
  }

  while (!ptWorkStack.empty())
  {
    delete ptWorkStack.top();
    ptWorkStack.pop();
  }
  for (uint32_t i = 0; i < viewList.size(); i++)
  {
    delete viewList[i];
  }
  viewList.clear();
}

void clearStacks(gp_walk_info& gwi, bool andViews = true, bool mayDelete = false)
{
  while (!gwi.rcWorkStack.empty())
  {
    if (mayDelete)
    {
      delete gwi.rcWorkStack.top();
    }
    gwi.rcWorkStack.pop();
  }

  while (!gwi.ptWorkStack.empty())
  {
    if (mayDelete)
    {
      delete gwi.ptWorkStack.top();
    }
    gwi.ptWorkStack.pop();
  }
  if (andViews)
  {
    gwi.viewList.clear();
  }
}
void clearDeleteStacks(gp_walk_info& gwi)
{
  while (!gwi.rcWorkStack.empty())
  {
    delete gwi.rcWorkStack.top();
    gwi.rcWorkStack.pop();
  }

  while (!gwi.ptWorkStack.empty())
  {
    delete gwi.ptWorkStack.top();
    gwi.ptWorkStack.pop();
  }
  for (uint32_t i = 0; i < gwi.viewList.size(); i++)
  {
    delete gwi.viewList[i];
  }
  gwi.viewList.clear();
}

/*@brief getColNameFromItem - builds a name from an Item    */
/***********************************************************
 * DESCRIPTION:
 * This f() looks for a first proper Item_ident and populate
 * ostream with schema, table and column names.
 * Used to build db.table.field tuple for debugging output
 * in getSelectPlan().
 * PARAMETERS:
 *   item               source Item*
 *   ostream            output stream
 * RETURNS
 *  void
 ***********************************************************/
void getColNameFromItem(std::ostringstream& ostream, Item* item)
{
  // Item_func doesn't have proper db.table.field values
  // inherited from Item_ident. TBD what is the valid output.
  // !!!dynamic_cast fails compilation
  ostream << "'";

  if (item->type() != Item::FIELD_ITEM)
  {
    ostream << "unknown db" << '.';
    ostream << "unknown table" << '.';
    ostream << "unknown field";
  }
  else
  {
    Item_ident* iip = static_cast<Item_ident*>(item);

    if (iip->db_name.str)
      ostream << iip->db_name.str << '.';
    else
      ostream << "unknown db" << '.';

    if (iip->table_name.str)
      ostream << iip->table_name.str << '.';
    else
      ostream << "unknown table" << '.';

    if (iip->field_name.length)
      ostream << iip->field_name.str;
    else
      ostream << "unknown field";
  }

  ostream << "'";
  return;
}

/*@brf sortItemIsInGroupRec - seeks for an item in grouping*/
/***********************************************************
 * DESCRIPTION:
 * This f() recursively traverses grouping items and looks
 * for an FUNC_ITEM, REF_ITEM or FIELD_ITEM.
 * f() is used by sortItemIsInGrouping().
 * PARAMETERS:
 *   sort_item          Item* used to build aggregation.
 *   group_item         GROUP BY item.
 * RETURNS
 *  bool
 ***********************************************************/
bool sortItemIsInGroupRec(Item* sort_item, Item* group_item)
{
  bool found = false;
  // If ITEM_REF::ref is NULL
  if (sort_item == NULL)
  {
    return found;
  }

  // base cases for Item_field and Item_ref. The second arg is binary cmp switch
  found = group_item->eq(sort_item, false);
  if (!found && sort_item->type() == Item::REF_ITEM)
  {
    Item_ref* ifp_sort_ref = static_cast<Item_ref*>(sort_item);
    found = sortItemIsInGroupRec(*ifp_sort_ref->ref, group_item);
  }
  else if (sort_item->type() == Item::FIELD_ITEM)
  {
    return found;
  }

  Item_func* ifp_sort = static_cast<Item_func*>(sort_item);

  // seeking for a group_item match
  for (uint32_t i = 0; !found && i < ifp_sort->argument_count(); i++)
  {
    Item* ifp_sort_arg = ifp_sort->arguments()[i];
    if (ifp_sort_arg->type() == Item::FUNC_ITEM || ifp_sort_arg->type() == Item::FIELD_ITEM)
    {
      Item* ifp_sort_arg = ifp_sort->arguments()[i];
      found = sortItemIsInGroupRec(ifp_sort_arg, group_item);
    }
    else if (ifp_sort_arg->type() == Item::REF_ITEM)
    {
      // dereference the Item
      Item_ref* ifp_sort_ref = static_cast<Item_ref*>(ifp_sort_arg);
      found = sortItemIsInGroupRec(*ifp_sort_ref->ref, group_item);
    }
  }

  return found;
}

/*@brief check_sum_func_item - This traverses Item       */
/**********************************************************
 * DESCRIPTION:
 * This f() walks Item looking for the existence of
 * a Item::REF_ITEM, which references an item of
 * type Item::SUM_FUNC_ITEM
 * PARAMETERS:
 *    Item * Item to traverse
 * RETURN:
 *********************************************************/
void check_sum_func_item(const Item* item, void* arg)
{
  bool* found = static_cast<bool*>(arg);

  if (*found)
    return;

  if (item->type() == Item::REF_ITEM)
  {
    const Item_ref* ref_item = static_cast<const Item_ref*>(item);
    Item* ref_item_item = (Item*)*ref_item->ref;
    if (ref_item_item->type() == Item::SUM_FUNC_ITEM)
    {
      *found = true;
    }
  }
  else if (item->type() == Item::CONST_ITEM)
  {
    *found = true;
  }
}

/*@brief sortItemIsInGrouping- seeks for an item in grouping*/
/***********************************************************
 * DESCRIPTION:
 * This f() traverses grouping items and looks for an item.
 * only Item_fields, Item_func are considered. However there
 * could be Item_ref down the tree.
 * f() is used in sorting parsing by getSelectPlan().
 * PARAMETERS:
 *   sort_item          Item* used to build aggregation.
 *   groupcol           GROUP BY items from this unit.
 * RETURNS
 *  bool
 ***********************************************************/
bool sortItemIsInGrouping(Item* sort_item, ORDER* groupcol)
{
  bool found = false;

  if (sort_item->type() == Item::SUM_FUNC_ITEM)
  {
    found = true;
    return found;
  }

  {
    // as we now can warp ORDER BY or SELECT expression into
    // an aggregate, we can pass FIELD_ITEM as "found" as well.
    Item* item = sort_item;
    while (item->type() == Item::REF_ITEM)
    {
      const Item_ref* ref_item = static_cast<const Item_ref*>(item);
      item = (Item*)*ref_item->ref;
    }
    if (item->type() == Item::FIELD_ITEM || item->type() == Item::CONST_ITEM ||
        item->type() == Item::NULL_ITEM)
    {
      return true;
    }
  }

  // A function that contains an aggregate function
  // can be included in the ORDER BY clause
  // e.g. select a, if (sum(b) > 1, 2, 1) from t1 group by 1 order by 2;
  if (sort_item->type() == Item::FUNC_ITEM)
  {
    Item_func* ifp = static_cast<Item_func*>(sort_item);
    ifp->traverse_cond(check_sum_func_item, &found, Item::POSTFIX);
  }
  else if (sort_item->type() == Item::CONST_ITEM || sort_item->type() == Item::WINDOW_FUNC_ITEM)
  {
    found = true;
  }

  for (; !found && groupcol; groupcol = groupcol->next)
  {
    Item* group_item = *(groupcol->item);
    found = (group_item->eq(sort_item, false)) ? true : false;
    // Detect aggregation functions first then traverse
    // if sort field is a Func and group field
    // is either Field or Func
    // Consider nonConstFunc() check here
    if (!found && sort_item->type() == Item::FUNC_ITEM &&
        (group_item->type() == Item::FUNC_ITEM || group_item->type() == Item::FIELD_ITEM ||
         group_item->type() == Item::REF_ITEM))
    {
      // MCOL-5236: see @bug5993 and @bug5916.
      Item* item = group_item;
      while (item->type() == Item::REF_ITEM)
      {
        Item_ref* item_ref = static_cast<Item_ref*>(item);
        item = *item_ref->ref;
      }

      found = sortItemIsInGroupRec(sort_item, item);
    }
  }

  return found;
}

/*@brief  buildAggFrmTempField- build aggr func from extSELECT list item*/
/***********************************************************
 * DESCRIPTION:
 * Server adds additional aggregation items to extended SELECT list and
 * references them in projection and HAVING. This f() finds
 * corresponding item in extSelAggColsItems and builds
 * ReturnedColumn using the item.
 * PARAMETERS:
 *    item          Item* used to build aggregation
 *    gwi           main structure
 * RETURNS
 *  ReturnedColumn* if corresponding Item has been found
 *  NULL otherwise
 ***********************************************************/
ReturnedColumn* buildAggFrmTempField(Item* item, gp_walk_info& gwi)
{
  ReturnedColumn* result = NULL;
  Item_field* ifip = NULL;
  Item_ref* irip;
  Item_func_or_sum* isfp;
  switch (item->type())
  {
    case Item::FIELD_ITEM: ifip = static_cast<Item_field*>(item); break;
    default:
      irip = static_cast<Item_ref*>(item);
      if (irip)
        ifip = static_cast<Item_field*>(irip->ref[0]);
      break;
  }

  if (ifip && ifip->field)
  {
    std::vector<Item*>::iterator iter = gwi.extSelAggColsItems.begin();
    for (; iter != gwi.extSelAggColsItems.end(); iter++)
    {
      isfp = static_cast<Item_func_or_sum*>(*iter);

      if (isfp->type() == Item::SUM_FUNC_ITEM && isfp->result_field == ifip->field)
      {
        ReturnedColumn* rc = buildAggregateColumn(isfp, gwi);

        if (rc)
          result = rc;

        break;
      }
    }
  }

  return result;
}

/*@brief  isDuplicateSF - search for a duplicate SimpleFilter*/
/***********************************************************
 * DESCRIPTION:
 * As of 1.4 CS potentially has duplicate filter predicates
 * in both join->conds and join->cond_equal. This utility f()
 * searches for semantically equal SF in the list of already
 * applied equi predicates.
 * TODO
 *  We must move find_select_handler to either future or
 *  later execution phase.
 * PARAMETERS:
 *  gwi           main structure
 *  sf            SimpleFilter * to find
 * RETURNS
 *  true          if sf has been found in the applied SF list
 *  false         otherwise
 * USED
 *  buildPredicateItem()
 ***********************************************************/
bool isDuplicateSF(gp_walk_info* gwip, execplan::SimpleFilter* sfp)
{
  List_iterator<execplan::SimpleFilter> it(gwip->equiCondSFList);
  execplan::SimpleFilter* isfp;
  while ((isfp = it++))
  {
    if (sfp->semanticEq(*isfp))
    {
      return true;
    }
  }

  return false;
}

// DESCRIPTION:
// This f() checks if the arguments is a UDF Item
// PARAMETERS:
//    Item * Item to traverse
// RETURN:
//    bool
inline bool isUDFSumItem(const Item_sum* isp)
{
  return typeid(*isp) == typeid(Item_sum_udf_int) || typeid(*isp) == typeid(Item_sum_udf_float) ||
         typeid(*isp) == typeid(Item_sum_udf_decimal) || typeid(*isp) == typeid(Item_sum_udf_str);
}

string getViewName(TABLE_LIST* table_ptr)
{
  string viewName = "";

  if (!table_ptr)
    return viewName;

  TABLE_LIST* view = table_ptr->referencing_view;

  if (view)
  {
    if (!view->derived)
      viewName = view->alias.str;

    while ((view = view->referencing_view))
    {
      if (view->derived)
        continue;

      viewName = view->alias.str + string(".") + viewName;
    }
  }

  return viewName;
}

void buildNestedJoinLeafTables(List<TABLE_LIST>& join_list,
                               std::set<execplan::CalpontSystemCatalog::TableAliasName>& leafTables)
{
  TABLE_LIST* table;
  List_iterator<TABLE_LIST> li(join_list);

  while ((table = li++))
  {
    if (table->nested_join)
      buildNestedJoinLeafTables(table->nested_join->join_list, leafTables);
    else
    {
      CalpontSystemCatalog::TableAliasName tan = makeTableAliasName(table);
      leafTables.insert(tan);
    }
  }
}

uint32_t buildJoin(gp_walk_info& gwi, List<TABLE_LIST>& join_list,
                   std::stack<execplan::ParseTree*>& outerJoinStack)
{
  TABLE_LIST* table;
  List_iterator<TABLE_LIST> li(join_list);

  while ((table = li++))
  {
    // Make sure we don't process the derived table nests again,
    // they were already handled by FromSubQuery::transform()
    if (table->nested_join && !table->derived)
      buildJoin(gwi, table->nested_join->join_list, outerJoinStack);

    std::vector<COND*> tableOnExprList;

    auto iter = gwi.tableOnExprList.find(table);

    // Check if this table's on_expr is available in the hash map
    // built/updated during convertOuterJoinToInnerJoin().
    if ((iter != gwi.tableOnExprList.end()) && !iter->second.empty())
    {
      tableOnExprList = iter->second;
    }
    // This table's on_expr has not been seen/processed before.
    else if ((iter == gwi.tableOnExprList.end()) && table->on_expr)
    {
      tableOnExprList.push_back(table->on_expr);
    }

    if (!tableOnExprList.empty())
    {
      if (table->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT))
      {
        // inner tables block
        gp_walk_info gwi_outer = gwi;
        gwi_outer.subQuery = NULL;
        gwi_outer.hasSubSelect = false;
        gwi_outer.innerTables.clear();
        clearStacks(gwi_outer);

        // recursively build the leaf tables for this nested join node
        if (table->nested_join)
          buildNestedJoinLeafTables(table->nested_join->join_list, gwi_outer.innerTables);
        else  // this is a leaf table
        {
          CalpontSystemCatalog::TableAliasName tan = makeTableAliasName(table);
          gwi_outer.innerTables.insert(tan);
        }

#ifdef DEBUG_WALK_COND
        cerr << "inner tables: " << endl;
        set<CalpontSystemCatalog::TableAliasName>::const_iterator it;
        for (it = gwi_outer.innerTables.begin(); it != gwi_outer.innerTables.end(); ++it)
          cerr << (*it) << " ";
        cerr << endl;

        cerr << " outer table expression: " << endl;

        for (Item* expr : tableOnExprList)
          expr->traverse_cond(debug_walk, &gwi_outer, Item::POSTFIX);
#endif

        for (Item* expr : tableOnExprList)
        {
          expr->traverse_cond(gp_walk, &gwi_outer, Item::POSTFIX);

          // Error out subquery in outer join on filter for now
          if (gwi_outer.hasSubSelect)
          {
            gwi.fatalParseError = true;
            gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_OUTER_JOIN_SUBSELECT);
            setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText);
            return -1;
          }
        }

        // build outerjoinon filter
        ParseTree *filters = NULL, *ptp = NULL, *rhs = NULL;

        while (!gwi_outer.ptWorkStack.empty())
        {
          filters = gwi_outer.ptWorkStack.top();
          gwi_outer.ptWorkStack.pop();

          if (gwi_outer.ptWorkStack.empty())
            break;

          ptp = new ParseTree(new LogicOperator("and"));
          ptp->left(filters);
          rhs = gwi_outer.ptWorkStack.top();
          gwi_outer.ptWorkStack.pop();
          ptp->right(rhs);
          gwi_outer.ptWorkStack.push(ptp);
        }

        // should have only 1 pt left in stack.
        if (filters)
        {
          SPTP on_sp(filters);
          OuterJoinOnFilter* onFilter = new OuterJoinOnFilter(on_sp);
          ParseTree* pt = new ParseTree(onFilter);
          outerJoinStack.push(pt);
        }
      }
      else  // inner join
      {
        for (Item* expr : tableOnExprList)
        {
          expr->traverse_cond(gp_walk, &gwi, Item::POSTFIX);
        }

#ifdef DEBUG_WALK_COND
        cerr << " inner join expression: " << endl;
        for (Item* expr : tableOnExprList)
          expr->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
#endif
      }
    }
  }

  return 0;
}

ParseTree* buildRowPredicate(gp_walk_info* gwip, RowColumn* lhs, RowColumn* rhs, string predicateOp)
{
  PredicateOperator* po = new PredicateOperator(predicateOp);
  boost::shared_ptr<Operator> sop(po);
  LogicOperator* lo = NULL;

  if (predicateOp == "=")
    lo = new LogicOperator("and");
  else
    lo = new LogicOperator("or");

  ParseTree* pt = new ParseTree(lo);
  sop->setOpType(lhs->columnVec()[0]->resultType(), rhs->columnVec()[0]->resultType());
  SimpleFilter* sf = new SimpleFilter(sop, lhs->columnVec()[0]->clone(), rhs->columnVec()[0]->clone());
  sf->timeZone(gwip->timeZone);
  pt->left(new ParseTree(sf));

  for (uint32_t i = 1; i < lhs->columnVec().size(); i++)
  {
    sop.reset(po->clone());
    sop->setOpType(lhs->columnVec()[i]->resultType(), rhs->columnVec()[i]->resultType());
    SimpleFilter* sf = new SimpleFilter(sop, lhs->columnVec()[i]->clone(), rhs->columnVec()[i]->clone());
    sf->timeZone(gwip->timeZone);
    pt->right(new ParseTree(sf));

    if (i + 1 < lhs->columnVec().size())
    {
      ParseTree* lpt = pt;
      pt = new ParseTree(lo->clone());
      pt->left(lpt);
    }
  }

  return pt;
}

bool buildRowColumnFilter(gp_walk_info* gwip, RowColumn* rhs, RowColumn* lhs, Item_func* ifp)
{
  // rhs and lhs are being dismembered here and then get thrown away, leaking.
  // So we create two scoped pointers to get them delete'd automatically at
  // return.
  // Also look below into heldoutVals vector - it contains values produced from
  // ifp's arguments.
  const std::unique_ptr<RowColumn> rhsp(rhs), lhsp(lhs);
  if (ifp->functype() == Item_func::EQ_FUNC || ifp->functype() == Item_func::NE_FUNC)
  {
    // (c1,c2,..) = (v1,v2,...) transform to: c1=v1 and c2=v2 and ...
    assert(!lhs->columnVec().empty() && lhs->columnVec().size() == rhs->columnVec().size());
    gwip->ptWorkStack.push(buildRowPredicate(gwip, rhs, lhs, ifp->func_name()));
  }
  else if (ifp->functype() == Item_func::IN_FUNC)
  {
    // (c1,c2,...) in ((v11,v12,...),(v21,v22,...)...) transform to:
    // ((c1 = v11 and c2 = v12 and ...) or (c1 = v21 and c2 = v22 and ...) or ...)
    // and c1 in (v11,v21,...) and c2 in (v12,v22,...) => to favor CP
    Item_func_opt_neg* inp = (Item_func_opt_neg*)ifp;
    string predicateOp, logicOp;

    if (inp->negated)
    {
      predicateOp = "<>";
      logicOp = "and";
    }
    else
    {
      predicateOp = "=";
      logicOp = "or";
    }

    boost::scoped_ptr<LogicOperator> lo(new LogicOperator(logicOp));

    // 1st round. build the equivalent filters
    // two entries have been popped from the stack already: lhs and rhs
    stack<ReturnedColumn*> tmpStack;
    vector<RowColumn*> valVec;
    vector<SRCP> heldOutVals;  // these vals are not rhs/lhs and need to be freed
    tmpStack.push(rhs);
    tmpStack.push(lhs);
    assert(gwip->rcWorkStack.size() >= ifp->argument_count() - 2);

    for (uint32_t i = 2; i < ifp->argument_count(); i++)
    {
      tmpStack.push(gwip->rcWorkStack.top());
      heldOutVals.push_back(SRCP(gwip->rcWorkStack.top()));

      if (!gwip->rcWorkStack.empty())
        gwip->rcWorkStack.pop();
    }

    RowColumn* columns = dynamic_cast<RowColumn*>(tmpStack.top());
    tmpStack.pop();
    RowColumn* vals = dynamic_cast<RowColumn*>(tmpStack.top());
    valVec.push_back(vals);
    tmpStack.pop();
    ParseTree* pt = buildRowPredicate(gwip, columns, vals, predicateOp);

    while (!tmpStack.empty())
    {
      ParseTree* pt1 = new ParseTree(lo->clone());
      pt1->left(pt);
      vals = dynamic_cast<RowColumn*>(tmpStack.top());
      valVec.push_back(vals);
      tmpStack.pop();
      pt1->right(buildRowPredicate(gwip, columns, vals, predicateOp));
      pt = pt1;
    }

    gwip->ptWorkStack.push(pt);

    // done for NOTIN clause
    if (predicateOp == "<>")
      return true;

    // 2nd round. add the filter to favor casual partition for IN clause
    boost::shared_ptr<Operator> sop;
    boost::shared_ptr<SimpleColumn> ssc;
    stack<ParseTree*> tmpPtStack;

    for (uint32_t i = 0; i < columns->columnVec().size(); i++)
    {
      std::unique_ptr<ConstantFilter> cf(new ConstantFilter());

      sop.reset(lo->clone());
      cf->op(sop);
      SimpleColumn* sc = dynamic_cast<SimpleColumn*>(columns->columnVec()[i].get());

      // no optimization for non-simple column because CP won't apply
      if (!sc)
      {
        continue;
      }

      ssc.reset(sc->clone());
      cf->col(ssc);

      uint32_t j = 0;

      for (; j < valVec.size(); j++)
      {
        sop.reset(new PredicateOperator(predicateOp));
        ConstantColumn* cc = dynamic_cast<ConstantColumn*>(valVec[j]->columnVec()[i].get());

        // no optimization for non-constant value because CP won't apply
        if (!cc)
          break;

        sop->setOpType(sc->resultType(), valVec[j]->columnVec()[i]->resultType());
        cf->pushFilter(
            new SimpleFilter(sop, sc->clone(), valVec[j]->columnVec()[i]->clone(), gwip->timeZone));
      }

      if (j < valVec.size())
      {
        continue;
      }

      tmpPtStack.push(new ParseTree(cf.release()));
    }

    // "and" all the filters together
    ParseTree* ptp = NULL;
    pt = NULL;

    while (!tmpPtStack.empty())
    {
      pt = tmpPtStack.top();
      tmpPtStack.pop();

      if (tmpPtStack.empty())
        break;

      ptp = new ParseTree(new LogicOperator("and"));
      ptp->left(pt);
      pt = tmpPtStack.top();
      tmpPtStack.pop();
      ptp->right(pt);
      tmpPtStack.push(ptp);
    }

    if (pt)
    {
      tmpPtStack.push(pt);

      // AND with the pt built from the first round
      if (!gwip->ptWorkStack.empty() && !tmpPtStack.empty())
      {
        pt = new ParseTree(new LogicOperator("and"));
        pt->left(gwip->ptWorkStack.top());
        gwip->ptWorkStack.pop();
        pt->right(tmpPtStack.top());
        tmpPtStack.pop();
      }

      // Push the final pt tree for this IN clause
      gwip->ptWorkStack.push(pt);
    }
  }
  else
  {
    gwip->fatalParseError = true;
    gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FUNC_MULTI_COL);
    return false;
  }

  return true;
}

void checkOuterTableColumn(gp_walk_info* gwip, const CalpontSystemCatalog::TableAliasName& tan,
                           execplan::ReturnedColumn* col)
{
  set<CalpontSystemCatalog::TableAliasName>::const_iterator it;

  bool notInner = true;

  for (it = gwip->innerTables.begin(); it != gwip->innerTables.end(); ++it)
  {
    if (tan.alias == it->alias && tan.view == it->view)
      notInner = false;
  }

  if (notInner)
  {
    col->returnAll(true);
    IDEBUG(cerr << "setting returnAll on " << tan << endl);
  }
}

bool buildEqualityPredicate(execplan::ReturnedColumn* lhs, execplan::ReturnedColumn* rhs, gp_walk_info* gwip,
                            boost::shared_ptr<Operator>& sop, const Item_func::Functype& funcType,
                            const vector<Item*>& itemList, bool isInSubs)
{
  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  // push the column that is associated with the correlated column to the returned
  // column list, so the materialized view have the complete projection list.
  // e.g. tout.c1 in (select tin.c1 from tin where tin.c2=tout.c2);
  // the projetion list of subquery will have tin.c1, tin.c2.
  ReturnedColumn* correlatedCol = nullptr;
  ReturnedColumn* localCol = nullptr;

  if (rhs->joinInfo() & JOIN_CORRELATED)
  {
    correlatedCol = rhs;
    localCol = lhs;
  }
  else if (lhs->joinInfo() & JOIN_CORRELATED)
  {
    correlatedCol = lhs;
    localCol = rhs;
  }

  if (correlatedCol && localCol)
  {
    ConstantColumn* cc = dynamic_cast<ConstantColumn*>(localCol);

    if ((!cc || (cc && funcType == Item_func::EQ_FUNC)) && !(localCol->joinInfo() & JOIN_CORRELATED))
    {
      if (isInSubs)
        localCol->sequence(0);
      else
        localCol->sequence(gwip->returnedCols.size());

      localCol->expressionId(ci->expressionId++);
      ReturnedColumn* rc = localCol->clone();
      rc->colSource(rc->colSource() | CORRELATED_JOIN);
      gwip->additionalRetCols.push_back(SRCP(rc));
      gwip->localCols.push_back(localCol);

      if (rc->hasWindowFunc() && !isInSubs)
        gwip->windowFuncList.push_back(rc);
    }

    // push the correlated join partner to the group by list only when there's aggregate
    // and we don't push aggregate column to the group by
    // @bug4756. mysql does not always give correct information about whether there is
    // aggregate on the SELECT list. Need to figure that by ourselves and then decide
    // to add the group by or not.
    if (gwip->subQuery)
    {
      if (!localCol->hasAggregate() && !localCol->hasWindowFunc())
        gwip->subGroupByCols.push_back(SRCP(localCol->clone()));
    }

    if (sop->op() == OP_EQ)
    {
      if (gwip->subSelectType == CalpontSelectExecutionPlan::IN_SUBS ||
          gwip->subSelectType == CalpontSelectExecutionPlan::EXISTS_SUBS)
        correlatedCol->joinInfo(correlatedCol->joinInfo() | JOIN_SEMI);
      else if (gwip->subSelectType == CalpontSelectExecutionPlan::NOT_IN_SUBS ||
               gwip->subSelectType == CalpontSelectExecutionPlan::NOT_EXISTS_SUBS)
        correlatedCol->joinInfo(correlatedCol->joinInfo() | JOIN_ANTI);
    }
  }

  SimpleFilter* sf = new SimpleFilter();
  sf->timeZone(gwip->timeZone);

  //@bug 2101 for when there are only constants in a delete or update where clause (eg "where 5 < 6").
  // There will be no field column and it will get here only if the comparison is true.
  if (gwip->columnMap.empty() && ((current_thd->lex->sql_command == SQLCOM_UPDATE) ||
                                  (current_thd->lex->sql_command == SQLCOM_UPDATE_MULTI) ||
                                  (current_thd->lex->sql_command == SQLCOM_DELETE) ||
                                  (current_thd->lex->sql_command == SQLCOM_DELETE_MULTI)))
  {
    IDEBUG(cerr << "deleted func with 2 const columns" << endl);
    delete rhs;
    delete lhs;
    return false;
  }

  // handle noop (only for table mode)
  if (rhs->data() == "noop" || lhs->data() == "noop")
  {
    sop.reset(new Operator("noop"));
  }
  else
  {
    for (uint32_t i = 0; i < itemList.size(); i++)
    {
      if (isPredicateFunction(itemList[i], gwip))
      {
        gwip->fatalParseError = true;
        gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_SUB_EXPRESSION);
      }
    }
  }

  sf->op(sop);
  sf->lhs(lhs);
  sf->rhs(rhs);
  sop->setOpType(lhs->resultType(), rhs->resultType());
  sop->resultType(sop->operationType());

  if (sop->op() == OP_EQ)
  {
    auto tan_rhs = rhs->singleTable();
    auto tan_lhs = lhs->singleTable();

    bool outerjoin = (tan_rhs && tan_lhs);

    // @bug 1632. Alias should be taken account to the identity of tables for selfjoin to work
    if (outerjoin && *tan_lhs != *tan_rhs)  // join
    {
      if (!gwip->condPush)  // vtable
      {
        if (!gwip->innerTables.empty())
        {
          checkOuterTableColumn(gwip, *tan_lhs, lhs);
          checkOuterTableColumn(gwip, *tan_rhs, rhs);
        }

        if (funcType == Item_func::EQ_FUNC)
        {
          gwip->equiCondSFList.push_back(sf);
        }

        ParseTree* ptp = new ParseTree(sf);
        gwip->ptWorkStack.push(ptp);
      }
    }
    else
    {
      ParseTree* ptp = new ParseTree(sf);
      gwip->ptWorkStack.push(ptp);
    }
  }
  else
  {
    ParseTree* ptp = new ParseTree(sf);
    gwip->ptWorkStack.push(ptp);
  }

  return true;
}

bool buildPredicateItem(Item_func* ifp, gp_walk_info* gwip)
{
  boost::shared_ptr<Operator> sop(new PredicateOperator(ifp->func_name()));

  if (ifp->functype() == Item_func::LIKE_FUNC)
  {
    // Starting with MariaDB 10.2, LIKE uses a negated flag instead of FUNC_NOT
    // Further processing is done below as before for LIKE
    if (((Item_func_like*)ifp)->get_negated())
    {
      sop->reverseOp();
    }
  }

  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  if (ifp->functype() == Item_func::BETWEEN)
  {
    idbassert(gwip->rcWorkStack.size() >= 3);
    ReturnedColumn* rhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    ReturnedColumn* lhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    ReturnedColumn* filterCol = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();  // pop gwip->scsp;
    Item_func_opt_neg* inp = (Item_func_opt_neg*)ifp;
    SimpleFilter* sfr = 0;
    SimpleFilter* sfl = 0;

    if (inp->negated)
    {
      sop.reset(new PredicateOperator(">"));
      sop->setOpType(filterCol->resultType(), rhs->resultType());
      sfr = new SimpleFilter(sop, filterCol, rhs);
      sfr->timeZone(gwip->timeZone);
      sop.reset(new PredicateOperator("<"));
      sop->setOpType(filterCol->resultType(), lhs->resultType());
      sfl = new SimpleFilter(sop, filterCol->clone(), lhs);
      sfl->timeZone(gwip->timeZone);
      ParseTree* ptp = new ParseTree(new LogicOperator("or"));
      ptp->left(sfr);
      ptp->right(sfl);
      gwip->ptWorkStack.push(ptp);
    }
    else
    {
      sop.reset(new PredicateOperator("<="));
      sop->setOpType(filterCol->resultType(), rhs->resultType());
      sfr = new SimpleFilter(sop, filterCol, rhs);
      sfr->timeZone(gwip->timeZone);
      sop.reset(new PredicateOperator(">="));
      sop->setOpType(filterCol->resultType(), lhs->resultType());
      sfl = new SimpleFilter(sop, filterCol->clone(), lhs);
      sfl->timeZone(gwip->timeZone);
      ParseTree* ptp = new ParseTree(new LogicOperator("and"));
      ptp->left(sfr);
      ptp->right(sfl);
      gwip->ptWorkStack.push(ptp);
    }

    return true;
  }
  else if (ifp->functype() == Item_func::IN_FUNC)
  {
    idbassert(gwip->rcWorkStack.size() >= 2);
    ReturnedColumn* rhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    ReturnedColumn* lhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();

    // @bug3038
    RowColumn* rrhs = dynamic_cast<RowColumn*>(rhs);
    RowColumn* rlhs = dynamic_cast<RowColumn*>(lhs);

    if (rrhs && rlhs)
    {
      return buildRowColumnFilter(gwip, rrhs, rlhs, ifp);
    }

    ConstantColumn* crhs = dynamic_cast<ConstantColumn*>(rhs);
    ConstantColumn* clhs = dynamic_cast<ConstantColumn*>(lhs);

    if (!crhs || !clhs)
    {
      gwip->fatalParseError = true;
      gwip->parseErrorText = "non constant value in IN clause";
      return false;
    }

    string eqop;
    string cmbop;
    Item_func_opt_neg* inp = (Item_func_opt_neg*)ifp;

    if (inp->negated)
    {
      eqop = "<>";
      cmbop = "and";
    }
    else
    {
      eqop = "=";
      cmbop = "or";
    }

    sop.reset(new PredicateOperator(eqop));
    SRCP scsp = gwip->scsp;
    idbassert(scsp.get() != nullptr);
    // sop->setOpType(gwip->scsp->resultType(), rhs->resultType());
    sop->setOpType(scsp->resultType(), rhs->resultType());
    ConstantFilter* cf = 0;

    cf = new ConstantFilter(sop, scsp->clone(), lhs);
    sop.reset(new LogicOperator(cmbop));
    cf->op(sop);
    sop.reset(new PredicateOperator(eqop));
    sop->setOpType(scsp->resultType(), rhs->resultType());
    cf->pushFilter(new SimpleFilter(sop, scsp->clone(), rhs->clone(), gwip->timeZone));

    while (!gwip->rcWorkStack.empty())
    {
      lhs = gwip->rcWorkStack.top();

      if (dynamic_cast<ConstantColumn*>(lhs) == 0)
        break;

      gwip->rcWorkStack.pop();
      sop.reset(new PredicateOperator(eqop));
      sop->setOpType(scsp->resultType(), lhs->resultType());
      cf->pushFilter(new SimpleFilter(sop, scsp->clone(), lhs->clone(), gwip->timeZone));
    }

    if (!gwip->rcWorkStack.empty())
    {
      delete gwip->rcWorkStack.top();
      gwip->rcWorkStack.pop();  // pop gwip->scsp
    }

    if (cf->filterList().size() < inp->argument_count() - 1)
    {
      gwip->fatalParseError = true;
      gwip->parseErrorText = "non constant value in IN clause";
      delete cf;
      return false;
    }

    cf->functionName(gwip->funcName);
    String str;
    // @bug5811. This filter string is for cross engine to use.
    // Use real table name.
    ifp->print(&str, QT_ORDINARY);
    IDEBUG(cerr << str.ptr() << endl);

    if (str.ptr())
      cf->data(str.c_ptr());

    ParseTree* ptp = new ParseTree(cf);
    gwip->ptWorkStack.push(ptp);
  }

  else if (ifp->functype() == Item_func::ISNULL_FUNC || ifp->functype() == Item_func::ISNOTNULL_FUNC)
  {
    ReturnedColumn* rhs = NULL;
    if (!gwip->rcWorkStack.empty() && !gwip->inCaseStmt)
    {
      rhs = gwip->rcWorkStack.top();
      gwip->rcWorkStack.pop();
    }
    else
    {
      rhs = buildReturnedColumn(ifp->arguments()[0], *gwip, gwip->fatalParseError);
    }

    if (rhs && !gwip->fatalParseError)
      buildConstPredicate(ifp, rhs, gwip);
    else if (!rhs)  // @bug 3802
    {
      gwip->fatalParseError = true;
      gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
      return false;
    }
  }

  else if (ifp->functype() == Item_func::GUSERVAR_FUNC)
  {
    Item_func_get_user_var* udf = static_cast<Item_func_get_user_var*>(ifp);
    String buf;

    if (udf->result_type() == INT_RESULT)
    {
      if (udf->unsigned_flag)
      {
        gwip->rcWorkStack.push(new ConstantColumn((uint64_t)udf->val_uint()));
      }
      else
      {
        gwip->rcWorkStack.push(new ConstantColumn((int64_t)udf->val_int()));
      }
      (dynamic_cast<ConstantColumn*>(gwip->rcWorkStack.top()))->timeZone(gwip->timeZone);
    }
    else
    {
      // const String* str = udf->val_str(&buf);
      udf->val_str(&buf);

      if (!buf.ptr())
      {
        ostringstream oss;
        oss << "Unknown user variable: " << udf->name.str;
        gwip->parseErrorText = oss.str();
        gwip->fatalParseError = true;
        return false;
      }

      if (udf->result_type() == STRING_RESULT)
        gwip->rcWorkStack.push(
            new ConstantColumn(buf.ptr()));  // XXX: constantcolumn from string = can it be NULL?
      else
      {
        gwip->rcWorkStack.push(new ConstantColumn(buf.ptr(), ConstantColumn::NUM));
      }
      (dynamic_cast<ConstantColumn*>(gwip->rcWorkStack.top()))->timeZone(gwip->timeZone);

      return false;
    }
  }

#if 0
    else if (ifp->functype() == Item_func::NEG_FUNC)
    {
        //peek at the (hopefully) ConstantColumn on the top of stack, negate it in place
        ConstantColumn* ccp = dynamic_cast<ConstantColumn*>(gwip->rcWorkStack.top());

        if (!ccp)
        {
            ostringstream oss;
            oss << "Attempt to negate a non-constant column";
            gwip->parseErrorText = oss.str();
            gwip->fatalParseError = true;
            return false;
        }

        string cval = ccp->constval();
        string newval;

        if (cval[0] == '-')
            newval.assign(cval, 1, string::npos);
        else
            newval = "-" + cval;

        ccp->constval(newval);
    }

#endif
  else if (ifp->functype() == Item_func::NOT_FUNC)
  {
    if (gwip->condPush && ifp->next->type() == Item::SUBSELECT_ITEM)
      return false;

    if (ifp->next && ifp->next->type() == Item::SUBSELECT_ITEM && gwip->lastSub)
    {
      gwip->lastSub->handleNot();
      return false;
    }

    idbassert(ifp->argument_count() == 1);
    ParseTree* ptp = 0;
    Item_func* argfp = dynamic_cast<Item_func*>(ifp->arguments()[0]);
    if (argfp && argfp->functype() == Item_func::EQUAL_FUNC)
    {
      // negate it in place
      // Note that an EQUAL_FUNC ( a <=> b) was converted to
      // ( a = b OR ( a is null AND b is null) )
      // NOT of the above expression is: ( a != b AND (a is not null OR b is not null )

      if (!gwip->ptWorkStack.empty())
        ptp = gwip->ptWorkStack.top();

      if (ptp)
      {
        ParseTree* or_ptp = ptp;
        ParseTree* and_ptp = or_ptp->right();
        ParseTree* equal_ptp = or_ptp->left();
        ParseTree* nullck_left_ptp = and_ptp->left();
        ParseTree* nullck_right_ptp = and_ptp->right();
        SimpleFilter* sf_left_nullck = dynamic_cast<SimpleFilter*>(nullck_left_ptp->data());
        SimpleFilter* sf_right_nullck = dynamic_cast<SimpleFilter*>(nullck_right_ptp->data());
        SimpleFilter* sf_equal = dynamic_cast<SimpleFilter*>(equal_ptp->data());

        if (sf_left_nullck && sf_right_nullck && sf_equal)
        {
          // Negate the null checks
          sf_left_nullck->op()->reverseOp();
          sf_right_nullck->op()->reverseOp();
          sf_equal->op()->reverseOp();
          // Rehook the nodes
          ptp = and_ptp;
          ptp->left(equal_ptp);
          ptp->right(or_ptp);
          or_ptp->left(nullck_left_ptp);
          or_ptp->right(nullck_right_ptp);
          gwip->ptWorkStack.pop();
          gwip->ptWorkStack.push(ptp);
        }
        else
        {
          gwip->fatalParseError = true;
          gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_ASSERTION_FAILURE);
          return false;
        }
      }
    }
    else if (isPredicateFunction(ifp->arguments()[0], gwip) || ifp->arguments()[0]->type() == Item::COND_ITEM)
    {
      // negate it in place
      if (!gwip->ptWorkStack.empty())
        ptp = gwip->ptWorkStack.top();

      SimpleFilter* sf = 0;

      if (ptp)
      {
        sf = dynamic_cast<SimpleFilter*>(ptp->data());

        if (sf)
          sf->op()->reverseOp();
      }
    }
    else
    {
      // transfrom the not item to item = 0
      ReturnedColumn* rhs = 0;

      if (!gwip->rcWorkStack.empty())
      {
        rhs = gwip->rcWorkStack.top();
        gwip->rcWorkStack.pop();
      }
      else
      {
        rhs = buildReturnedColumn(ifp->arguments()[0], *gwip, gwip->fatalParseError);
      }

      if (rhs && !gwip->fatalParseError)
        return buildConstPredicate(ifp, rhs, gwip);
      else if (!rhs)  // @bug3802
      {
        gwip->fatalParseError = true;
        gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
        return false;
      }
    }
  }
  /*else if (ifp->functype() == Item_func::MULT_EQUAL_FUNC)
  {
      Item_equal *cur_item_eq = (Item_equal*)ifp;
      Item *lhs_item, *rhs_item;
      // There must be at least two items
      Item_equal_fields_iterator lhs_it(*cur_item_eq);
      Item_equal_fields_iterator rhs_it(*cur_item_eq); rhs_it++;
      while ((lhs_item = lhs_it++) && (rhs_item = rhs_it++))
      {
          ReturnedColumn* lhs = buildReturnedColumn(lhs_item, *gwip, gwip->fatalParseError);
          ReturnedColumn* rhs = buildReturnedColumn(rhs_item, *gwip, gwip->fatalParseError);
          if (!rhs || !lhs)
          {
              gwip->fatalParseError = true;
              gwip->parseErrorText = "Unsupport elements in MULT_EQUAL item";
              delete rhs;
              delete lhs;
              return false;
          }
          PredicateOperator* po = new PredicateOperator("=");
          boost::shared_ptr<Operator> sop(po);
          sop->setOpType(lhs->resultType(), rhs->resultType());
          SimpleFilter* sf = new SimpleFilter(sop, lhs, rhs);
          // Search in ptWorkStack for duplicates of the SF
          // before we push the SF
          if (!isDuplicateSF(gwip, sf))
          {
              ParseTree* pt = new ParseTree(sf);
              gwip->ptWorkStack.push(pt);
          }
      }
  }*/
  else if (ifp->functype() == Item_func::EQUAL_FUNC)
  {
    // Convert "a <=> b" to (a = b OR (a IS NULL AND b IS NULL))"
    idbassert(gwip->rcWorkStack.size() >= 2);
    ReturnedColumn* rhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    ReturnedColumn* lhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    SimpleFilter* sfn1 = 0;
    SimpleFilter* sfn2 = 0;
    SimpleFilter* sfo = 0;
    // b IS NULL
    ConstantColumn* nlhs1 = new ConstantColumn("", ConstantColumn::NULLDATA);
    nlhs1->timeZone(gwip->timeZone);
    sop.reset(new PredicateOperator("isnull"));
    sop->setOpType(lhs->resultType(), rhs->resultType());
    sfn1 = new SimpleFilter(sop, rhs, nlhs1);
    sfn1->timeZone(gwip->timeZone);
    ParseTree* ptpl = new ParseTree(sfn1);
    // a IS NULL
    ConstantColumn* nlhs2 = new ConstantColumn("", ConstantColumn::NULLDATA);
    nlhs2->timeZone(gwip->timeZone);
    sop.reset(new PredicateOperator("isnull"));
    sop->setOpType(lhs->resultType(), rhs->resultType());
    sfn2 = new SimpleFilter(sop, lhs, nlhs2);
    sfn2->timeZone(gwip->timeZone);
    ParseTree* ptpr = new ParseTree(sfn2);
    // AND them both
    ParseTree* ptpn = new ParseTree(new LogicOperator("and"));
    ptpn->left(ptpl);
    ptpn->right(ptpr);
    // a = b
    sop.reset(new PredicateOperator("="));
    sop->setOpType(lhs->resultType(), rhs->resultType());
    sfo = new SimpleFilter(sop, lhs->clone(), rhs->clone());
    sfo->timeZone(gwip->timeZone);
    // OR with the NULL comparison tree
    ParseTree* ptp = new ParseTree(new LogicOperator("or"));
    ptp->left(sfo);
    ptp->right(ptpn);
    gwip->ptWorkStack.push(ptp);
    return true;
  }
  else  // std rel ops (incl "like")
  {
    if (gwip->rcWorkStack.size() < 2)
    {
      idbassert(ifp->argument_count() == 2);

      if (isPredicateFunction(ifp->arguments()[0], gwip) || ifp->arguments()[0]->type() == Item::COND_ITEM ||
          isPredicateFunction(ifp->arguments()[1], gwip) || ifp->arguments()[1]->type() == Item::COND_ITEM)
      {
        gwip->fatalParseError = true;
        gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
      }

      return false;
    }

    ReturnedColumn* rhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();
    ReturnedColumn* lhs = gwip->rcWorkStack.top();
    gwip->rcWorkStack.pop();

    // @bug3038. rowcolumn filter
    RowColumn* rrhs = dynamic_cast<RowColumn*>(rhs);
    RowColumn* rlhs = dynamic_cast<RowColumn*>(lhs);

    if (rrhs && rlhs)
    {
      return buildRowColumnFilter(gwip, rrhs, rlhs, ifp);
    }

    vector<Item*> itemList;

    for (uint32_t i = 0; i < ifp->argument_count(); i++)
      itemList.push_back(ifp->arguments()[i]);

    return buildEqualityPredicate(lhs, rhs, gwip, sop, ifp->functype(), itemList);
  }

  return true;
}

bool buildConstPredicate(Item_func* ifp, ReturnedColumn* rhs, gp_walk_info* gwip)
{
  SimpleFilter* sf = new SimpleFilter();
  sf->timeZone(gwip->timeZone);
  boost::shared_ptr<Operator> sop(new PredicateOperator(ifp->func_name()));
  ConstantColumn* lhs = 0;

  if (ifp->functype() == Item_func::ISNULL_FUNC)
  {
    lhs = new ConstantColumn("", ConstantColumn::NULLDATA);
    sop.reset(new PredicateOperator("isnull"));
  }
  else if (ifp->functype() == Item_func::ISNOTNULL_FUNC)
  {
    lhs = new ConstantColumn("", ConstantColumn::NULLDATA);
    sop.reset(new PredicateOperator("isnotnull"));
  }
  else  // if (ifp->functype() == Item_func::NOT_FUNC)
  {
    lhs = new ConstantColumn((int64_t)0, ConstantColumn::NUM);
    sop.reset(new PredicateOperator("="));
  }
  lhs->timeZone(gwip->timeZone);

  CalpontSystemCatalog::ColType opType = rhs->resultType();

  if ((opType.colDataType == CalpontSystemCatalog::CHAR && opType.colWidth <= 8) ||
      (opType.colDataType == CalpontSystemCatalog::VARCHAR && opType.colWidth < 8) ||
      (opType.colDataType == CalpontSystemCatalog::VARBINARY && opType.colWidth < 8))
  {
    opType.colDataType = execplan::CalpontSystemCatalog::BIGINT;
    opType.colWidth = 8;
  }

  sop->operationType(opType);
  sf->op(sop);

  // yes, these are backwards
  assert(lhs);
  sf->lhs(rhs);
  sf->rhs(lhs);
  ParseTree* ptp = new ParseTree(sf);
  gwip->ptWorkStack.push(ptp);
  return true;
}

SimpleColumn* buildSimpleColFromDerivedTable(gp_walk_info& gwi, Item_field* ifp)
{
  SimpleColumn* sc = NULL;

  // view name
  string viewName = getViewName(ifp->cached_table);

  // Check from the end because local scope resolve is preferred
  for (int32_t i = gwi.tbList.size() - 1; i >= 0; i--)
  {
    if (sc)
      break;

    if (!gwi.tbList[i].schema.empty() && !gwi.tbList[i].table.empty() &&
        (!ifp->table_name.str || strcasecmp(ifp->table_name.str, gwi.tbList[i].alias.c_str()) == 0))
    {
      CalpontSystemCatalog::TableName tn(gwi.tbList[i].schema, gwi.tbList[i].table);
      CalpontSystemCatalog::RIDList oidlist = gwi.csc->columnRIDs(tn, true);

      for (unsigned int j = 0; j < oidlist.size(); j++)
      {
        CalpontSystemCatalog::TableColName tcn = gwi.csc->colName(oidlist[j].objnum);
        CalpontSystemCatalog::ColType ct = gwi.csc->colType(oidlist[j].objnum);

        if (strcasecmp(ifp->field_name.str, tcn.column.c_str()) == 0)
        {
          // @bug4827. Remove the checking because outside tables could be the same
          // name as inner tables. This function is to identify column from a table,
          // as long as it matches the next step in predicate parsing will tell the scope
          // of the column.
          /*if (sc)
          {
              gwi.fatalParseError = true;
              Message::Args args;
              args.add(ifp->name);
              gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_AMBIGUOUS_COL, args);
              return NULL;
          }*/

          sc = new SimpleColumn();
          sc->columnName(tcn.column);
          sc->tableName(tcn.table);
          sc->schemaName(tcn.schema);
          sc->oid(oidlist[j].objnum);

          // @bug 3003. Keep column alias if it has.
          sc->alias(!ifp->is_explicit_name() ? tcn.column : ifp->name.str);

          sc->tableAlias(gwi.tbList[i].alias);
          sc->viewName(viewName, lower_case_table_names);
          sc->partitions(gwi.tbList[i].partitions);
          sc->resultType(ct);
          sc->timeZone(gwi.timeZone);
          break;
        }
      }
    }
  }

  if (sc)
    return sc;

  // Check from the end because local scope resolve is preferred
  for (int32_t i = gwi.derivedTbList.size() - 1; i >= 0; i--)
  {
    if (sc)
      break;

    CalpontSelectExecutionPlan* csep = dynamic_cast<CalpontSelectExecutionPlan*>(gwi.derivedTbList[i].get());
    string derivedName = csep->derivedTbAlias();

    if (!ifp->table_name.str || strcasecmp(ifp->table_name.str, derivedName.c_str()) == 0)
    {
      CalpontSelectExecutionPlan::ReturnedColumnList cols = csep->returnedCols();

      for (uint32_t j = 0; j < cols.size(); j++)
      {
        // @bug 3167 duplicate column alias is full name
        SimpleColumn* col = dynamic_cast<SimpleColumn*>(cols[j].get());
        string alias = cols[j]->alias();

        // MCOL-5357 For the following query:

        // select item from (
        // select item from (select a as item from t1) tt
        // union all
        // select item from (select a as item from t1) tt
        // ) ttt;

        // When the execution reaches the outermost item (ttt.item),
        // alias = "`tt`.`item`" and ifp->field_name.str = "item".
        // To make the execution enter the if block below, we strip off
        // the backticks from alias.
        boost::erase_all(alias, "`");

        if (strcasecmp(ifp->field_name.str, alias.c_str()) == 0 ||
            (col && alias.find(".") != string::npos &&
             (strcasecmp(ifp->field_name.str, col->columnName().c_str()) == 0 ||
              strcasecmp(ifp->field_name.str, (alias.substr(alias.find_last_of(".") + 1)).c_str()) ==
                  0)))  //@bug6066
        {
          // @bug4827. Remove the checking because outside tables could be the same
          // name as inner tables. This function is to identify column from a table,
          // as long as it matches the next step in predicate parsing will tell the scope
          // of the column.
          /*if (sc)
          {
              gwi.fatalParseError = true;
              Message::Args args;
              args.add(ifp->name);
              gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_AMBIGUOUS_COL, args);
              return NULL;
          }*/
          sc = new SimpleColumn();

          if (!col)
            sc->columnName(cols[j]->alias());
          else
            sc->columnName(col->columnName());

          // @bug 3003. Keep column alias if it has.
          sc->alias(!ifp->is_explicit_name() ? cols[j]->alias() : ifp->name.str);
          sc->tableName(csep->derivedTbAlias());
          sc->colPosition(j);
          sc->tableAlias(csep->derivedTbAlias());
          sc->timeZone(gwi.timeZone);
          if (!viewName.empty())
          {
            sc->viewName(viewName, lower_case_table_names);
          }
          else
          {
            sc->viewName(csep->derivedTbView());
          }
          sc->resultType(cols[j]->resultType());
          sc->hasAggregate(cols[j]->hasAggregate());
          // XXX partitions???

          if (col)
            sc->isColumnStore(col->isColumnStore());

          // @bug5634, @bug5635. mark used derived col on derived table.
          // outer join inner table filter can not be moved in
          // MariaDB 10.1: cached_table is never available for derived tables.
          // Find the uncached object in table_list
          TABLE_LIST* tblList = ifp->context ? ifp->context->table_list : NULL;

          while (tblList)
          {
            if (strcasecmp(tblList->alias.str, ifp->table_name.str) == 0)
            {
              if (!tblList->outer_join)
              {
                sc->derivedTable(derivedName);
                sc->derivedRefCol(cols[j].get());
              }

              break;
            }

            tblList = tblList->next_local;
          }

          cols[j]->incRefCount();
          break;
        }
      }
    }
  }

  if (!sc)
  {
    gwi.fatalParseError = true;
    Message::Args args;
    string name;

    if (ifp->db_name.str)
      name += string(ifp->db_name.str) + ".";

    if (ifp->table_name.str)
      name += string(ifp->table_name.str) + ".";

    if (ifp->name.length)
      name += ifp->name.str;
    args.add(name);
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_UNKNOWN_COL, args);
  }

  if (ifp->depended_from)
  {
    sc->joinInfo(sc->joinInfo() | JOIN_CORRELATED);

    if (gwi.subQuery)
      gwi.subQuery->correlated(true);

    // for error out non-support select filter case (comparison outside semi join tables)
    gwi.correlatedTbNameVec.push_back(make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias()));

    // imply semi for scalar for now.
    if (gwi.subSelectType == CalpontSelectExecutionPlan::SINGLEROW_SUBS)
      sc->joinInfo(sc->joinInfo() | JOIN_SCALAR | JOIN_SEMI);

    if (gwi.subSelectType == CalpontSelectExecutionPlan::SELECT_SUBS)
      sc->joinInfo(sc->joinInfo() | JOIN_SCALAR | JOIN_OUTER_SELECT);
  }

  return sc;
}

// for FROM clause subquery. get all the columns from real tables and derived tables.
void collectAllCols(gp_walk_info& gwi, Item_field* ifp)
{
  // view name
  string viewName = getViewName(ifp->cached_table);

  if (gwi.derivedTbList.empty())
  {
    gwi.fatalParseError = true;
    Message::Args args;
    args.add("*");
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_UNKNOWN_COL, args);
  }

  string tableName = (ifp->table_name.str ? string(ifp->table_name.str) : "");
  CalpontSelectExecutionPlan::SelectList::const_iterator it = gwi.derivedTbList.begin();

  for (uint32_t i = 0; i < gwi.tbList.size(); i++)
  {
    SRCP srcp;

    // derived table
    if (gwi.tbList[i].schema.empty())
    {
      CalpontSelectExecutionPlan* csep = dynamic_cast<CalpontSelectExecutionPlan*>((*it).get());
      ++it;

      if (!tableName.empty() && strcasecmp(tableName.c_str(), csep->derivedTbAlias().c_str()) != 0)
        continue;

      CalpontSelectExecutionPlan::ReturnedColumnList cols = csep->returnedCols();

      for (uint32_t j = 0; j < cols.size(); j++)
      {
        SimpleColumn* sc = new SimpleColumn();
        sc->columnName(cols[j]->alias());
        sc->colPosition(j);
        sc->tableAlias(csep->derivedTbAlias());
        sc->viewName(gwi.tbList[i].view);
        sc->partitions(gwi.tbList[i].partitions);
        sc->resultType(cols[j]->resultType());
        sc->timeZone(gwi.timeZone);

        // @bug5634 derived table optimization
        cols[j]->incRefCount();
        sc->derivedTable(sc->tableAlias());
        sc->derivedRefCol(cols[j].get());
        srcp.reset(sc);
        gwi.returnedCols.push_back(srcp);
        gwi.columnMap.insert(CalpontSelectExecutionPlan::ColumnMap::value_type(sc->columnName(), srcp));
      }
    }
    // real tables
    else
    {
      CalpontSystemCatalog::TableName tn(gwi.tbList[i].schema, gwi.tbList[i].table);

      if (lower_case_table_names)
      {
        if (!tableName.empty() && (strcasecmp(tableName.c_str(), tn.table.c_str()) != 0 &&
                                   strcasecmp(tableName.c_str(), gwi.tbList[i].alias.c_str()) != 0))
          continue;
      }
      else
      {
        if (!tableName.empty() && (strcmp(tableName.c_str(), tn.table.c_str()) != 0 &&
                                   strcmp(tableName.c_str(), gwi.tbList[i].alias.c_str()) != 0))
          continue;
      }
      if (lower_case_table_names)
      {
        boost::algorithm::to_lower(tn.schema);
        boost::algorithm::to_lower(tn.table);
      }
      CalpontSystemCatalog::RIDList oidlist = gwi.csc->columnRIDs(tn, true);

      for (unsigned int j = 0; j < oidlist.size(); j++)
      {
        SimpleColumn* sc = new SimpleColumn();
        CalpontSystemCatalog::TableColName tcn = gwi.csc->colName(oidlist[j].objnum);
        CalpontSystemCatalog::ColType ct = gwi.csc->colType(oidlist[j].objnum);
        sc->columnName(tcn.column);
        sc->tableName(tcn.table, lower_case_table_names);
        sc->schemaName(tcn.schema, lower_case_table_names);
        sc->oid(oidlist[j].objnum);
        sc->alias(tcn.column);
        sc->resultType(ct);
        sc->tableAlias(gwi.tbList[i].alias, lower_case_table_names);
        sc->partitions(gwi.tbList[i].partitions);
        sc->viewName(viewName, lower_case_table_names);
        sc->timeZone(gwi.timeZone);
        srcp.reset(sc);
        gwi.returnedCols.push_back(srcp);
        gwi.columnMap.insert(CalpontSelectExecutionPlan::ColumnMap::value_type(sc->columnName(), srcp));
      }
    }
  }
}

void buildSubselectFunc(Item_func* ifp, gp_walk_info* gwip)
{
  // @bug 3035
  if (!isPredicateFunction(ifp, gwip))
  {
    gwip->fatalParseError = true;
    gwip->parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_FUNC_SUB);
    return;
  }

  WhereSubQuery* subquery = NULL;

  for (uint32_t i = 0; i < ifp->argument_count(); i++)
  {
#if MYSQL_VERSION_ID >= 50172

    // changes comply with mysql 5.1.72
    if (ifp->arguments()[i]->type() == Item::FUNC_ITEM &&
        string(((Item_func*)ifp->arguments()[i])->func_name()) == "<in_optimizer>")
    {
      if (ifp->functype() == Item_func::NOT_FUNC)
      {
        if (gwip->lastSub)
          gwip->lastSub->handleNot();
      }
    }

#endif

    if (ifp->arguments()[i]->type() == Item::SUBSELECT_ITEM)
    {
      Item_subselect* sub = (Item_subselect*)ifp->arguments()[i];

      switch (sub->substype())
      {
        case Item_subselect::SINGLEROW_SUBS: subquery = new ScalarSub(*gwip, ifp); break;

        case Item_subselect::IN_SUBS: subquery = new InSub(*gwip, ifp); break;

        case Item_subselect::EXISTS_SUBS:

          // exists sub has been handled earlier. here is for not function
          if (ifp->functype() == Item_func::NOT_FUNC)
          {
            if (gwip->lastSub)
              gwip->lastSub->handleNot();
          }

          break;

        default:
          Message::Args args;
          gwip->fatalParseError = true;
          gwip->parseErrorText = "non supported subquery";
          return;
      }
    }
  }

  if (subquery)
  {
    gwip->hasSubSelect = true;
    SubQuery* orig = gwip->subQuery;
    gwip->subQuery = subquery;
    // no need to check NULL for now. error will be handled in gp_walk
    gwip->ptWorkStack.push(subquery->transform());
    // recover original sub. Save current sub for Not handling.
    gwip->lastSub = subquery;
    gwip->subQuery = orig;
  }

  return;
}

bool isPredicateFunction(Item* item, gp_walk_info* gwip)
{
  if (item->type() == Item::COND_ITEM)
    return true;

  if (item->type() != Item::FUNC_ITEM)
    return false;

  Item_func* ifp = (Item_func*)item;
  return ( ifp->functype() == Item_func::EQ_FUNC ||
             ifp->functype() == Item_func::NE_FUNC ||
             ifp->functype() == Item_func::LT_FUNC ||
             ifp->functype() == Item_func::LE_FUNC ||
             ifp->functype() == Item_func::GE_FUNC ||
             ifp->functype() == Item_func::GT_FUNC ||
             ifp->functype() == Item_func::LIKE_FUNC ||
             ifp->functype() == Item_func::BETWEEN ||
             ifp->functype() == Item_func::IN_FUNC ||
             (ifp->functype() == Item_func::ISNULL_FUNC &&
              (gwip->clauseType == WHERE || gwip->clauseType == HAVING)) ||
             (ifp->functype() == Item_func::ISNOTNULL_FUNC &&
              (gwip->clauseType == WHERE || gwip->clauseType == HAVING)) ||
             ifp->functype() == Item_func::NOT_FUNC ||
             ifp->functype() == Item_func::ISNOTNULLTEST_FUNC ||
             ifp->functype() == Item_func::TRIG_COND_FUNC ||
             string(ifp->func_name()) == "<in_optimizer>"/* ||
		string(ifp->func_name()) == "xor"*/);
}

void setError(THD* thd, uint32_t errcode, string errmsg)
{
  thd->get_stmt_da()->set_overwrite_status(true);

  if (errmsg.empty())
    errmsg = "Unknown error";

  if (errcode < ER_ERROR_FIRST || errcode > ER_ERROR_LAST)
  {
    errcode = ER_UNKNOWN_ERROR;
  }

  thd->raise_error_printf(errcode, errmsg.c_str());

  // reset expressionID
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());
  ci->expressionId = 0;
}

void setError(THD* thd, uint32_t errcode, string errmsg, gp_walk_info& /*gwi*/)
{
  setError(thd, errcode, errmsg);
}

int setErrorAndReturn(gp_walk_info& gwi)
{
  // if this is dervied table process phase, mysql may have not developed the plan
  // completely. Do not error and eventually mysql will call JOIN::exec() again.
  // related to bug 2922. Need to find a way to skip calling rnd_init for derived table
  // processing.
  if (gwi.thd->derived_tables_processing)
  {
    gwi.cs_vtable_is_update_with_derive = true;
    return -1;
  }

  setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
  return ER_INTERNAL_ERROR;
}

const string bestTableName(const Item_field* ifp)
{
  idbassert(ifp);

  if (!ifp->table_name.str)
    return "";

  if (!ifp->field)
    return ifp->table_name.str;

  string table_name(ifp->table_name.str);
  string field_table_table_name;

  if (ifp->cached_table)
    field_table_table_name = ifp->cached_table->table_name.str;
  else if (ifp->field->table && ifp->field->table->s && ifp->field->table->s->table_name.str)
    field_table_table_name = ifp->field->table->s->table_name.str;

  string tn;

  if (!field_table_table_name.empty())
    tn = field_table_table_name;
  else
    tn = table_name;

  return tn;
}

uint32_t setAggOp(AggregateColumn* ac, Item_sum* isp)
{
  Item_sum::Sumfunctype agg_type = isp->sum_func();
  uint32_t rc = 0;

  switch (agg_type)
  {
    case Item_sum::COUNT_FUNC: ac->aggOp(AggregateColumn::COUNT); return rc;

    case Item_sum::SUM_FUNC: ac->aggOp(AggregateColumn::SUM); return rc;

    case Item_sum::AVG_FUNC: ac->aggOp(AggregateColumn::AVG); return rc;

    case Item_sum::MIN_FUNC: ac->aggOp(AggregateColumn::MIN); return rc;

    case Item_sum::MAX_FUNC: ac->aggOp(AggregateColumn::MAX); return rc;

    case Item_sum::COUNT_DISTINCT_FUNC:
      ac->aggOp(AggregateColumn::DISTINCT_COUNT);
      ac->distinct(true);
      return rc;

    case Item_sum::SUM_DISTINCT_FUNC:
      ac->aggOp(AggregateColumn::DISTINCT_SUM);
      ac->distinct(true);
      return rc;

    case Item_sum::AVG_DISTINCT_FUNC:
      ac->aggOp(AggregateColumn::DISTINCT_AVG);
      ac->distinct(true);
      return rc;

    case Item_sum::STD_FUNC:
    {
      Item_sum_variance* var = (Item_sum_variance*)isp;

      if (var->sample)
        ac->aggOp(AggregateColumn::STDDEV_SAMP);
      else
        ac->aggOp(AggregateColumn::STDDEV_POP);

      return rc;
    }

    case Item_sum::VARIANCE_FUNC:
    {
      Item_sum_variance* var = (Item_sum_variance*)isp;

      if (var->sample)
        ac->aggOp(AggregateColumn::VAR_SAMP);
      else
        ac->aggOp(AggregateColumn::VAR_POP);

      return rc;
    }

    case Item_sum::GROUP_CONCAT_FUNC:
    {
      Item_func_group_concat* gc = (Item_func_group_concat*)isp;
      ac->aggOp(AggregateColumn::GROUP_CONCAT);
      ac->distinct(gc->get_distinct());
      return rc;
    }

    case Item_sum::JSON_ARRAYAGG_FUNC:
    {
      Item_func_json_arrayagg* gc = (Item_func_json_arrayagg*)isp;
      ac->aggOp(AggregateColumn::JSON_ARRAYAGG);
      ac->distinct(gc->get_distinct());
      return rc;
    }

    case Item_sum::SUM_BIT_FUNC:
    {
      string funcName = isp->func_name();
      if (funcName.compare("bit_and(") == 0)
        ac->aggOp(AggregateColumn::BIT_AND);
      else if (funcName.compare("bit_or(") == 0)
        ac->aggOp(AggregateColumn::BIT_OR);
      else if (funcName.compare("bit_xor(") == 0)
        ac->aggOp(AggregateColumn::BIT_XOR);
      else
        return ER_CHECK_NOT_IMPLEMENTED;

      return rc;
    }

    case Item_sum::UDF_SUM_FUNC: ac->aggOp(AggregateColumn::UDAF); return rc;

    default: return ER_CHECK_NOT_IMPLEMENTED;
  }
}

/* get the smallest column of a table. Used for filling columnmap */
SimpleColumn* getSmallestColumn(boost::shared_ptr<CalpontSystemCatalog> csc,
                                CalpontSystemCatalog::TableName& tn,
                                CalpontSystemCatalog::TableAliasName& tan, TABLE* table, gp_walk_info& gwi)
{
  if (lower_case_table_names)
  {
    boost::algorithm::to_lower(tn.schema);
    boost::algorithm::to_lower(tn.table);
    boost::algorithm::to_lower(tan.schema);
    boost::algorithm::to_lower(tan.table);
    boost::algorithm::to_lower(tan.alias);
    boost::algorithm::to_lower(tan.view);
  }
  // derived table
  if (tan.schema.empty())
  {
    for (uint32_t i = 0; i < gwi.derivedTbList.size(); i++)
    {
      CalpontSelectExecutionPlan* csep =
          dynamic_cast<CalpontSelectExecutionPlan*>(gwi.derivedTbList[i].get());

      if (tan.alias == csep->derivedTbAlias())
      {
        const CalpontSelectExecutionPlan::ReturnedColumnList& cols = csep->returnedCols();

        CalpontSelectExecutionPlan::ReturnedColumnList::const_iterator iter;

        ReturnedColumn* rc = nullptr;

        for (iter = cols.begin(); iter != cols.end(); iter++)
        {
          if ((*iter)->refCount() != 0)
          {
            rc = dynamic_cast<ReturnedColumn*>(iter->get());
            break;
          }
        }

        if (iter == cols.end())
        {
          assert(!cols.empty());

          // We take cols[0] here due to the optimization happening in
          // derivedTableOptimization. All cols with refCount 0 from
          // the end of the cols list are optimized out, until the
          // first column with non-zero refCount is encountered. So
          // here, if instead of cols[0], we take cols[1] (based on
          // some logic) and increment it's refCount, then cols[0] is
          // not optimized out in derivedTableOptimization and is
          // added as a ConstantColumn to the derived table's returned
          // column list. This later causes an ineffective row group
          // with row of the form (1, cols[1]_value1) to be created in ExeMgr.
          rc = dynamic_cast<ReturnedColumn*>(cols[0].get());

          // @bug5634 derived table optimization.
          rc->incRefCount();
        }

        SimpleColumn* sc = new SimpleColumn();
        sc->columnName(rc->alias());
        sc->sequence(0);
        sc->tableAlias(tan.alias);
        sc->partitions(tan.partitions);
        sc->timeZone(gwi.timeZone);
        sc->derivedTable(csep->derivedTbAlias());
        sc->derivedRefCol(rc);
        return sc;
      }
    }

    throw runtime_error("getSmallestColumn: Internal error.");
  }

  // check engine type
  if (!tan.fisColumnStore)
  {
    // get the first column to project. @todo optimization to get the smallest one for foreign engine.
    Field* field = *(table->field);
    SimpleColumn* sc = new SimpleColumn(table->s->db.str, table->s->table_name.str, field->field_name.str,
                                        tan.fisColumnStore, gwi.sessionid, lower_case_table_names);
    sc->tableAlias(table->alias.ptr(), lower_case_table_names);
    sc->partitions(tan.partitions);
    sc->isColumnStore(false);
    sc->timeZone(gwi.timeZone);
    sc->resultType(fieldType_MysqlToIDB(field));
    sc->oid(field->field_index + 1);
    return sc;
  }

  CalpontSystemCatalog::RIDList oidlist = csc->columnRIDs(tn, true);
  idbassert(oidlist.size() == table->s->fields);
  CalpontSystemCatalog::TableColName tcn;
  int minColWidth = -1;
  int minWidthColOffset = 0;

  for (unsigned int j = 0; j < oidlist.size(); j++)
  {
    CalpontSystemCatalog::ColType ct = csc->colType(oidlist[j].objnum);

    if (ct.colDataType == CalpontSystemCatalog::VARBINARY)
      continue;

    if (minColWidth == -1 || ct.colWidth < minColWidth)
    {
      minColWidth = ct.colWidth;
      minWidthColOffset = j;
    }
  }

  tcn = csc->colName(oidlist[minWidthColOffset].objnum);
  SimpleColumn* sc = new SimpleColumn(tcn.schema, tcn.table, tcn.column, csc->sessionID());
  sc->tableAlias(tan.alias);
  sc->viewName(tan.view);
  sc->partitions(tan.partitions);
  sc->timeZone(gwi.timeZone);
  sc->resultType(csc->colType(oidlist[minWidthColOffset].objnum));
  sc->charsetNumber(table->field[minWidthColOffset]->charset()->number);
  return sc;
}

CalpontSystemCatalog::ColType fieldType_MysqlToIDB(const Field* field)
{
  CalpontSystemCatalog::ColType ct;
  ct.precision = 4;

  switch (field->result_type())
  {
    case INT_RESULT:
      ct.colDataType = CalpontSystemCatalog::BIGINT;
      ct.colWidth = 8;
      break;

    case STRING_RESULT:
      ct.colDataType = CalpontSystemCatalog::VARCHAR;
      ct.colWidth = field->field_length;
      break;

    case DECIMAL_RESULT:
    {
      const Field_new_decimal* idp = dynamic_cast<const Field_new_decimal*>(field);

      idbassert(idp);

      ct.colDataType = CalpontSystemCatalog::DECIMAL;
      ct.colWidth = 8;
      ct.scale = idp->dec;

      if (ct.scale == 0)
        ct.precision = idp->field_length - 1;
      else
        ct.precision = idp->field_length - idp->dec;

      break;
    }

    case REAL_RESULT:
      ct.colDataType = CalpontSystemCatalog::DOUBLE;
      ct.colWidth = 8;
      break;

    default:
      IDEBUG(cerr << "fieldType_MysqlToIDB:: Unknown result type of MySQL " << field->result_type() << endl);
      break;
  }

  return ct;
}

CalpontSystemCatalog::ColType colType_MysqlToIDB(const Item* item)
{
  CalpontSystemCatalog::ColType ct;
  ct.precision = 4;

  switch (item->result_type())
  {
    case INT_RESULT:
      if (item->unsigned_flag)
      {
        ct.colDataType = CalpontSystemCatalog::UBIGINT;
      }
      else
      {
        ct.colDataType = CalpontSystemCatalog::BIGINT;
      }

      ct.colWidth = 8;
      break;

    case STRING_RESULT:
      ct.colDataType = CalpontSystemCatalog::VARCHAR;

      // MCOL-4758 the longest TEXT we deal with is 16777215 so
      // limit to that.
      if (item->max_length < 16777215)
        ct.colWidth = item->max_length;
      else
        ct.colWidth = 16777215;

      // force token
      if (item->type() == Item::FUNC_ITEM)
      {
        if (ct.colWidth < 20)
          ct.colWidth = 20;  // for infinidb date length
      }

      // @bug5083. MySQL gives string type for date/datetime column.
      // need to adjust here.
      if (item->type() == Item::FIELD_ITEM)
      {
        if (item->field_type() == MYSQL_TYPE_DATE)
        {
          ct.colDataType = CalpontSystemCatalog::DATE;
          ct.colWidth = 4;
        }
        else if (item->field_type() == MYSQL_TYPE_DATETIME || item->field_type() == MYSQL_TYPE_DATETIME2)
        {
          ct.colDataType = CalpontSystemCatalog::DATETIME;
          ct.colWidth = 8;
        }
        else if (item->field_type() == MYSQL_TYPE_TIMESTAMP || item->field_type() == MYSQL_TYPE_TIMESTAMP2)
        {
          ct.colDataType = CalpontSystemCatalog::TIMESTAMP;
          ct.colWidth = 8;
        }
        else if (item->field_type() == MYSQL_TYPE_TIME)
        {
          ct.colDataType = CalpontSystemCatalog::TIME;
          ct.colWidth = 8;
        }

        if (item->field_type() == MYSQL_TYPE_BLOB)
        {
          // We default to BLOB, but then try to correct type,
          // because many textual types in server have type_handler_blob
          // (and variants) as their type.
          ct.colDataType = CalpontSystemCatalog::BLOB;
          const Item_result_field* irf = dynamic_cast<const Item_result_field*>(item);
          if (irf && irf->result_field && !irf->result_field->binary())
          {
            ct.colDataType = CalpontSystemCatalog::TEXT;
          }
        }
      }

      break;

    /* FIXME:
                    case xxxBINARY_RESULT:
                            ct.colDataType = CalpontSystemCatalog::VARBINARY;
                            ct.colWidth = item->max_length;
                            // force token
                            if (item->type() == Item::FUNC_ITEM)
                            {
                                    if (ct.colWidth < 20)
                                            ct.colWidth = 20; // for infinidb date length
                            }
                            break;
    */
    case DECIMAL_RESULT:
    {
      // decimal result do not shows us Item is Item_decimal
      ct.colDataType = CalpontSystemCatalog::DECIMAL;

      unsigned int precision = item->decimal_precision();
      unsigned int scale = item->decimal_scale();

      ct.setDecimalScalePrecision(precision, scale);

      break;
    }

    case REAL_RESULT:
      ct.colDataType = CalpontSystemCatalog::DOUBLE;
      ct.colWidth = 8;
      break;

    default:
      IDEBUG(cerr << "colType_MysqlToIDB:: Unknown result type of MySQL " << item->result_type() << endl);
      break;
  }
  ct.charsetNumber = item->collation.collation->number;
  return ct;
}

bool itemDisablesWrapping(Item* item, gp_walk_info& gwi)
{
  if (gwi.select_lex == nullptr)
  {
    return true;
  }
  ORDER* groupcol = static_cast<ORDER*>(gwi.select_lex->group_list.first);

  while (groupcol)
  {
    Item* gci = *groupcol->item;
    while (gci->type() == Item::REF_ITEM)
    {
      if (item->eq(gci, false))
      {
        return true;
      }
      Item_ref* ref = (Item_ref*)gci;
      gci = *(ref->ref);
    }
    if (item->eq(gci, false))
    {
      return true;
    }
    groupcol = groupcol->next;
  }
  return false;
}
ReturnedColumn* wrapIntoAggregate(ReturnedColumn* rc, gp_walk_info& gwi, Item* baseItem)
{
  if (!gwi.implicitExplicitGroupBy || gwi.disableWrapping || !gwi.select_lex)
  {
    return rc;
  }

  if (dynamic_cast<AggregateColumn*>(rc) != nullptr || dynamic_cast<ConstantColumn*>(rc) != nullptr)
  {
    return rc;
  }

  if (itemDisablesWrapping(baseItem, gwi))
  {
    return rc;
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  AggregateColumn* ac = new AggregateColumn(gwi.sessionid);
  ac->timeZone(gwi.timeZone);
  ac->alias(rc->alias());
  ac->aggOp(AggregateColumn::SELECT_SOME);
  ac->asc(rc->asc());
  ac->charsetNumber(rc->charsetNumber());
  ac->orderPos(rc->orderPos());
  uint32_t i;
  for (i = 0; i < gwi.processed.size() && !gwi.processed[i].first->eq(baseItem, false); i++)
  {
  }
  if (i < gwi.processed.size())
  {
    ac->expressionId(gwi.processed[i].second);
  }
  else
  {
    ac->expressionId(ci->expressionId++);
  }

  ac->aggParms().push_back(SRCP(rc));
  ac->resultType(rc->resultType());
  return ac;
}

ReturnedColumn* buildReturnedColumnNull(gp_walk_info& gwi)
{
  if (gwi.condPush)
    return new SimpleColumn("noop");
  ConstantColumn* rc = new ConstantColumnNull();
  if (rc)
    rc->timeZone(gwi.timeZone);
  return rc;
}

ReturnedColumn* buildReturnedColumnBody(Item* item, gp_walk_info& gwi, bool& nonSupport, bool /*isRefItem*/)
{
  ReturnedColumn* rc = NULL;

  if (gwi.thd)
  {
    // if ( ((gwi.thd->lex)->sql_command == SQLCOM_UPDATE ) || ((gwi.thd->lex)->sql_command ==
    // SQLCOM_UPDATE_MULTI ))
    {
      if (!item->fixed())
      {
        item->fix_fields(gwi.thd, (Item**)&item);
      }
    }
  }

  switch (item->type())
  {
    case Item::FIELD_ITEM:
    {
      Item_field* ifp = (Item_field*)item;

      return wrapIntoAggregate(buildSimpleColumn(ifp, gwi), gwi, ifp);
    }
    case Item::NULL_ITEM: return buildReturnedColumnNull(gwi);
    case Item::CONST_ITEM:
    {
      rc = buildConstantColumnNotNullUsingValNative(item, gwi);
      break;
    }
    case Item::FUNC_ITEM:
    {
      if (item->const_item())
      {
        rc = buildConstantColumnMaybeNullUsingValStr(item, gwi);
        break;
      }
      Item_func* ifp = (Item_func*)item;
      string func_name = ifp->func_name();

      // try to evaluate const F&E. only for select clause
      vector<Item_field*> tmpVec;
      // bool hasAggColumn = false;
      uint16_t parseInfo = 0;
      parse_item(ifp, tmpVec, gwi.fatalParseError, parseInfo, &gwi);

      if (parseInfo & SUB_BIT)
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_SELECT_SUB);
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
        break;
      }

      if (!gwi.fatalParseError && !nonConstFunc(ifp) && !(parseInfo & AF_BIT) && (tmpVec.size() == 0))
      {
        rc = buildConstantColumnMaybeNullUsingValStr(item, gwi);
        break;
      }

      if (func_name == "+" || func_name == "-" || func_name == "*" || func_name == "/")
        return buildArithmeticColumn(ifp, gwi, nonSupport);
      else
      {
        return buildFunctionColumn(ifp, gwi, nonSupport);
      }
    }

    case Item::SUM_FUNC_ITEM:
    {
      return buildAggregateColumn(item, gwi);
    }

    case Item::REF_ITEM:
    {
      Item_ref* ref = (Item_ref*)item;

      switch ((*(ref->ref))->type())
      {
        case Item::SUM_FUNC_ITEM: return buildAggregateColumn(*(ref->ref), gwi);

        case Item::FIELD_ITEM: return buildReturnedColumn(*(ref->ref), gwi, nonSupport);

        case Item::REF_ITEM: return buildReturnedColumn(*(((Item_ref*)(*(ref->ref)))->ref), gwi, nonSupport);

        case Item::FUNC_ITEM: return buildFunctionColumn((Item_func*)(*(ref->ref)), gwi, nonSupport);

        case Item::WINDOW_FUNC_ITEM: return buildWindowFunctionColumn(*(ref->ref), gwi, nonSupport);

        case Item::SUBSELECT_ITEM:
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_SELECT_SUB);
          break;

        default:
          if (ref->ref_type() == Item_ref::DIRECT_REF)
          {
            return buildReturnedColumn(ref->real_item(), gwi, nonSupport);
          }
          gwi.fatalParseError = true;
          gwi.parseErrorText = "Unknown REF item";
          break;
      }
      return buildReturnedColumnNull(gwi);
    }
    case Item::CACHE_ITEM:
    {
      Item* col = ((Item_cache*)item)->get_example();
      rc = buildReturnedColumn(col, gwi, nonSupport);

      if (rc)
      {
        ConstantColumn* cc = dynamic_cast<ConstantColumn*>(rc);

        if (!cc)
        {
          rc->joinInfo(rc->joinInfo() | JOIN_CORRELATED);

          if (gwi.subQuery)
            gwi.subQuery->correlated(true);
        }
      }

      break;
    }

    case Item::EXPR_CACHE_ITEM:
    {
      // TODO: item is a Item_cache_wrapper
      printf("EXPR_CACHE_ITEM in buildReturnedColumn\n");
      cerr << "EXPR_CACHE_ITEM in buildReturnedColumn" << endl;
      break;
    }

    case Item::WINDOW_FUNC_ITEM:
    {
      return buildWindowFunctionColumn(item, gwi, nonSupport);
    }

#if INTERVAL_ITEM

    case Item::INTERVAL_ITEM:
    {
      Item_interval* interval = (Item_interval*)item;
      SRCP srcp;
      srcp.reset(buildReturnedColumn(interval->item, gwi, nonSupport));

      if (!srcp)
        return NULL;

      rc = new IntervalColumn(srcp, (int)interval->interval);
      rc->resultType(srcp->resultType());
      break;
    }

#endif

    case Item::SUBSELECT_ITEM:
    {
      gwi.hasSubSelect = true;
      break;
    }

    case Item::COND_ITEM:
    {
      // MCOL-1196: Allow COND_ITEM thru. They will be picked up
      // by further logic. It may become desirable to add code here.
      break;
    }

    default:
    {
      gwi.fatalParseError = true;
      gwi.parseErrorText = "Unknown item type";
      break;
    }
  }

  if (rc && item->name.length)
    rc->alias(item->name.str);

  if (rc)
    rc->charsetNumber(item->collation.collation->number);

  return rc;
}
ReturnedColumn* buildReturnedColumn(Item* item, gp_walk_info& gwi, bool& nonSupport, bool isRefItem)
{
  bool disableWrapping = gwi.disableWrapping;
  gwi.disableWrapping = gwi.disableWrapping || itemDisablesWrapping(item, gwi);
  ReturnedColumn* rc = buildReturnedColumnBody(item, gwi, nonSupport, isRefItem);
  gwi.disableWrapping = disableWrapping;
  return rc;
}

// parse the boolean fields to string "true" or "false"
ReturnedColumn* buildBooleanConstantColumn(Item* item, gp_walk_info& gwi, bool& /*nonSupport*/)
{
  ConstantColumn* cc = NULL;

  if (gwi.thd)
  {
    {
      if (!item->fixed())
      {
        item->fix_fields(gwi.thd, (Item**)&item);
      }
    }
  }
  int64_t val = static_cast<int64_t>(item->val_int());
  cc = new ConstantColumnSInt(colType_MysqlToIDB(item), val ? "true" : "false", val);

  if (cc)
    cc->timeZone(gwi.timeZone);

  if (cc && item->name.length)
    cc->alias(item->name.str);

  if (cc)
    cc->charsetNumber(item->collation.collation->number);

  return cc;
}

ReturnedColumn* buildArithmeticColumnBody(Item_func* item, gp_walk_info& gwi, bool& nonSupport)
{
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  ArithmeticColumn* ac = new ArithmeticColumn();
  Item** sfitempp = item->arguments();
  ArithmeticOperator* aop = new ArithmeticOperator(item->func_name());
  aop->timeZone(gwi.timeZone);
  aop->setOverflowCheck(get_decimal_overflow_check(gwi.thd));
  ParseTree* pt = new ParseTree(aop);
  // ReturnedColumn *lhs = 0, *rhs = 0;
  ParseTree *lhs = 0, *rhs = 0;
  SRCP srcp;

  if (item->name.length)
    ac->alias(item->name.str);

  // argument_count() should generally be 2, except negate expression
  if (item->argument_count() == 2)
  {
    if (gwi.clauseType == SELECT || /*gwi.clauseType == HAVING || */ gwi.clauseType == GROUP_BY ||
        gwi.clauseType == FROM)  // select list
    {
      lhs = new ParseTree(buildReturnedColumn(sfitempp[0], gwi, nonSupport));

      if (!lhs->data() && (sfitempp[0]->type() == Item::FUNC_ITEM))
      {
        delete lhs;
        lhs = buildParseTree(sfitempp[0], gwi, nonSupport);
      }
      else if (!lhs->data() && (sfitempp[0]->type() == Item::REF_ITEM))
      {
        // There must be an aggregation column in extended SELECT
        // list so find the corresponding column.
        // Could have it set if there are aggregation funcs as this function arguments.
        gwi.fatalParseError = false;

        // ReturnedColumn* rc = buildAggFrmTempField(sfitempp[0], gwi);
        ReturnedColumn* rc = buildReturnedColumn(sfitempp[0], gwi, nonSupport);
        if (rc)
          lhs = new ParseTree(rc);
      }

      rhs = new ParseTree(buildReturnedColumn(sfitempp[1], gwi, nonSupport));

      if (!rhs->data() && (sfitempp[1]->type() == Item::FUNC_ITEM))
      {
        delete rhs;
        rhs = buildParseTree(sfitempp[1], gwi, nonSupport);
      }
      else if (!rhs->data() && (sfitempp[1]->type() == Item::REF_ITEM))
      {
        // There must be an aggregation column in extended SELECT
        // list so find the corresponding column.
        // Could have it set if there are aggregation funcs as this function arguments.
        gwi.fatalParseError = false;

        // ReturnedColumn* rc = buildAggFrmTempField(sfitempp[1], gwi);
        ReturnedColumn* rc = buildReturnedColumn(sfitempp[1], gwi, nonSupport);
        if (rc)
          rhs = new ParseTree(rc);
      }
    }
    else  // where clause SZ: XXX: is it also HAVING clause??? it appears so judging from condition above.
    {
      if (isPredicateFunction(sfitempp[1], &gwi))
      {
        if (gwi.ptWorkStack.empty())
        {
          rhs = new ParseTree(buildReturnedColumn(sfitempp[1], gwi, nonSupport));
        }
        else
        {
          rhs = gwi.ptWorkStack.top();
          gwi.ptWorkStack.pop();
        }
      }
      else
      {
        if (gwi.rcWorkStack.empty())
        {
          rhs = new ParseTree(buildReturnedColumn(sfitempp[1], gwi, nonSupport));
        }
        else
        {
          rhs = new ParseTree(gwi.rcWorkStack.top());
          gwi.rcWorkStack.pop();
        }
      }

      if (isPredicateFunction(sfitempp[0], &gwi))
      {
        if (gwi.ptWorkStack.empty())
        {
          lhs = new ParseTree(buildReturnedColumn(sfitempp[0], gwi, nonSupport));
        }
        else
        {
          lhs = gwi.ptWorkStack.top();
          gwi.ptWorkStack.pop();
        }
      }
      else
      {
        if (gwi.rcWorkStack.empty())
        {
          lhs = new ParseTree(buildReturnedColumn(sfitempp[0], gwi, nonSupport));
        }
        else
        {
          lhs = new ParseTree(gwi.rcWorkStack.top());
          gwi.rcWorkStack.pop();
        }
      }
    }

    if (nonSupport || !lhs->data() || !rhs->data())
    {
      gwi.fatalParseError = true;

      if (gwi.parseErrorText.empty())
        gwi.parseErrorText = "Un-recognized Arithmetic Operand";

      delete lhs;
      delete rhs;
      return NULL;
    }

    // aop->operationType(lhs->resultType(), rhs->resultType());
    pt->left(lhs);
    pt->right(rhs);
  }
  else
  {
    ConstantColumn* cc = new ConstantColumn(string("0"), (int64_t)0);
    cc->timeZone(gwi.timeZone);

    if (gwi.clauseType == SELECT || gwi.clauseType == HAVING || gwi.clauseType == GROUP_BY)  // select clause
    {
      rhs = new ParseTree(buildReturnedColumn(sfitempp[0], gwi, nonSupport));
    }
    else
    {
      if (gwi.rcWorkStack.empty())
      {
        rhs = new ParseTree(buildReturnedColumn(sfitempp[0], gwi, nonSupport));
      }
      else
      {
        rhs = new ParseTree(gwi.rcWorkStack.top());
        gwi.rcWorkStack.pop();
      }
    }

    if (nonSupport || !rhs->data())
    {
      gwi.fatalParseError = true;

      if (gwi.parseErrorText.empty())
        gwi.parseErrorText = "Un-recognized Arithmetic Operand";

      delete rhs;
      return NULL;
    }

    pt->left(cc);
    pt->right(rhs);
  }

  // @bug5715. Use InfiniDB adjusted coltype for result type.
  // decimal arithmetic operation gives double result when the session variable is set.
  CalpontSystemCatalog::ColType mysqlType = colType_MysqlToIDB(item);

  const CalpontSystemCatalog::ColType& leftColType = pt->left()->data()->resultType();
  const CalpontSystemCatalog::ColType& rightColType = pt->right()->data()->resultType();

  if (datatypes::isDecimal(leftColType.colDataType) || datatypes::isDecimal(rightColType.colDataType))
  {
    int32_t leftColWidth = leftColType.colWidth;
    int32_t rightColWidth = rightColType.colWidth;

    if ((leftColWidth == datatypes::MAXDECIMALWIDTH || rightColWidth == datatypes::MAXDECIMALWIDTH) &&
        datatypes::isDecimal(mysqlType.colDataType))
    {
      mysqlType.colWidth = datatypes::MAXDECIMALWIDTH;

      string funcName = item->func_name();

      int32_t scale1 = leftColType.scale;
      int32_t scale2 = rightColType.scale;

      mysqlType.precision = datatypes::INT128MAXPRECISION;

      if (funcName == "/" && (mysqlType.scale - (scale1 - scale2)) > datatypes::INT128MAXPRECISION)
      {
        Item_decimal* idp = (Item_decimal*)item;

        unsigned int precision = idp->decimal_precision();
        unsigned int scale = idp->decimal_scale();

        mysqlType.setDecimalScalePrecisionHeuristic(precision, scale);

        if (mysqlType.scale < scale1)
          mysqlType.scale = scale1;

        if (mysqlType.precision < mysqlType.scale)
          mysqlType.precision = mysqlType.scale;
      }
    }
  }

  if (get_double_for_decimal_math(current_thd) == true)
    aop->adjustResultType(mysqlType);
  else
    aop->resultType(mysqlType);

  // adjust decimal result type according to internalDecimalScale
  if (gwi.internalDecimalScale >= 0 && aop->resultType().colDataType == CalpontSystemCatalog::DECIMAL)
  {
    CalpontSystemCatalog::ColType ct = aop->resultType();
    ct.scale = gwi.internalDecimalScale;
    aop->resultType(ct);
  }

  aop->operationType(aop->resultType());
  ac->expression(pt);
  ac->resultType(aop->resultType());
  ac->operationType(aop->operationType());
  ac->expressionId(ci->expressionId++);

  // @3391. optimization. try to associate expression ID to the expression on the select list
  bool isOnSelectList = false;
  if (gwi.clauseType != SELECT || gwi.havingDespiteSelect)
  {
    for (uint32_t i = 0; i < gwi.returnedCols.size(); i++)
    {
      if ((!ac->alias().empty()) &&
          strcasecmp(ac->alias().c_str(), gwi.returnedCols[i]->alias().c_str()) == 0)
      {
        ac->expressionId(gwi.returnedCols[i]->expressionId());
        isOnSelectList = true;
        break;
      }
    }
  }

  // For function join. If any argument has non-zero joininfo, set it to the function.
  ac->setSimpleColumnList();
  std::vector<SimpleColumn*> simpleColList = ac->simpleColumnList();

  for (uint i = 0; i < simpleColList.size(); i++)
  {
    if (simpleColList[i]->joinInfo() != 0)
    {
      ac->joinInfo(simpleColList[i]->joinInfo());
      break;
    }
  }

  if (isOnSelectList && gwi.havingDespiteSelect)
  {
    SimpleColumn* sc = new SimpleColumn(*ac);
    delete ac;
    return sc;
  }
  return ac;
}
ReturnedColumn* buildArithmeticColumn(Item_func* item, gp_walk_info& gwi, bool& nonSupport)
{
  bool disableWrapping = gwi.disableWrapping;
  gwi.disableWrapping = gwi.disableWrapping || itemDisablesWrapping(item, gwi);
  ReturnedColumn* rc = buildArithmeticColumnBody(item, gwi, nonSupport);
  gwi.disableWrapping = disableWrapping;
  return rc;
}

ReturnedColumn* buildFunctionColumnBody(Item_func* ifp, gp_walk_info& gwi, bool& nonSupport,
                                        bool selectBetweenIn)
{
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  string funcName = ifp->func_name();
  if (nullptr != dynamic_cast<Item_func_concat_operator_oracle*>(ifp))
  {
    // the condition above is the only way to recognize this particular case.
    funcName = "concat_operator_oracle";
  }
  else
  {
    const Schema* funcSchema = ifp->schema();
    if (funcSchema)
    {
      idbassert(funcSchema->name().str);
      string funcSchemaName(funcSchema->name().str, funcSchema->name().length);
      if (funcSchemaName == "oracle_schema")
      {
        // XXX: this is a shortcut.
        funcName = funcName + "_oracle";
      }
    }
  }
  FuncExp* funcExp = FuncExp::instance();
  Func* functor;
  FunctionColumn* fc = NULL;

  // Pseudocolumn
  uint32_t pseudoType = PSEUDO_UNKNOWN;

  if (ifp->functype() == Item_func::UDF_FUNC)
    pseudoType = PseudoColumn::pseudoNameToType(funcName);

  if (pseudoType != PSEUDO_UNKNOWN)
  {
    ReturnedColumn* rc = buildPseudoColumn(ifp, gwi, gwi.fatalParseError, pseudoType);

    if (!rc || gwi.fatalParseError)
    {
      if (gwi.parseErrorText.empty())
        gwi.parseErrorText = "Unsupported function.";

      setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
      return NULL;
    }

    return rc;
  }

  // Item_equal is supported by buildPredicateItem()
  if (funcName == "multiple equal")
    return NULL;

  // Arithmetic exp
  if (funcName == "+" || funcName == "-" || funcName == "*" || funcName == "/")
  {
    return buildArithmeticColumn(ifp, gwi, nonSupport);
  }

  else if (funcName == "case")
  {
    fc = buildCaseFunction(ifp, gwi, nonSupport);
  }

  else if ((funcName == "charset" || funcName == "collation") && ifp->argument_count() == 1 &&
           ifp->arguments()[0]->type() == Item::FIELD_ITEM)
  {
    Item_field* item = static_cast<Item_field*>(ifp->arguments()[0]);
    CHARSET_INFO* info = item->charset_for_protocol();
    ReturnedColumn* rc;
    string val;

    if (funcName == "charset")
    {
      val = info->cs_name.str;
    }
    else  // collation
    {
      val = info->coll_name.str;
    }

    rc = new ConstantColumn(val, ConstantColumn::LITERAL);

    return rc;
  }

  else if ((functor = funcExp->getFunctor(funcName)))
  {
    // where clause isnull still treated as predicate operator
    // MCOL-686: between also a predicate operator so that extent elimination can happen
    if ((funcName == "isnull" || funcName == "isnotnull" || funcName == "between") &&
        (gwi.clauseType == WHERE || gwi.clauseType == HAVING))
      return NULL;

    if (funcName == "in" || funcName == " IN " || funcName == "between")
    {
      // if F&E involved, build function. otherwise, fall to the old path.
      // @todo need more checks here
      if (ifp->arguments()[0]->type() == Item::ROW_ITEM)
      {
        return NULL;
      }
      if (ifp->argument_count() > std::numeric_limits<uint16_t>::max())
      {
        nonSupport = true;
        gwi.fatalParseError = true;
        Message::Args args;
        string info =
            funcName + " with argument count > " + std::to_string(std::numeric_limits<uint16_t>::max());
        args.add(info);
        gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORTED_FUNCTION, args);
        return NULL;
      }

      if (!selectBetweenIn && (ifp->arguments()[0]->type() == Item::FIELD_ITEM ||
                               (ifp->arguments()[0]->type() == Item::REF_ITEM &&
                                (*(((Item_ref*)ifp->arguments()[0])->ref))->type() == Item::FIELD_ITEM)))
      {
        bool fe = false;

        for (uint32_t i = 1; i < ifp->argument_count(); i++)
        {
          if (!(ifp->arguments()[i]->type() == Item::NULL_ITEM ||
                (ifp->arguments()[i]->type() == Item::CONST_ITEM &&
                 (ifp->arguments()[i]->cmp_type() == INT_RESULT ||
                  ifp->arguments()[i]->cmp_type() == STRING_RESULT ||
                  ifp->arguments()[i]->cmp_type() == REAL_RESULT ||
                  ifp->arguments()[i]->cmp_type() == DECIMAL_RESULT))))
          {
            if (ifp->arguments()[i]->type() == Item::FUNC_ITEM)
            {
              // try to identify const F&E. fall to primitive if parms are constant F&E.
              vector<Item_field*> tmpVec;
              uint16_t parseInfo = 0;
              parse_item(ifp->arguments()[i], tmpVec, gwi.fatalParseError, parseInfo, &gwi);

              if (!gwi.fatalParseError && !(parseInfo & AF_BIT) && tmpVec.size() == 0)
                continue;
            }

            fe = true;
            break;
          }
        }

        if (!fe)
          return NULL;
      }

      Item_func_opt_neg* inp = (Item_func_opt_neg*)ifp;

      if (inp->negated)
        funcName = "not" + funcName;
    }

    // @todo non-support function as argument. need to do post process. Assume all support for now
    fc = new FunctionColumn();
    fc->data(funcName);
    FunctionParm funcParms;
    SPTP sptp;
    ClauseType clauseType = gwi.clauseType;

    if (gwi.clauseType == SELECT ||
        /*gwi.clauseType == HAVING || */ gwi.clauseType == GROUP_BY)  // select clause
    {
      for (uint32_t i = 0; i < ifp->argument_count(); i++)
      {
        // group by clause try to see if the arguments are alias
        if (gwi.clauseType == GROUP_BY && ifp->arguments()[i]->name.length)
        {
          uint32_t j = 0;

          for (; j < gwi.returnedCols.size(); j++)
          {
            if (string(ifp->arguments()[i]->name.str) == gwi.returnedCols[j]->alias())
            {
              ReturnedColumn* rc = gwi.returnedCols[j]->clone();
              rc->orderPos(j);
              sptp.reset(new ParseTree(rc));
              funcParms.push_back(sptp);
              break;
            }
          }

          if (j != gwi.returnedCols.size())
            continue;
        }

        // special handling for function that takes a filter arguments, like if().
        // @todo. merge this logic to buildParseTree().
        if ((funcName == "if" && i == 0) || funcName == "xor")
        {
          // make sure the rcWorkStack is cleaned.
          gwi.clauseType = WHERE;
          sptp.reset(buildParseTree(ifp->arguments()[i], gwi, nonSupport));
          gwi.clauseType = clauseType;

          if (!sptp)
          {
            nonSupport = true;
            delete fc;
            return NULL;
          }

          funcParms.push_back(sptp);
          continue;
        }

        // @bug 3039
        // if (isPredicateFunction(ifp->arguments()[i], &gwi) || ifp->arguments()[i]->with_subquery())
        if (ifp->arguments()[i]->with_subquery())
        {
          nonSupport = true;
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_SUB_EXPRESSION);
          delete fc;
          return NULL;
        }

        ReturnedColumn* rc = NULL;

        // Special treatment for json functions
        // All boolean arguments will be parsed as boolean string true(false)
        // E.g. the result of `SELECT JSON_ARRAY(true, false)` should be [true, false] instead of [1, 0]
        bool mayHasBoolArg =
            ((funcName == "json_insert" || funcName == "json_replace" || funcName == "json_set" ||
              funcName == "json_array_append" || funcName == "json_array_insert") &&
             i != 0 && i % 2 == 0) ||
            (funcName == "json_array") || (funcName == "json_object" && i % 2 == 1);
        bool isBoolType =
            (ifp->arguments()[i]->const_item() && ifp->arguments()[i]->type_handler()->is_bool_type());

        if (mayHasBoolArg && isBoolType)
          rc = buildBooleanConstantColumn(ifp->arguments()[i], gwi, nonSupport);
        else
        {
          rc = buildReturnedColumn(ifp->arguments()[i], gwi, nonSupport);
        }

        // MCOL-1510 It must be a temp table field, so find the corresponding column.
        if (!rc && ifp->arguments()[i]->type() == Item::REF_ITEM)
        {
          gwi.fatalParseError = false;
          rc = buildAggFrmTempField(ifp->arguments()[i], gwi);
        }

        if (!rc || nonSupport)
        {
          nonSupport = true;
          delete fc;
          return NULL;
        }

        sptp.reset(new ParseTree(rc));
        funcParms.push_back(sptp);
      }
    }
    else  // where clause
    {
      stack<SPTP> tmpPtStack;

      for (int32_t i = ifp->argument_count() - 1; i >= 0; i--)
      {
        if (isPredicateFunction((ifp->arguments()[i]), &gwi) && !gwi.ptWorkStack.empty())
        {
          sptp.reset(gwi.ptWorkStack.top());
          tmpPtStack.push(sptp);
          gwi.ptWorkStack.pop();
        }
        else if (!isPredicateFunction((ifp->arguments()[i]), &gwi) && !gwi.rcWorkStack.empty())
        {
          sptp.reset(new ParseTree(gwi.rcWorkStack.top()));
          tmpPtStack.push(sptp);
          gwi.rcWorkStack.pop();
        }
        else
        {
          nonSupport = true;
          delete fc;
          return NULL;
        }
      }

      while (!tmpPtStack.empty())
      {
        funcParms.push_back(tmpPtStack.top());
        tmpPtStack.pop();
      }
    }

    // the followings are special treatment of some functions
    if (funcName == "week" && funcParms.size() == 1)
    {
      THD* thd = current_thd;
      sptp.reset(
          new ParseTree(new ConstantColumn(static_cast<uint64_t>(thd->variables.default_week_format))));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
      funcParms.push_back(sptp);
    }

    // add the keyword unit argument for interval function
    if (funcName == "date_add_interval" || funcName == "extract" || funcName == "timestampdiff")
    {
      addIntervalArgs(&gwi, ifp, funcParms);
    }

    // add the keyword unit argument and char length for cast functions
    if (funcName == "cast_as_char")
    {
      castCharArgs(&gwi, ifp, funcParms);
    }

    // add the length and scale arguments
    if (funcName == "decimal_typecast")
    {
      castDecimalArgs(&gwi, ifp, funcParms);
    }

    // add the type argument
    if (funcName == "get_format")
    {
      castTypeArgs(&gwi, ifp, funcParms);
    }

    // add my_time_zone
    if (funcName == "unix_timestamp")
    {
      time_t tmp_t = 1;
      struct tm tmp;
      localtime_r(&tmp_t, &tmp);
      sptp.reset(new ParseTree(new ConstantColumn(static_cast<int64_t>(tmp.tm_gmtoff), ConstantColumn::NUM)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
      funcParms.push_back(sptp);
    }

    // add the default seed to rand function without arguments
    if (funcName == "rand")
    {
      if (funcParms.size() == 0)
      {
        sptp.reset(new ParseTree(new ConstantColumn((int64_t)gwi.thd->rand.seed1, ConstantColumn::NUM)));
        (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
        funcParms.push_back(sptp);
        sptp.reset(new ParseTree(new ConstantColumn((int64_t)gwi.thd->rand.seed2, ConstantColumn::NUM)));
        (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
        funcParms.push_back(sptp);
        gwi.no_parm_func_list.push_back(fc);
      }
      else
      {
        ConstantColumn* cc = dynamic_cast<ConstantColumn*>(funcParms[0]->data());

        if (cc)
          gwi.no_parm_func_list.push_back(fc);
      }
    }

    // Take the value of to/from TZ data and check if its a description or offset via
    // my_tzinfo_find. Offset value will leave the corresponding tzinfo funcParms empty.
    // while descriptions will lookup the time_zone_info structure and serialize for use
    // in primproc func_convert_tz
    if (funcName == "convert_tz")
    {
      messageqcpp::ByteStream bs;
      string tzinfo;
      SimpleColumn* scCheck;
      // MCOL-XXXX There is no way currently to perform this lookup when the timezone description
      // comes from another table of timezone descriptions.
      // 1. Move proc code into plugin where it will have access to this table data
      // 2. Create a library that primproc can use to access the time zone data tables.
      // for now throw a message that this is not supported
      scCheck = dynamic_cast<SimpleColumn*>(funcParms[1]->data());
      if (scCheck)
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = "convert_tz with parameter column_name in a Columnstore table";
        setError(gwi.thd, ER_NOT_SUPPORTED_YET, gwi.parseErrorText, gwi);
        return NULL;
      }
      scCheck = dynamic_cast<SimpleColumn*>(funcParms[2]->data());
      if (scCheck)
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = "convert_tz with parameter column_name in a Columnstore table";
        setError(gwi.thd, ER_NOT_SUPPORTED_YET, gwi.parseErrorText, gwi);
        return NULL;
      }
      dataconvert::TIME_ZONE_INFO* from_tzinfo = my_tzinfo_find(gwi.thd, ifp->arguments()[1]->val_str());
      dataconvert::TIME_ZONE_INFO* to_tzinfo = my_tzinfo_find(gwi.thd, ifp->arguments()[2]->val_str());
      if (from_tzinfo)
      {
        serializeTimezoneInfo(bs, from_tzinfo);
        messageqcpp::BSSizeType length = bs.length();
        uint8_t* buf = new uint8_t[length];
        bs >> buf;
        tzinfo = string((char*)buf, length);
      }
      sptp.reset(new ParseTree(new ConstantColumn(tzinfo)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
      funcParms.push_back(sptp);
      tzinfo.clear();
      if (to_tzinfo)
      {
        serializeTimezoneInfo(bs, to_tzinfo);
        messageqcpp::BSSizeType length = bs.length();
        uint8_t* buf = new uint8_t[length];
        bs >> buf;
        tzinfo = string((char*)buf, length);
      }
      sptp.reset(new ParseTree(new ConstantColumn(tzinfo)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
      funcParms.push_back(sptp);
      tzinfo.clear();
    }
    if (funcName == "sysdate")
    {
      gwi.no_parm_func_list.push_back(fc);
    }

    // func name is addtime/subtime in 10.3.9
    // note: this means get_time() can now go away in our server fork
    if ((funcName == "addtime") || (funcName == "subtime"))
    {
      int64_t sign = 1;
      if (funcName == "subtime")
      {
        sign = -1;
      }
      sptp.reset(new ParseTree(new ConstantColumn(sign)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwi.timeZone);
      funcParms.push_back(sptp);
    }

    fc->functionName(funcName);
    fc->functionParms(funcParms);
    fc->resultType(colType_MysqlToIDB(ifp));

    // if the result type is DECIMAL and any function parameter is a wide decimal
    // column, set the result colwidth to wide
    if (fc->resultType().colDataType == CalpontSystemCatalog::DECIMAL)
    {
      for (size_t i = 0; i < funcParms.size(); i++)
      {
        if (funcParms[i]->data()->resultType().isWideDecimalType())
        {
          fc->resultType().colWidth = datatypes::MAXDECIMALWIDTH;
          break;
        }
      }
    }

    // MySQL give string result type for date function, but has the flag set.
    // we should set the result type to be datetime for comparision.
    if (ifp->field_type() == MYSQL_TYPE_DATETIME || ifp->field_type() == MYSQL_TYPE_DATETIME2)
    {
      CalpontSystemCatalog::ColType ct;
      ct.colDataType = CalpontSystemCatalog::DATETIME;
      ct.colWidth = 8;
      fc->resultType(ct);
    }
    if (ifp->field_type() == MYSQL_TYPE_TIMESTAMP || ifp->field_type() == MYSQL_TYPE_TIMESTAMP2)
    {
      CalpontSystemCatalog::ColType ct;
      ct.colDataType = CalpontSystemCatalog::TIMESTAMP;
      ct.colWidth = 8;
      fc->resultType(ct);
    }
    else if (ifp->field_type() == MYSQL_TYPE_DATE)
    {
      CalpontSystemCatalog::ColType ct;
      ct.colDataType = CalpontSystemCatalog::DATE;
      ct.colWidth = 4;
      fc->resultType(ct);
    }
    else if (ifp->field_type() == MYSQL_TYPE_TIME)
    {
      CalpontSystemCatalog::ColType ct;
      ct.colDataType = CalpontSystemCatalog::TIME;
      ct.colWidth = 8;
      fc->resultType(ct);
    }
    if (funcName == "last_day")
    {
      CalpontSystemCatalog::ColType ct;
      ct.colDataType = CalpontSystemCatalog::DATE;
      ct.colWidth = 4;
      fc->resultType(ct);
    }

#if 0

        if (is_temporal_type_with_date(ifp->field_type()))
        {
            CalpontSystemCatalog::ColType ct;
            ct.colDataType = CalpontSystemCatalog::DATETIME;
            ct.colWidth = 8;
            fc->resultType(ct);
        }

        if (funcName == "cast_as_date")
        {
            CalpontSystemCatalog::ColType ct;
            ct.colDataType = CalpontSystemCatalog::DATE;
            ct.colWidth = 4;
            fc->resultType(ct);
        }

#endif

    execplan::CalpontSystemCatalog::ColType& resultType = fc->resultType();
    resultType.setTimeZone(gwi.timeZone);
    fc->operationType(functor->operationType(funcParms, resultType));

    // For floor/ceiling/truncate/round functions applied on TIMESTAMP columns, set the
    // function result type to TIMESTAMP
    if ((funcName == "floor" || funcName == "ceiling" || funcName == "truncate" || funcName == "round") &&
        fc->operationType().colDataType == CalpontSystemCatalog::TIMESTAMP)
    {
      CalpontSystemCatalog::ColType ct = fc->resultType();
      ct.colDataType = CalpontSystemCatalog::TIMESTAMP;
      ct.colWidth = 8;
      fc->resultType(ct);
    }

    fc->expressionId(ci->expressionId++);

    // A few functions use a different collation than that found in
    // the base ifp class
    if (funcName == "locate" || funcName == "find_in_set" || funcName == "strcmp" ||
        funcName == "regexp_instr")
    {
      DTCollation dt;
      ifp->Type_std_attributes::agg_arg_charsets_for_comparison(dt, ifp->func_name_cstring(),
                                                                ifp->arguments(), 1, 1);
      fc->charsetNumber(dt.collation->number);
    }
    else
    {
      fc->charsetNumber(ifp->collation.collation->number);
    }
  }
  else if (ifp->type() == Item::COND_ITEM || ifp->functype() == Item_func::EQ_FUNC ||
           ifp->functype() == Item_func::NE_FUNC || ifp->functype() == Item_func::LT_FUNC ||
           ifp->functype() == Item_func::LE_FUNC || ifp->functype() == Item_func::GE_FUNC ||
           ifp->functype() == Item_func::GT_FUNC || ifp->functype() == Item_func::LIKE_FUNC ||
           ifp->functype() == Item_func::BETWEEN || ifp->functype() == Item_func::IN_FUNC ||
           ifp->functype() == Item_func::ISNULL_FUNC || ifp->functype() == Item_func::ISNOTNULL_FUNC ||
           ifp->functype() == Item_func::NOT_FUNC || ifp->functype() == Item_func::EQUAL_FUNC)
  {
    return NULL;
  }
  else
  {
    nonSupport = true;
    gwi.fatalParseError = true;
    Message::Args args;
    args.add(funcName);
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORTED_FUNCTION, args);
    return NULL;
  }

  // adjust decimal result type according to internalDecimalScale
  if (!fc)
    return NULL;

  if (gwi.internalDecimalScale >= 0 && fc->resultType().colDataType == CalpontSystemCatalog::DECIMAL)
  {
    CalpontSystemCatalog::ColType ct = fc->resultType();
    ct.scale = gwi.internalDecimalScale;
    fc->resultType(ct);
  }

  if (ifp->name.length)
    fc->alias(ifp->name.str);

  // @3391. optimization. try to associate expression ID to the expression on the select list
  if (gwi.clauseType != SELECT)
  {
    for (uint32_t i = 0; i < gwi.returnedCols.size(); i++)
    {
      if ((!fc->alias().empty()) &&
          strcasecmp(fc->alias().c_str(), gwi.returnedCols[i]->alias().c_str()) == 0)
        fc->expressionId(gwi.returnedCols[i]->expressionId());
    }
  }

  // For function join. If any argument has non-zero joininfo, set it to the function.
  fc->setSimpleColumnList();
  std::vector<SimpleColumn*> simpleColList = fc->simpleColumnList();

  for (uint i = 0; i < simpleColList.size(); i++)
  {
    if (simpleColList[i]->joinInfo() != 0)
    {
      fc->joinInfo(simpleColList[i]->joinInfo());
      break;
    }
  }

  fc->timeZone(gwi.timeZone);

  return fc;
}
ReturnedColumn* buildFunctionColumn(Item_func* ifp, gp_walk_info& gwi, bool& nonSupport, bool selectBetweenIn)
{
  bool disableWrapping = gwi.disableWrapping;
  gwi.disableWrapping = gwi.disableWrapping || itemDisablesWrapping(ifp, gwi);
  ReturnedColumn* rc = buildFunctionColumnBody(ifp, gwi, nonSupport, selectBetweenIn);
  gwi.disableWrapping = disableWrapping;
  return rc;
}

FunctionColumn* buildCaseFunction(Item_func* item, gp_walk_info& gwi, bool& nonSupport)
{
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  FunctionColumn* fc = new FunctionColumn();
  FunctionParm funcParms;
  SPTP sptp;
  stack<SPTP> tmpPtStack;
  FuncExp* funcexp = FuncExp::instance();
  string funcName = "case_simple";

  if (item->functype() == Item_func::CASE_SEARCHED_FUNC)
  {
    funcName = "case_searched";
  }
  /*    if (dynamic_cast<Item_func_case_searched*>(item))
      {
          funcName = "case_searched";
      }*/

  funcParms.reserve(item->argument_count());
  // so buildXXXcolumn function will not pop stack.
  ClauseType realClauseType = gwi.clauseType;
  gwi.clauseType = SELECT;

  // We ought to be able to just build from the stack, and would
  // be able to if there were any way to know which stack had the
  // next case item. Unfortunately, parameters may have been pushed
  // onto the ptWorkStack or rcWorkStack or neither, depending on type
  // and position. We can't tell which at this point, so we
  // rebuild the item from the arguments directly and then try to
  // figure what to pop, if anything, in order to sync the stacks.
  //
  // MCOL-1341 - With MariaDB 10.2.14 onwards CASE is now in the order:
  // [case,]when1,when2,...,then1,then2,...[,else]
  // See server commit bf1ca14ff3f3faa9f7a018097b25aa0f66d068cd for more
  // information.
  int32_t arg_offset = 0;

  if ((item->argument_count() - 1) % 2)
  {
    arg_offset = (item->argument_count() - 1) / 2;
  }
  else
  {
    arg_offset = item->argument_count() / 2;
  }

  for (int32_t i = item->argument_count() - 1; i >= 0; i--)
  {
    // For case_searched, we know the items for the WHEN clause will
    // not be ReturnedColumns. We do this separately just to save
    // some cpu cycles trying to build a ReturnedColumn as below.
    // Every even numbered arg is a WHEN. In between are the THEN.
    // An odd number of args indicates an ELSE residing in the last spot.
    if ((item->functype() == Item_func::CASE_SEARCHED_FUNC) && (i < arg_offset))
    {
      // MCOL-1472 Nested CASE with an ISNULL predicate. We don't want the predicate
      // to pull off of rcWorkStack, so we set this inCaseStmt flag to tell it
      // not to.
      gwi.inCaseStmt = true;
      sptp.reset(buildParseTree(item->arguments()[i], gwi, nonSupport));
      gwi.inCaseStmt = false;
      if (!gwi.ptWorkStack.empty() && *gwi.ptWorkStack.top() == *sptp.get())
      {
        delete gwi.ptWorkStack.top();
        gwi.ptWorkStack.pop();
      }
    }
    else
    {
      // First try building a ReturnedColumn. It may or may not succeed
      // depending on the types involved. There's also little correlation
      // between buildReturnedColumn and the existance of the item on
      // rwWorkStack or ptWorkStack.
      // For example, simple predicates, such as 1=1 or 1=0, land in the
      // ptWorkStack but other stuff might land in the rwWorkStack
      ReturnedColumn* parm = buildReturnedColumn(item->arguments()[i], gwi, nonSupport);

      if (parm)
      {
        sptp.reset(new ParseTree(parm));

        // We need to pop whichever stack is holding it, if any.
        if ((!gwi.rcWorkStack.empty()) && *gwi.rcWorkStack.top() == parm)
        {
          delete gwi.rcWorkStack.top();
          gwi.rcWorkStack.pop();
        }
        else if (!gwi.ptWorkStack.empty())
        {
          ReturnedColumn* ptrc = dynamic_cast<ReturnedColumn*>(gwi.ptWorkStack.top()->data());

          if (ptrc && *ptrc == *parm)
          {
            delete gwi.ptWorkStack.top();
            gwi.ptWorkStack.pop();
          }
        }
      }
      else
      {
        sptp.reset(buildParseTree(item->arguments()[i], gwi, nonSupport));

        // We need to pop whichever stack is holding it, if any.
        if ((!gwi.ptWorkStack.empty()) && *gwi.ptWorkStack.top()->data() == sptp->data())
        {
          delete gwi.ptWorkStack.top();
          gwi.ptWorkStack.pop();
        }
        else if (!gwi.rcWorkStack.empty())
        {
          // Probably won't happen, but it might have been on the
          // rcWorkStack all along.
          ReturnedColumn* ptrc = dynamic_cast<ReturnedColumn*>(sptp->data());

          if (ptrc && *ptrc == *gwi.rcWorkStack.top())
          {
            delete gwi.rcWorkStack.top();
            gwi.rcWorkStack.pop();
          }
        }
      }
    }

    funcParms.insert(funcParms.begin(), sptp);
  }

  // recover clause type
  gwi.clauseType = realClauseType;

  if (gwi.fatalParseError)
  {
    setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
    return NULL;
  }

  Func* functor = funcexp->getFunctor(funcName);
  fc->resultType(colType_MysqlToIDB(item));
  execplan::CalpontSystemCatalog::ColType& resultType = fc->resultType();
  resultType.setTimeZone(gwi.timeZone);
  fc->operationType(functor->operationType(funcParms, resultType));
  fc->functionName(funcName);
  fc->functionParms(funcParms);
  fc->expressionId(ci->expressionId++);
  fc->timeZone(gwi.timeZone);

  // For function join. If any argument has non-zero joininfo, set it to the function.
  fc->setSimpleColumnList();
  std::vector<SimpleColumn*> simpleColList = fc->simpleColumnList();

  for (uint i = 0; i < simpleColList.size(); i++)
  {
    if (simpleColList[i]->joinInfo() != 0)
    {
      fc->joinInfo(simpleColList[i]->joinInfo());
      break;
    }
  }

  return fc;
}

ConstantColumn* buildDecimalColumn(const Item* idp, const std::string& valStr, gp_walk_info& gwi)
{
  IDB_Decimal columnstore_decimal;
  ostringstream columnstore_decimal_val;
  uint32_t i = 0;

  if (valStr[0] == '+' || valStr[0] == '-')
  {
    columnstore_decimal_val << valStr[0];
    i = 1;
  }

  bool specialPrecision = false;

  // handle the case when the constant value is 0.12345678901234567890123456789012345678
  // In this case idp->decimal_precision() = 39, but we can
  if (((i + 1) < valStr.length()) && valStr[i] == '0' && valStr[i + 1] == '.')
    specialPrecision = true;

  for (; i < valStr.length(); i++)
  {
    if (valStr[i] == '.')
      continue;

    columnstore_decimal_val << valStr[i];
  }

  if (idp->decimal_precision() <= datatypes::INT64MAXPRECISION)
    columnstore_decimal.value = strtoll(columnstore_decimal_val.str().c_str(), 0, 10);
  else if (idp->decimal_precision() <= datatypes::INT128MAXPRECISION ||
           (idp->decimal_precision() <= datatypes::INT128MAXPRECISION + 1 && specialPrecision))
  {
    bool dummy = false;
    columnstore_decimal.s128Value = dataconvert::strtoll128(columnstore_decimal_val.str().c_str(), dummy, 0);
  }

  // TODO MCOL-641 Add support here
  if (gwi.internalDecimalScale >= 0 && idp->decimals > (uint)gwi.internalDecimalScale)
  {
    columnstore_decimal.scale = gwi.internalDecimalScale;
    uint32_t diff = (uint32_t)(idp->decimals - gwi.internalDecimalScale);
    columnstore_decimal.value = columnstore_decimal.TDecimal64::toSInt64Round(diff);
  }
  else
    columnstore_decimal.scale = idp->decimal_scale();

  columnstore_decimal.precision = (idp->decimal_precision() > datatypes::INT128MAXPRECISION)
                                      ? datatypes::INT128MAXPRECISION
                                      : idp->decimal_precision();
  ConstantColumn* cc = new ConstantColumn(valStr, columnstore_decimal);
  cc->charsetNumber(idp->collation.collation->number);
  return cc;
}

SimpleColumn* buildSimpleColumn(Item_field* ifp, gp_walk_info& gwi)
{
  if (!gwi.csc)
  {
    gwi.csc = CalpontSystemCatalog::makeCalpontSystemCatalog(gwi.sessionid);
    gwi.csc->identity(CalpontSystemCatalog::FE);
  }

  bool isInformationSchema = false;

  // @bug5523
  if (ifp->cached_table && ifp->cached_table->db.length > 0 &&
      strcmp(ifp->cached_table->db.str, "information_schema") == 0)
    isInformationSchema = true;

  // support FRPM subquery. columns from the derived table has no definition
  if ((!ifp->field || !ifp->db_name.str || strlen(ifp->db_name.str) == 0) && !isInformationSchema)
    return buildSimpleColFromDerivedTable(gwi, ifp);

  CalpontSystemCatalog::ColType ct;
  datatypes::SimpleColumnParam prm(gwi.sessionid, true);

  try
  {
    // check foreign engine
    if (ifp->cached_table && ifp->cached_table->table)
      prm.columnStore(ha_mcs_common::isMCSTable(ifp->cached_table->table));
    // @bug4509. ifp->cached_table could be null for myisam sometimes
    else if (ifp->field && ifp->field->table)
      prm.columnStore(ha_mcs_common::isMCSTable(ifp->field->table));

    if (prm.columnStore())
    {
      ct = gwi.csc->colType(
          gwi.csc->lookupOID(make_tcn(ifp->db_name.str, bestTableName(ifp), ifp->field_name.str)));
    }
    else
    {
      ct = colType_MysqlToIDB(ifp);
    }
  }
  catch (std::exception& ex)
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = ex.what();
    return NULL;
  }

  const datatypes::DatabaseQualifiedColumnName name(ifp->db_name.str, bestTableName(ifp),
                                                    ifp->field_name.str);
  const datatypes::TypeHandler* h = ct.typeHandler();
  SimpleColumn* sc = h->newSimpleColumn(name, ct, prm);

  sc->resultType(ct);
  sc->charsetNumber(ifp->collation.collation->number);
  string tbname(ifp->table_name.str);

  if (isInformationSchema)
  {
    sc->schemaName("information_schema");
    sc->tableName(tbname, lower_case_table_names);
  }

  sc->tableAlias(tbname, lower_case_table_names);

  // view name
  sc->viewName(getViewName(ifp->cached_table), lower_case_table_names);
  // sc->partitions(...); // XXX how???
  sc->alias(ifp->name.str);

  sc->isColumnStore(prm.columnStore());
  sc->timeZone(gwi.timeZone);

  if (!prm.columnStore() && ifp->field)
    sc->oid(ifp->field->field_index + 1);  // ExeMgr requires offset started from 1

  if (ifp->depended_from)
  {
    sc->joinInfo(sc->joinInfo() | JOIN_CORRELATED);

    if (gwi.subQuery)
      gwi.subQuery->correlated(true);

    // for error out non-support select filter case (comparison outside semi join tables)
    gwi.correlatedTbNameVec.push_back(make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias()));

    // imply semi for scalar for now.
    if (gwi.subSelectType == CalpontSelectExecutionPlan::SINGLEROW_SUBS)
      sc->joinInfo(sc->joinInfo() | JOIN_SCALAR | JOIN_SEMI);

    if (gwi.subSelectType == CalpontSelectExecutionPlan::SELECT_SUBS)
      sc->joinInfo(sc->joinInfo() | JOIN_SCALAR | JOIN_OUTER_SELECT);
  }

  if (ifp->cached_table)
  {
    sc->partitions(getPartitions(ifp->cached_table));
  }

  return sc;
}

ParseTree* buildParseTree(Item* item, gp_walk_info& gwi, bool& /*nonSupport*/)
{
  ParseTree* pt = 0;
#ifdef DEBUG_WALK_COND
  // debug
  cerr << "Build Parsetree: " << endl;
  item->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
#endif
  //@bug5044. PPSTFIX walking should always be treated as WHERE clause filter
  ClauseType clauseType = gwi.clauseType;
  gwi.clauseType = WHERE;
  item->traverse_cond(gp_walk, &gwi, Item::POSTFIX);
  gwi.clauseType = clauseType;

  if (gwi.fatalParseError)
    return NULL;

  // bug 2840. if the filter/function is constant, result is in rcWorkStack
  if (!gwi.ptWorkStack.empty())
  {
    pt = gwi.ptWorkStack.top();
    gwi.ptWorkStack.pop();
  }
  else if (!gwi.rcWorkStack.empty())
  {
    pt = new ParseTree(gwi.rcWorkStack.top());
    gwi.rcWorkStack.pop();
  }

  return pt;
}

class ConstArgParam
{
 public:
  unsigned int precision;
  unsigned int scale;
  bool bIsConst;
  bool hasDecimalConst;
  ConstArgParam() : precision(0), scale(0), bIsConst(false), hasDecimalConst(false)
  {
  }
};

static void processAggregateColumnConstArg(gp_walk_info& gwi, SRCP& parm, AggregateColumn* ac, Item* sfitemp,
                                           ConstArgParam& constParam)
{
  DBUG_ASSERT(sfitemp->const_item());
  switch (sfitemp->cmp_type())
  {
    case INT_RESULT:
    case STRING_RESULT:
    case REAL_RESULT:
    case DECIMAL_RESULT:
    {
      ReturnedColumn* rt = buildReturnedColumn(sfitemp, gwi, gwi.fatalParseError);
      if (!rt)
      {
        gwi.fatalParseError = true;
        return;
      }
      ConstantColumn* cc;
      if ((cc = dynamic_cast<ConstantColumn*>(rt)) && cc->isNull())
      {
        // Explicit NULL or a const function that evaluated to NULL
        cc = new ConstantColumnNull();
        cc->timeZone(gwi.timeZone);
        parm.reset(cc);
        ac->constCol(SRCP(rt));
        return;
      }

      // treat as count(*)
      if (ac->aggOp() == AggregateColumn::COUNT)
        ac->aggOp(AggregateColumn::COUNT_ASTERISK);

      parm.reset(rt);
      ac->constCol(parm);
      constParam.bIsConst = true;
      if (sfitemp->cmp_type() == DECIMAL_RESULT)
      {
        constParam.hasDecimalConst = true;
        constParam.precision = sfitemp->decimal_precision();
        constParam.scale = sfitemp->decimal_scale();
      }
      break;
    }
    case TIME_RESULT:
      // QQ: why temporal constants are not handled?
    case ROW_RESULT:
    {
      gwi.fatalParseError = true;
    }
  }
}

void analyzeForImplicitGroupBy(Item* item, gp_walk_info& gwi)
{
  if (gwi.implicitExplicitGroupBy)
  {
    return;
  }
  while (item->type() == Item::REF_ITEM)
  {
    Item_ref* ref = static_cast<Item_ref*>(item);
    item = *ref->ref;
  }
  if (item->type() == Item::SUM_FUNC_ITEM)
  {
    // definitely an aggregate and thus needs an implicit group by.
    gwi.implicitExplicitGroupBy = true;
    return;
  }
  if (item->type() == Item::FUNC_ITEM)
  {
    Item_func* ifp = static_cast<Item_func*>(item);
    for (uint32_t i = 0; i < ifp->argument_count() && !gwi.implicitExplicitGroupBy; i++)
    {
      analyzeForImplicitGroupBy(ifp->arguments()[i], gwi);
    }
  }
}

ReturnedColumn* buildAggregateColumnBody(Item* item, gp_walk_info& gwi)
{
  // MCOL-1201 For UDAnF multiple parameters
  vector<SRCP> selCols;
  vector<SRCP> orderCols;
  ConstArgParam constArgParam;

  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = static_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  Item_sum* isp = static_cast<Item_sum*>(item);
  Item** sfitempp = isp->get_orig_args();
  SRCP parm;

  // @bug4756
  if (gwi.clauseType == SELECT)
    gwi.aggOnSelect = true;

  // Argument_count() is the # of formal parms to the agg fcn. Columnstore
  // only supports 1 argument except UDAnF, COUNT(DISTINC), GROUP_CONCAT and JSON_ARRAYAGG
  if (isp->argument_count() != 1 && isp->sum_func() != Item_sum::COUNT_DISTINCT_FUNC &&
      isp->sum_func() != Item_sum::GROUP_CONCAT_FUNC && isp->sum_func() != Item_sum::UDF_SUM_FUNC &&
      isp->sum_func() != Item_sum::JSON_ARRAYAGG_FUNC)
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_MUL_ARG_AGG);
    return NULL;
  }

  AggregateColumn* ac = NULL;

  if (isp->sum_func() == Item_sum::GROUP_CONCAT_FUNC)
  {
    ac = new GroupConcatColumn(gwi.sessionid);
  }
  else if (isp->sum_func() == Item_sum::JSON_ARRAYAGG_FUNC)
  {
    ac = new GroupConcatColumn(gwi.sessionid, true);
  }
  else if (isp->sum_func() == Item_sum::UDF_SUM_FUNC)
  {
    ac = new UDAFColumn(gwi.sessionid);
  }
  else
  {
    ac = new AggregateColumn(gwi.sessionid);
  }

  ac->timeZone(gwi.timeZone);

  if (isp->name.length)
    ac->alias(isp->name.str);

  if ((setAggOp(ac, isp)))
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = "Non supported aggregate type on the select clause";

    if (ac)
      delete ac;

    return NULL;
  }

  try
  {
    // special parsing for group_concat
    if (isp->sum_func() == Item_sum::GROUP_CONCAT_FUNC)
    {
      Item_func_group_concat* gc = (Item_func_group_concat*)isp;
      vector<SRCP> orderCols;
      RowColumn* rowCol = new RowColumn();
      vector<SRCP> selCols;

      uint32_t select_ctn = gc->get_count_field();
      ReturnedColumn* rc = NULL;

      for (uint32_t i = 0; i < select_ctn; i++)
      {
        rc = buildReturnedColumn(sfitempp[i], gwi, gwi.fatalParseError);

        if (!rc || gwi.fatalParseError)
        {
          if (ac)
            delete ac;

          return NULL;
        }

        selCols.push_back(SRCP(rc));
      }

      ORDER **order_item, **end;

      for (order_item = gc->get_order(), end = order_item + gc->get_order_field(); order_item < end;
           order_item++)
      {
        Item* ord_col = *(*order_item)->item;

        if (ord_col->type() == Item::CONST_ITEM && ord_col->cmp_type() == INT_RESULT)
        {
          Item_int* id = (Item_int*)ord_col;
          int64_t index = id->val_int();

          if (index > (int)selCols.size() || index < 1)
          {
            gwi.fatalParseError = true;

            if (ac)
              delete ac;

            return NULL;
          }

          rc = selCols[index - 1]->clone();
          rc->orderPos(index - 1);
        }
        else
        {
          rc = buildReturnedColumn(ord_col, gwi, gwi.fatalParseError);

          if (!rc || gwi.fatalParseError)
          {
            if (ac)
              delete ac;

            return NULL;
          }
        }

        // 10.2 TODO: direction is now a tri-state flag
        rc->asc((*order_item)->direction == ORDER::ORDER_ASC ? true : false);
        orderCols.push_back(SRCP(rc));
      }

      rowCol->columnVec(selCols);
      (dynamic_cast<GroupConcatColumn*>(ac))->orderCols(orderCols);
      parm.reset(rowCol);
      ac->aggParms().push_back(parm);

      if (gc->get_separator())
      {
        string separator;
        separator.assign(gc->get_separator()->ptr(), gc->get_separator()->length());
        (dynamic_cast<GroupConcatColumn*>(ac))->separator(separator);
      }
    }
    else if (isp->sum_func() == Item_sum::JSON_ARRAYAGG_FUNC)
    {
      Item_func_json_arrayagg* gc = (Item_func_json_arrayagg*)isp;
      vector<SRCP> orderCols;
      RowColumn* rowCol = new RowColumn();
      vector<SRCP> selCols;

      uint32_t select_ctn = gc->get_count_field();
      ReturnedColumn* rc = NULL;

      for (uint32_t i = 0; i < select_ctn; i++)
      {
        rc = buildReturnedColumn(sfitempp[i], gwi, gwi.fatalParseError);

        if (!rc || gwi.fatalParseError)
        {
          if (ac)
            delete ac;

          return NULL;
        }

        selCols.push_back(SRCP(rc));
      }

      ORDER **order_item, **end;

      for (order_item = gc->get_order(), end = order_item + gc->get_order_field(); order_item < end;
           order_item++)
      {
        Item* ord_col = *(*order_item)->item;

        if (ord_col->type() == Item::CONST_ITEM && ord_col->cmp_type() == INT_RESULT)
        {
          Item_int* id = (Item_int*)ord_col;

          if (id->val_int() > (int)selCols.size())
          {
            gwi.fatalParseError = true;

            if (ac)
              delete ac;

            return NULL;
          }

          rc = selCols[id->val_int() - 1]->clone();
          rc->orderPos(id->val_int() - 1);
        }
        else
        {
          rc = buildReturnedColumn(ord_col, gwi, gwi.fatalParseError);

          if (!rc || gwi.fatalParseError)
          {
            if (ac)
              delete ac;

            return NULL;
          }
        }

        // 10.2 TODO: direction is now a tri-state flag
        rc->asc((*order_item)->direction == ORDER::ORDER_ASC ? true : false);
        orderCols.push_back(SRCP(rc));
      }

      rowCol->columnVec(selCols);
      (dynamic_cast<GroupConcatColumn*>(ac))->orderCols(orderCols);
      parm.reset(rowCol);
      ac->aggParms().push_back(parm);

      if (gc->get_separator())
      {
        string separator;
        separator.assign(gc->get_separator()->ptr(), gc->get_separator()->length());
        (dynamic_cast<GroupConcatColumn*>(ac))->separator(separator);
      }
    }
    else if (isSupportedAggregateWithOneConstArg(isp, sfitempp))
    {
      processAggregateColumnConstArg(gwi, parm, ac, sfitempp[0], constArgParam);
    }
    else
    {
      for (uint32_t i = 0; i < isp->argument_count(); i++)
      {
        Item* sfitemp = sfitempp[i];
        Item::Type sfitype = sfitemp->type();

        switch (sfitype)
        {
          case Item::FIELD_ITEM:
          {
            Item_field* ifp = static_cast<Item_field*>(sfitemp);
            SimpleColumn* sc = buildSimpleColumn(ifp, gwi);

            if (!sc)
            {
              gwi.fatalParseError = true;
              break;
            }

            parm.reset(sc);
            gwi.columnMap.insert(
                CalpontSelectExecutionPlan::ColumnMap::value_type(string(ifp->field_name.str), parm));
            TABLE_LIST* tmp = (ifp->cached_table ? ifp->cached_table : 0);
            gwi.tableMap[make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias(),
                                         sc->isColumnStore())] = make_pair(1, tmp);
            break;
          }

          case Item::CONST_ITEM:
          case Item::NULL_ITEM:
          {
            processAggregateColumnConstArg(gwi, parm, ac, sfitemp, constArgParam);
            break;
          }

          case Item::FUNC_ITEM:
          {
            Item_func* ifp = (Item_func*)sfitemp;
            ReturnedColumn* rc = 0;

            // check count(1+1) case
            vector<Item_field*> tmpVec;
            uint16_t parseInfo = 0;
            parse_item(ifp, tmpVec, gwi.fatalParseError, parseInfo, &gwi);

            if (parseInfo & SUB_BIT)
            {
              gwi.fatalParseError = true;
              break;
            }
            else if (!gwi.fatalParseError && !(parseInfo & AGG_BIT) && !(parseInfo & AF_BIT) &&
                     tmpVec.size() == 0)
            {
              rc = buildFunctionColumn(ifp, gwi, gwi.fatalParseError);
              FunctionColumn* fc = dynamic_cast<FunctionColumn*>(rc);

              if ((fc && fc->functionParms().empty()) || !fc)
              {
                ReturnedColumn* rc = buildReturnedColumn(sfitemp, gwi, gwi.fatalParseError);

                if (dynamic_cast<ConstantColumn*>(rc))
                {
                  //@bug5229. handle constant function on aggregate argument
                  ac->constCol(SRCP(rc));
                  // XXX: this skips restoration of clauseType.
                  break;
                }
                // the "rc" can be in gwi.no_parm_func_list. erase it from that list and
                // then delete it.
                // kludge, I know.
                uint32_t i;

                for (i = 0; gwi.no_parm_func_list[i] != rc && i < gwi.no_parm_func_list.size(); i++)
                {
                }

                if (i < gwi.no_parm_func_list.size())
                {
                  gwi.no_parm_func_list.erase(gwi.no_parm_func_list.begin() + i);
                  delete rc;
                }
              }
            }

            // MySQL carelessly allows correlated aggregate function on the WHERE clause.
            // Here is the work around to deal with that inconsistence.
            // e.g., SELECT (SELECT t.c FROM t1 AS t WHERE t.b=MAX(t1.b + 0)) FROM t1;
            ClauseType clauseType = gwi.clauseType;

            if (gwi.clauseType == WHERE)
              gwi.clauseType = HAVING;

            // @bug 3603. for cases like max(rand()). try to build function first.
            if (!rc)
            {
              rc = buildFunctionColumn(ifp, gwi, gwi.fatalParseError);
            }
            if (!rc)
            {
              gwi.fatalParseError = true;
            }
            parm.reset(rc);
            gwi.clauseType = clauseType;
            break;
          }
          case Item::REF_ITEM:
          {
            ReturnedColumn* rc = buildReturnedColumn(sfitemp, gwi, gwi.fatalParseError);

            if (rc)
            {
              parm.reset(rc);
              break;
            }
          }
            /* fall through */

          default:
          {
            gwi.fatalParseError = true;
          }
        }

        if (gwi.fatalParseError)
        {
          if (gwi.parseErrorText.empty())
          {
            Message::Args args;

            if (item->name.length)
              args.add(item->name.str);
            else
              args.add("");

            gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_AGG_ARGS, args);
          }

          if (ac)
            delete ac;

          return NULL;
        }

        if (parm)
        {
          // MCOL-1201 multi-argument aggregate
          ac->aggParms().push_back(parm);
        }
      }
    }

    bool isAvg = (isp->sum_func() == Item_sum::AVG_FUNC || isp->sum_func() == Item_sum::AVG_DISTINCT_FUNC);

    // Get result type
    // Modified for MCOL-1201 multi-argument aggregate
    if (!constArgParam.bIsConst && ac->aggParms().size() > 0)
    {
      // These are all one parm functions, so we can safely
      // use the first parm for result type.
      parm = ac->aggParms()[0];

      if (isAvg || isp->sum_func() == Item_sum::SUM_FUNC || isp->sum_func() == Item_sum::SUM_DISTINCT_FUNC)
      {
        CalpontSystemCatalog::ColType ct = parm->resultType();
        if (ct.isWideDecimalType())
        {
          uint32_t precision = ct.precision;
          uint32_t scale = ct.scale;
          if (isAvg)
          {
            datatypes::Decimal::setScalePrecision4Avg(precision, scale);
          }
          ct.precision = precision;
          ct.scale = scale;
        }
        else if (datatypes::hasUnderlyingWideDecimalForSumAndAvg(ct.colDataType))
        {
          uint32_t precision = datatypes::INT128MAXPRECISION;
          uint32_t scale = ct.scale;
          ct.colDataType = CalpontSystemCatalog::DECIMAL;
          ct.colWidth = datatypes::MAXDECIMALWIDTH;
          if (isAvg)
          {
            datatypes::Decimal::setScalePrecision4Avg(precision, scale);
          }
          ct.scale = scale;
          ct.precision = precision;
        }
        else
        {
          ct.colDataType = CalpontSystemCatalog::LONGDOUBLE;
          ct.colWidth = sizeof(long double);
          if (isAvg)
          {
            ct.scale += datatypes::MAXSCALEINC4AVG;
          }
          ct.precision = datatypes::INT64MAXPRECISION;
        }
        ac->resultType(ct);
      }
      else if (isp->sum_func() == Item_sum::COUNT_FUNC || isp->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
      {
        CalpontSystemCatalog::ColType ct;
        ct.colDataType = CalpontSystemCatalog::BIGINT;
        ct.colWidth = 8;
        ct.scale = 0;
        ac->resultType(ct);
      }
      else if (isp->sum_func() == Item_sum::STD_FUNC || isp->sum_func() == Item_sum::VARIANCE_FUNC)
      {
        CalpontSystemCatalog::ColType ct;
        ct.colDataType = CalpontSystemCatalog::DOUBLE;
        ct.colWidth = 8;
        ct.scale = 0;
        ac->resultType(ct);
      }
      else if (isp->sum_func() == Item_sum::SUM_BIT_FUNC)
      {
        CalpontSystemCatalog::ColType ct;
        ct.colDataType = CalpontSystemCatalog::UBIGINT;
        ct.colWidth = 8;
        ct.scale = 0;
        ct.precision = -16;  // borrowed to indicate skip null value check on connector
        ac->resultType(ct);
      }
      else if (isp->sum_func() == Item_sum::GROUP_CONCAT_FUNC ||
               isp->sum_func() == Item_sum::JSON_ARRAYAGG_FUNC)
      {
        // Item_func_group_concat* gc = (Item_func_group_concat*)isp;
        CalpontSystemCatalog::ColType ct;
        ct.colDataType = CalpontSystemCatalog::VARCHAR;

        // MCOL-5429 CalpontSystemCatalog::ColType::colWidth is currently
        // stored as an int32_t (see calpontsystemcatalog.h). However,
        // Item_sum::max_length is an uint32_t. This means there will be an
        // integer overflow when Item_sum::max_length > colWidth. This ultimately
        // causes an array index out of bound in GroupConcator::swapStreamWithStringAndReturnBuf()
        // in groupconcat.cpp when ExeMgr processes groupconcat. As a temporary
        // fix, we cap off the max groupconcat length to std::numeric_limits<int32_t>::max().
        // The proper fix would be to change colWidth type to uint32_t.
        if (isp->max_length <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        {
          ct.colWidth = isp->max_length;
        }
        else
        {
          ct.colWidth = std::numeric_limits<int32_t>::max();
        }

        ct.precision = 0;
        ac->resultType(ct);
      }
      // Setting the ColType in the resulting RowGroup
      // according with what MDB expects.
      // Internal processing UDAF result type will be set below.
      else if (isUDFSumItem(isp))
      {
        ac->resultType(colType_MysqlToIDB(isp));
      }
      // Using the first param to deduce ac data type
      else if (ac->aggParms().size() == 1)
      {
        ac->resultType(parm->resultType());
      }
      else
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText =
            "Can not deduce Aggregate Column resulting type \
because it has multiple arguments.";

        if (ac)
          delete ac;

        return nullptr;
      }
    }
    else if (constArgParam.bIsConst && constArgParam.hasDecimalConst && isAvg)
    {
      CalpontSystemCatalog::ColType ct = parm->resultType();
      if (datatypes::Decimal::isWideDecimalTypeByPrecision(constArgParam.precision))
      {
        ct.precision = constArgParam.precision;
        ct.scale = constArgParam.scale;
        ct.colWidth = datatypes::MAXDECIMALWIDTH;
      }
      ac->resultType(ct);
    }
    else
    {
      ac->resultType(colType_MysqlToIDB(isp));
    }

    // adjust decimal result type according to internalDecimalScale
    bool isWideDecimal = ac->resultType().isWideDecimalType();
    if (!isWideDecimal && gwi.internalDecimalScale >= 0 &&
        (ac->resultType().colDataType == CalpontSystemCatalog::DECIMAL ||
         ac->resultType().colDataType == CalpontSystemCatalog::UDECIMAL))
    {
      CalpontSystemCatalog::ColType ct = ac->resultType();
      ct.scale = gwi.internalDecimalScale;
      ac->resultType(ct);
    }

    // check for same aggregate on the select list
    ac->expressionId(ci->expressionId++);

    if (gwi.clauseType != SELECT)
    {
      for (uint32_t i = 0; i < gwi.returnedCols.size(); i++)
      {
        if (*ac == gwi.returnedCols[i].get() && ac->alias() == gwi.returnedCols[i].get()->alias())
          ac->expressionId(gwi.returnedCols[i]->expressionId());
      }
    }

    // @bug5977 @note Temporary fix to avoid mysqld crash. The permanent fix will
    // be applied in ExeMgr. When the ExeMgr fix is available, this checking
    // will be taken out.
    if (isp->sum_func() != Item_sum::UDF_SUM_FUNC)
    {
      if (ac->constCol() && gwi.tbList.empty() && gwi.derivedTbList.empty())
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = "No project column found for aggregate function";

        if (ac)
          delete ac;

        return NULL;
      }
      else if (ac->constCol())
      {
        gwi.count_asterisk_list.push_back(ac);
      }
    }

    // For UDAF, populate the context and call the UDAF init() function.
    // The return type is (should be) set in context by init().
    if (isp->sum_func() == Item_sum::UDF_SUM_FUNC)
    {
      UDAFColumn* udafc = dynamic_cast<UDAFColumn*>(ac);

      if (udafc)
      {
        mcsv1sdk::mcsv1Context& context = udafc->getContext();
        context.setName(isp->func_name());

        // Get the return type as defined by CREATE AGGREGATE FUNCTION
        // Most functions don't care, but some may.
        context.setMariaDBReturnType((mcsv1sdk::enum_mariadb_return_type)isp->field_type());

        // Set up the return type defaults for the call to init()
        context.setResultType(udafc->resultType().colDataType);
        context.setColWidth(udafc->resultType().colWidth);
        context.setScale(udafc->resultType().scale);
        context.setPrecision(udafc->resultType().precision);

        context.setParamCount(udafc->aggParms().size());
        utils::VLArray<mcsv1sdk::ColumnDatum> colTypes(udafc->aggParms().size());

        // Build the column type vector.
        // Modified for MCOL-1201 multi-argument aggregate
        for (uint32_t i = 0; i < udafc->aggParms().size(); ++i)
        {
          const execplan::CalpontSystemCatalog::ColType& resultType = udafc->aggParms()[i]->resultType();
          mcsv1sdk::ColumnDatum& colType = colTypes[i];
          colType.dataType = resultType.colDataType;
          colType.precision = resultType.precision;
          colType.scale = resultType.scale;
          colType.charsetNumber = resultType.charsetNumber;
        }

        // Call the user supplied init()
        mcsv1sdk::mcsv1_UDAF* udaf = context.getFunction();

        if (!udaf)
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText =
              "Aggregate Function " + context.getName() + " doesn't exist in the ColumnStore engine";

          if (ac)
            delete ac;

          return NULL;
        }

        if (udaf->init(&context, colTypes) == mcsv1sdk::mcsv1_UDAF::ERROR)
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText = udafc->getContext().getErrorMessage();

          if (ac)
            delete ac;

          return NULL;
        }

        // UDAF_OVER_REQUIRED means that this function is for Window
        // Function only. Reject it here in aggregate land.
        if (udafc->getContext().getRunFlag(mcsv1sdk::UDAF_OVER_REQUIRED))
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText =
              logging::IDBErrorInfo::instance()->errorMsg(logging::ERR_WINDOW_FUNC_ONLY, context.getName());

          if (ac)
            delete ac;

          return NULL;
        }

        // Set the return type as set in init()
        CalpontSystemCatalog::ColType ct;
        ct.colDataType = context.getResultType();
        ct.colWidth = context.getColWidth();
        ct.scale = context.getScale();
        ct.precision = context.getPrecision();
        udafc->resultType(ct);
      }
    }
  }
  catch (std::logic_error& e)
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = "error building Aggregate Function: ";
    gwi.parseErrorText += e.what();

    if (ac)
      delete ac;

    return NULL;
  }
  catch (...)
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = "error building Aggregate Function: Unspecified exception";

    if (ac)
      delete ac;

    return NULL;
  }

  ac->charsetNumber(item->collation.collation->number);
  return ac;
}
ReturnedColumn* buildAggregateColumn(Item* item, gp_walk_info& gwi)
{
  bool disableWrapping = gwi.disableWrapping;
  gwi.disableWrapping = true;
  ReturnedColumn* rc = buildAggregateColumnBody(item, gwi);
  gwi.disableWrapping = disableWrapping;
  return rc;
}

void addIntervalArgs(gp_walk_info* gwip, Item_func* ifp, FunctionParm& functionParms)
{
  string funcName = ifp->func_name();
  int interval_type = -1;

  if (funcName == "date_add_interval")
    interval_type = ((Item_date_add_interval*)ifp)->int_type;
  else if (funcName == "timestampdiff")
    interval_type = ((Item_func_timestamp_diff*)ifp)->get_int_type();
  else if (funcName == "extract")
    interval_type = ((Item_extract*)ifp)->int_type;

  functionParms.push_back(getIntervalType(gwip, interval_type));
  SPTP sptp;

  if (funcName == "date_add_interval")
  {
    if (((Item_date_add_interval*)ifp)->date_sub_interval)
    {
      sptp.reset(new ParseTree(new ConstantColumn((int64_t)OP_SUB)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);
      functionParms.push_back(sptp);
    }
    else
    {
      sptp.reset(new ParseTree(new ConstantColumn((int64_t)OP_ADD)));
      (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);
      functionParms.push_back(sptp);
    }
  }
}

SPTP getIntervalType(gp_walk_info* gwip, int interval_type)
{
  SPTP sptp;
  sptp.reset(new ParseTree(new ConstantColumn((int64_t)interval_type)));
  (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);
  return sptp;
}

void castCharArgs(gp_walk_info* gwip, Item_func* ifp, FunctionParm& functionParms)
{
  Item_char_typecast* idai = (Item_char_typecast*)ifp;

  SPTP sptp;
  sptp.reset(new ParseTree(new ConstantColumn((int64_t)idai->get_cast_length())));
  (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);
  functionParms.push_back(sptp);
}

void castDecimalArgs(gp_walk_info* gwip, Item_func* ifp, FunctionParm& functionParms)
{
  Item_decimal_typecast* idai = (Item_decimal_typecast*)ifp;
  SPTP sptp;
  sptp.reset(new ParseTree(new ConstantColumn((int64_t)idai->decimals)));
  (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);
  functionParms.push_back(sptp);

  // max length including sign and/or decimal points
  if (idai->decimals == 0)
    sptp.reset(new ParseTree(new ConstantColumn((int64_t)idai->max_length - 1)));
  else
    sptp.reset(new ParseTree(new ConstantColumn((int64_t)idai->max_length - 2)));
  (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);

  functionParms.push_back(sptp);
}

void castTypeArgs(gp_walk_info* gwip, Item_func* ifp, FunctionParm& functionParms)
{
  Item_func_get_format* get_format = (Item_func_get_format*)ifp;
  SPTP sptp;

  if (get_format->type == MYSQL_TIMESTAMP_DATE)
    sptp.reset(new ParseTree(new ConstantColumn("DATE")));
  else
    sptp.reset(new ParseTree(new ConstantColumn("DATETIME")));
  (dynamic_cast<ConstantColumn*>(sptp->data()))->timeZone(gwip->timeZone);

  functionParms.push_back(sptp);
}

/*@brief  set some runtime params to run the query         */
/***********************************************************
 * DESCRIPTION:
 * This function just sets a number of runtime params that
 * limits resource consumed.
 ***********************************************************/
void setExecutionParams(gp_walk_info& gwi, SCSEP& csep)
{
  gwi.internalDecimalScale = (get_use_decimal_scale(gwi.thd) ? get_decimal_scale(gwi.thd) : -1);
  // @bug 2123. Override large table estimate if infinidb_ordered hint was used.
  // @bug 2404. Always override if the infinidb_ordered_only variable is turned on.
  if (get_ordered_only(gwi.thd))
    csep->overrideLargeSideEstimate(true);

  // @bug 5741. Set a flag when in Local PM only query mode
  csep->localQuery(get_local_query(gwi.thd));

  // @bug 3321. Set max number of blocks in a dictionary file to be scanned for filtering
  csep->stringScanThreshold(get_string_scan_threshold(gwi.thd));

  csep->stringTableThreshold(get_stringtable_threshold(gwi.thd));

  csep->djsSmallSideLimit(get_diskjoin_smallsidelimit(gwi.thd) * 1024ULL * 1024);
  csep->djsLargeSideLimit(get_diskjoin_largesidelimit(gwi.thd) * 1024ULL * 1024);
  csep->djsPartitionSize(get_diskjoin_bucketsize(gwi.thd) * 1024ULL * 1024);
  csep->djsMaxPartitionTreeDepth(get_diskjoin_max_partition_tree_depth(gwi.thd));
  csep->djsForceRun(get_diskjoin_force_run(gwi.thd));
  csep->maxPmJoinResultCount(get_max_pm_join_result_count(gwi.thd));
  if (get_um_mem_limit(gwi.thd) == 0)
    csep->umMemLimit(numeric_limits<int64_t>::max());
  else
    csep->umMemLimit(get_um_mem_limit(gwi.thd) * 1024ULL * 1024);
}

/*@brief  Process FROM part of the query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  This function processes elements of List<TABLE_LIST> in
 *  FROM part of the query.
 *  isUnion tells that CS processes FROM taken from UNION UNIT.
 *  The notion is described in MDB code.
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processFrom(bool& isUnion, SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep,
                bool isSelectHandlerTop, bool isSelectLexUnit)
{
  // populate table map and trigger syscolumn cache for all the tables (@bug 1637).
  // all tables on FROM list must have at least one col in colmap
  TABLE_LIST* table_ptr = select_lex.get_table_list();

#ifdef DEBUG_WALK_COND
  List_iterator<TABLE_LIST> sj_list_it(select_lex.sj_nests);
  TABLE_LIST* sj_nest;

  while ((sj_nest = sj_list_it++))
  {
    cerr << sj_nest->db.str << "." << sj_nest->table_name.str << endl;
  }
#endif

  try
  {
    for (; table_ptr; table_ptr = table_ptr->next_local)
    {
      // Until we handle recursive cte:
      // Checking here ensures we catch all with clauses in the query.
      if (table_ptr->is_recursive_with_table())
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = "Recursive CTE";
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
        return ER_CHECK_NOT_IMPLEMENTED;
      }

      string viewName = getViewName(table_ptr);
      if (lower_case_table_names)
      {
        boost::algorithm::to_lower(viewName);
      }

      // @todo process from subquery
      if (table_ptr->derived)
      {
        SELECT_LEX* select_cursor = table_ptr->derived->first_select();
        FromSubQuery* fromSub = new FromSubQuery(gwi, select_cursor);
        string alias(table_ptr->alias.str);
        if (lower_case_table_names)
        {
          boost::algorithm::to_lower(alias);
        }
        fromSub->alias(alias);

        CalpontSystemCatalog::TableAliasName tn = make_aliasview("", "", alias, viewName);
        // @bug 3852. check return execplan
        SCSEP plan = fromSub->transform();

        if (!plan)
        {
          setError(gwi.thd, ER_INTERNAL_ERROR, fromSub->gwip().parseErrorText, gwi);
          CalpontSystemCatalog::removeCalpontSystemCatalog(gwi.sessionid);
          return ER_INTERNAL_ERROR;
        }

        gwi.derivedTbList.push_back(plan);
        gwi.tbList.push_back(tn);
        CalpontSystemCatalog::TableAliasName tan = make_aliastable("", alias, alias);
        gwi.tableMap[tan] = make_pair(0, table_ptr);
      }
      else if (table_ptr->view)
      {
        View* view = new View(*table_ptr->view->first_select_lex(), &gwi);
        CalpontSystemCatalog::TableAliasName tn = make_aliastable(
            table_ptr->db.str, table_ptr->table_name.str, table_ptr->alias.str, true, lower_case_table_names);
        view->viewName(tn);
        gwi.viewList.push_back(view);
        view->transform();
      }
      else
      {
        // check foreign engine tables
        bool columnStore = (table_ptr->table ? ha_mcs_common::isMCSTable(table_ptr->table) : true);

        // trigger system catalog cache
        if (columnStore)
        {
          gwi.csc->columnRIDs(
              make_table(table_ptr->db.str, table_ptr->table_name.str, lower_case_table_names), true);
        }
        else
        {
          for (uint j = 0; j < table_ptr->table->s->keys; j++)
          {
            // for (uint i = 0; i < table_ptr->table->s->key_info[j].usable_key_parts; i++)
            {
              Field* field = table_ptr->table->key_info[j].key_part[0].field;
              std::cout << "j index " << j << " i column " << 0 << " fieldnr "
                        << table_ptr->table->key_info[j].key_part[0].fieldnr << " " << field->field_name.str;
              if (field->read_stats)
              {
                auto* histogram = dynamic_cast<Histogram_json_hb*>(field->read_stats->histogram);
                if (histogram)
                {
                  std::cout << " has stats ";
                  SchemaAndTableName tableName = {field->table->s->db.str,
                    field->table->s->table_name.str};
                  execplan::SimpleColumn simpleColumn = {field->table->s->db.str,
                    field->table->s->table_name.str,
                    field->field_name.str};
                  auto tableStatisticsMapIt = gwi.tableStatisticsMap.find(tableName);
                  if (tableStatisticsMapIt == gwi.tableStatisticsMap.end())
                  {
                    gwi.tableStatisticsMap[tableName][field->field_name.str] = {simpleColumn, {*histogram}};
                  }
                  else
                  {
                    auto columnStatisticsMapIt = tableStatisticsMapIt->second.find(field->field_name.str);
                    if (columnStatisticsMapIt == tableStatisticsMapIt->second.end())
                    {
                      tableStatisticsMapIt->second[field->field_name.str] = {simpleColumn, {*histogram}};
                    }
                    else
                    {
                      auto columnStatisticsVec = columnStatisticsMapIt->second.second;
                      columnStatisticsVec.push_back(*histogram);
                    }
                  }
                }
                else
                {
                  std::cout << " no stats ";
                }
              }
              std::cout << std::endl;
            }
          }
        }
        string table_name = table_ptr->table_name.str;

        // @bug5523
        if (table_ptr->db.length && strcmp(table_ptr->db.str, "information_schema") == 0)
          table_name =
              (table_ptr->schema_table_name.length ? table_ptr->schema_table_name.str : table_ptr->alias.str);

        CalpontSystemCatalog::TableAliasName tn =
            make_aliasview(table_ptr->db.str, table_name, table_ptr->alias.str, viewName, columnStore,
                           lower_case_table_names);
        execplan::Partitions parts = getPartitions(table_ptr);
        tn.partitions = parts;
        gwi.tbList.push_back(tn);
        CalpontSystemCatalog::TableAliasName tan = make_aliastable(
            table_ptr->db.str, table_name, table_ptr->alias.str, columnStore, lower_case_table_names);
        tan.partitions = parts;
        gwi.tableMap[tan] = make_pair(0, table_ptr);
#ifdef DEBUG_WALK_COND
        cerr << tn << endl;
#endif
      }
    }

    if (gwi.fatalParseError)
    {
      setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
      return ER_INTERNAL_ERROR;
    }
  }
  catch (IDBExcept& ie)
  {
    setError(gwi.thd, ER_INTERNAL_ERROR, ie.what(), gwi);
    CalpontSystemCatalog::removeCalpontSystemCatalog(gwi.sessionid);
    // @bug 3852. set error status for gwi.
    gwi.fatalParseError = true;
    gwi.parseErrorText = ie.what();
    return ER_INTERNAL_ERROR;
  }
  catch (...)
  {
    string emsg = IDBErrorInfo::instance()->errorMsg(ERR_LOST_CONN_EXEMGR);
    // @bug3852 set error status for gwi.
    gwi.fatalParseError = true;
    gwi.parseErrorText = emsg;
    setError(gwi.thd, ER_INTERNAL_ERROR, emsg, gwi);
    CalpontSystemCatalog::removeCalpontSystemCatalog(gwi.sessionid);
    return ER_INTERNAL_ERROR;
  }

  csep->tableList(gwi.tbList);

  // Send this recursively to getSelectPlan
  bool unionSel = false;
  // UNION master unit check
  // Existed pushdown handlers won't get in this scope
  // MDEV-25080 Union pushdown would enter this scope
  // is_unit_op() give a segv for derived_handler's SELECT_LEX

  // check INTERSECT or EXCEPT, that are not implemented
  if (select_lex.master_unit() && select_lex.master_unit()->first_select())
  {
    for (auto nextSelect = select_lex.master_unit()->first_select()->next_select(); nextSelect;
         nextSelect = nextSelect->next_select())
    {
      if (nextSelect->get_linkage() == INTERSECT_TYPE)
      {
        setError(gwi.thd, ER_INTERNAL_ERROR, "INTERSECT is not supported by Columnstore engine", gwi);
        return ER_INTERNAL_ERROR;
      }
      else if (nextSelect->get_linkage() == EXCEPT_TYPE)
      {
        setError(gwi.thd, ER_INTERNAL_ERROR, "EXCEPT is not supported by Columnstore engine", gwi);
        return ER_INTERNAL_ERROR;
      }
    }
  }

  if (!isUnion && (!isSelectHandlerTop || isSelectLexUnit) && select_lex.master_unit()->is_unit_op())
  {
    CalpontSelectExecutionPlan::SelectList unionVec;
    SELECT_LEX* select_cursor = select_lex.master_unit()->first_select();
    unionSel = true;
    uint8_t distUnionNum = 0;

    for (SELECT_LEX* sl = select_cursor; sl; sl = sl->next_select())
    {
      SCSEP plan(new CalpontSelectExecutionPlan());
      plan->txnID(csep->txnID());
      plan->verID(csep->verID());
      plan->sessionID(csep->sessionID());
      plan->traceFlags(csep->traceFlags());
      plan->data(csep->data());

      // gwi for the union unit
      gp_walk_info union_gwi(gwi.timeZone, gwi.subQueriesChain);
      union_gwi.thd = gwi.thd;
      uint32_t err = 0;

      if ((err = getSelectPlan(union_gwi, *sl, plan, unionSel)) != 0)
        return err;

      unionVec.push_back(SCEP(plan));

      // distinct union num
      if (sl == select_lex.master_unit()->union_distinct)
        distUnionNum = unionVec.size();
    }

    csep->unionVec(unionVec);
    csep->distinctUnionNum(distUnionNum);
  }

  return 0;
}

/*@brief Create in-to-exists predicate for an IN subquery   */
/***********************************************************
 * DESCRIPTION:
 * This function processes the lhs and rhs of an IN predicate
 * for a query such as:
 * select col1 from t1 where col2 in (select col2' from t2);
 * here, lhs is col2 and rhs is the in subquery "select col2' from t2".
 * It creates a new predicate of the form "col2=col2'" which then later
 * gets injected into the execution plan of the subquery.
 * If lhs is of type Item::ROW_ITEM instead, such as:
 * select col1 from t1 where (col2,col3) in (select col2',col3' from t2);
 * the function builds an "and" filter of the form "col2=col2' and col3=col3'".
 * RETURNS
 *  none
 ***********************************************************/
void buildInToExistsFilter(gp_walk_info& gwi, SELECT_LEX& select_lex)
{
  RowColumn* rlhs = dynamic_cast<RowColumn*>(gwi.inSubQueryLHS);

  size_t additionalRetColsBefore = gwi.additionalRetCols.size();

  if (rlhs)
  {
    idbassert(gwi.inSubQueryLHSItem->type() == Item::ROW_ITEM);

    Item_row* row = (Item_row*)gwi.inSubQueryLHSItem;

    idbassert(!rlhs->columnVec().empty() && (rlhs->columnVec().size() == gwi.returnedCols.size()) &&
              row->cols() && (row->cols() == select_lex.item_list.elements) &&
              (row->cols() == gwi.returnedCols.size()));

    List_iterator_fast<Item> it(select_lex.item_list);
    Item* item;

    int i = 0;

    ParseTree* rowFilter = nullptr;

    while ((item = it++))
    {
      boost::shared_ptr<Operator> sop(new PredicateOperator("="));
      vector<Item*> itemList = {row->element_index(i), item};
      ReturnedColumn* rhs = gwi.returnedCols[i]->clone();

      buildEqualityPredicate(rlhs->columnVec()[i]->clone(), rhs, &gwi, sop, Item_func::EQ_FUNC, itemList,
                             true);

      if (gwi.fatalParseError)
      {
        delete rlhs;
        return;
      }

      ParseTree* tmpFilter = nullptr;

      if (!gwi.ptWorkStack.empty())
      {
        tmpFilter = gwi.ptWorkStack.top();
        gwi.ptWorkStack.pop();
      }

      if (i == 0 && tmpFilter)
      {
        rowFilter = tmpFilter;
      }
      else if (i != 0 && tmpFilter && rowFilter)
      {
        ParseTree* ptp = new ParseTree(new LogicOperator("and"));
        ptp->left(rowFilter);
        ptp->right(tmpFilter);
        rowFilter = ptp;
      }

      i++;
    }

    delete rlhs;

    if (rowFilter)
      gwi.ptWorkStack.push(rowFilter);
  }
  else
  {
    idbassert((gwi.returnedCols.size() == 1) && (select_lex.item_list.elements == 1));

    boost::shared_ptr<Operator> sop(new PredicateOperator("="));
    vector<Item*> itemList = {gwi.inSubQueryLHSItem, select_lex.item_list.head()};
    ReturnedColumn* rhs = gwi.returnedCols[0]->clone();
    buildEqualityPredicate(gwi.inSubQueryLHS, rhs, &gwi, sop, Item_func::EQ_FUNC, itemList, true);

    if (gwi.fatalParseError)
      return;
  }

  size_t additionalRetColsAdded = gwi.additionalRetCols.size() - additionalRetColsBefore;

  if (gwi.returnedCols.size() && (gwi.returnedCols.size() == additionalRetColsAdded))
  {
    for (size_t i = 0; i < gwi.returnedCols.size(); i++)
    {
      gwi.returnedCols[i]->expressionId(gwi.additionalRetCols[additionalRetColsBefore + i]->expressionId());
      gwi.returnedCols[i]->colSource(gwi.additionalRetCols[additionalRetColsBefore + i]->colSource());
    }

    // Delete the duplicate copy of the returned cols
    auto iter = gwi.additionalRetCols.begin();
    std::advance(iter, additionalRetColsBefore);
    gwi.additionalRetCols.erase(iter, gwi.additionalRetCols.end());
  }
}

/*@brief  Process HAVING part of the query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  This function processes HAVING clause.
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processHaving(SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep,
                  std::unique_ptr<ParseTree>& havingFilter)
{
  gwi.havingDespiteSelect = true;
  clearStacks(gwi, false, true);

  // clear fatalParseError that may be left from post process functions
  gwi.fatalParseError = false;
  gwi.parseErrorText = "";

  gwi.disableWrapping = false;
  gwi.havingDespiteSelect = true;
  if (select_lex.having != 0)
  {
#ifdef DEBUG_WALK_COND
    cerr << "------------------- HAVING ---------------------" << endl;
    select_lex.having->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
    cerr << "------------------------------------------------\n" << endl;
#endif
    select_lex.having->traverse_cond(gp_walk, &gwi, Item::POSTFIX);

    if (gwi.fatalParseError)
    {
      setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
      return ER_INTERNAL_ERROR;
    }

    ParseTree* ptp = 0;
    ParseTree* rhs = 0;

    // @bug 4215. some function filter will be in the rcWorkStack.
    if (gwi.ptWorkStack.empty() && !gwi.rcWorkStack.empty())
    {
      gwi.ptWorkStack.push(new ParseTree(gwi.rcWorkStack.top()));
      gwi.rcWorkStack.pop();
    }

    while (!gwi.ptWorkStack.empty())
    {
      havingFilter.reset(gwi.ptWorkStack.top());
      gwi.ptWorkStack.pop();

      if (gwi.ptWorkStack.empty())
        break;

      ptp = new ParseTree(new LogicOperator("and"));
      ptp->left(havingFilter.release());
      rhs = gwi.ptWorkStack.top();
      gwi.ptWorkStack.pop();
      ptp->right(rhs);
      gwi.ptWorkStack.push(ptp);
    }
  }
  gwi.havingDespiteSelect = false;
  gwi.disableWrapping = false;

  // MCOL-4617 If this is an IN subquery, then create the in-to-exists
  // predicate and inject it into the csep
  if (gwi.subQuery && gwi.subSelectType == CalpontSelectExecutionPlan::IN_SUBS && gwi.inSubQueryLHS &&
      gwi.inSubQueryLHSItem)
  {
    // create the predicate
    buildInToExistsFilter(gwi, select_lex);

    if (gwi.fatalParseError)
    {
      setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
      return ER_INTERNAL_ERROR;
    }

    // now inject the created predicate
    if (!gwi.ptWorkStack.empty())
    {
      ParseTree* inToExistsFilter = gwi.ptWorkStack.top();
      gwi.ptWorkStack.pop();

      if (havingFilter)
      {
        ParseTree* ptp = new ParseTree(new LogicOperator("and"));
        ptp->left(havingFilter.release());
        ptp->right(inToExistsFilter);
        havingFilter.reset(ptp);
      }
      else
      {
        if (csep->filters())
        {
          ParseTree* ptp = new ParseTree(new LogicOperator("and"));
          ptp->left(csep->filters());
          ptp->right(inToExistsFilter);
          csep->filters(ptp);
        }
        else
        {
          csep->filters(inToExistsFilter);
        }
      }
    }
  }
  return 0;
}

/*@brief  Process GROUP BY part of the query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  This function processes GROUP BY clause.
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processGroupBy(SELECT_LEX& select_lex, gp_walk_info& gwi, const bool withRollup)
{
  Item* nonSupportItem = NULL;
  ORDER* groupcol = static_cast<ORDER*>(select_lex.group_list.first);

  // check if window functions are in order by. InfiniDB process order by list if
  // window functions are involved, either in order by or projection.
  bool hasWindowFunc = gwi.hasWindowFunc;
  gwi.hasWindowFunc = false;

  for (; groupcol; groupcol = groupcol->next)
  {
    if ((*(groupcol->item))->type() == Item::WINDOW_FUNC_ITEM)
      gwi.hasWindowFunc = true;
  }

  if (gwi.hasWindowFunc)
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_WF_NOT_ALLOWED, "GROUP BY clause");
    setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
    return ER_CHECK_NOT_IMPLEMENTED;
  }

  gwi.hasWindowFunc = hasWindowFunc;
  groupcol = static_cast<ORDER*>(select_lex.group_list.first);

  gwi.disableWrapping = true;
  for (; groupcol; groupcol = groupcol->next)
  {
    Item* groupItem = *(groupcol->item);

    // @bug5993. Could be nested ref.
    while (groupItem->type() == Item::REF_ITEM)
      groupItem = (*((Item_ref*)groupItem)->ref);

    if (groupItem->type() == Item::FUNC_ITEM)
    {
      Item_func* ifp = (Item_func*)groupItem;

      // call buildFunctionColumn here mostly for finding out
      // non-support column on GB list. Should be simplified.
      ReturnedColumn* fc = buildFunctionColumn(ifp, gwi, gwi.fatalParseError);

      if (!fc || gwi.fatalParseError)
      {
        nonSupportItem = ifp;
        break;
      }

      if (groupcol->in_field_list && groupcol->counter_used)
      {
        delete fc;
        fc = gwi.returnedCols[groupcol->counter - 1].get();
        SRCP srcp(fc->clone());

        // check if no column parm
        for (uint32_t i = 0; i < gwi.no_parm_func_list.size(); i++)
        {
          if (gwi.no_parm_func_list[i]->expressionId() == fc->expressionId())
          {
            gwi.no_parm_func_list.push_back(dynamic_cast<FunctionColumn*>(srcp.get()));
            break;
          }
        }

        srcp->orderPos(groupcol->counter - 1);
        gwi.groupByCols.push_back(srcp);
        continue;
      }
      else if (groupItem->is_explicit_name())  // alias
      {
        uint32_t i = 0;

        for (; i < gwi.returnedCols.size(); i++)
        {
          if (string(groupItem->name.str) == gwi.returnedCols[i]->alias())
          {
            ReturnedColumn* rc = gwi.returnedCols[i]->clone();
            rc->orderPos(i);
            gwi.groupByCols.push_back(SRCP(rc));
            delete fc;
            break;
          }
        }

        if (i == gwi.returnedCols.size())
        {
          nonSupportItem = groupItem;
          break;
        }
      }
      else
      {
        uint32_t i = 0;

        for (; i < gwi.returnedCols.size(); i++)
        {
          if (fc->operator==(gwi.returnedCols[i].get()))
          {
            ReturnedColumn* rc = gwi.returnedCols[i]->clone();
            rc->orderPos(i);
            gwi.groupByCols.push_back(SRCP(rc));
            delete fc;
            break;
          }
        }

        if (i == gwi.returnedCols.size())
        {
          gwi.groupByCols.push_back(SRCP(fc));
          break;
        }
      }
    }
    else if (groupItem->type() == Item::FIELD_ITEM)
    {
      Item_field* ifp = (Item_field*)groupItem;
      // this GB col could be an alias of F&E on the SELECT clause, not necessarily a field.
      ReturnedColumn* rc = buildSimpleColumn(ifp, gwi);
      SimpleColumn* sc = dynamic_cast<SimpleColumn*>(rc);

      if (sc)
      {
        bool found = false;
        for (uint32_t j = 0; j < gwi.returnedCols.size(); j++)
        {
          if (sc->sameColumn(gwi.returnedCols[j].get()))
          {
            sc->orderPos(j);
            found = true;
            break;
          }
        }
        for (uint32_t j = 0; !found && j < gwi.returnedCols.size(); j++)
        {
          if (strcasecmp(sc->alias().c_str(), gwi.returnedCols[j]->alias().c_str()) == 0)
          {
            delete rc;
            rc = gwi.returnedCols[j].get()->clone();
            rc->orderPos(j);
            break;
          }
        }
      }
      else
      {
        for (uint32_t j = 0; j < gwi.returnedCols.size(); j++)
        {
          if (ifp->name.length && string(ifp->name.str) == gwi.returnedCols[j].get()->alias())
          {
            delete rc;
            rc = gwi.returnedCols[j].get()->clone();
            rc->orderPos(j);
            break;
          }
        }
      }

      if (!rc)
      {
        nonSupportItem = ifp;
        break;
      }

      SRCP srcp(rc);

      // bug 3151
      AggregateColumn* ac = dynamic_cast<AggregateColumn*>(rc);

      if (ac)
      {
        nonSupportItem = ifp;
        break;
      }

      gwi.groupByCols.push_back(srcp);
      gwi.columnMap.insert(
          CalpontSelectExecutionPlan::ColumnMap::value_type(string(ifp->field_name.str), srcp));
    }
    // @bug5638. The group by column is constant but not counter, alias has to match a column
    // on the select list
    else if (!groupcol->counter_used &&
             (groupItem->type() == Item::CONST_ITEM &&
              (groupItem->cmp_type() == INT_RESULT || groupItem->cmp_type() == STRING_RESULT ||
               groupItem->cmp_type() == REAL_RESULT || groupItem->cmp_type() == DECIMAL_RESULT)))
    {
      ReturnedColumn* rc = 0;

      for (uint32_t j = 0; j < gwi.returnedCols.size(); j++)
      {
        if (groupItem->name.length && string(groupItem->name.str) == gwi.returnedCols[j].get()->alias())
        {
          rc = gwi.returnedCols[j].get()->clone();
          rc->orderPos(j);
          break;
        }
      }

      if (!rc)
      {
        nonSupportItem = groupItem;
        break;
      }

      gwi.groupByCols.push_back(SRCP(rc));
    }
    else if ((*(groupcol->item))->type() == Item::SUBSELECT_ITEM)
    {
      if (!groupcol->in_field_list || !groupItem->name.length)
      {
        nonSupportItem = groupItem;
      }
      else
      {
        uint32_t i = 0;

        for (; i < gwi.returnedCols.size(); i++)
        {
          if (string(groupItem->name.str) == gwi.returnedCols[i]->alias())
          {
            ReturnedColumn* rc = gwi.returnedCols[i]->clone();
            rc->orderPos(i);
            gwi.groupByCols.push_back(SRCP(rc));
            break;
          }
        }

        if (i == gwi.returnedCols.size())
        {
          nonSupportItem = groupItem;
        }
      }
    }
    // @bug 3761.
    else if (groupcol->counter_used)
    {
      if (gwi.returnedCols.size() <= (uint32_t)(groupcol->counter - 1))
      {
        nonSupportItem = groupItem;
      }
      else
      {
        gwi.groupByCols.push_back(SRCP(gwi.returnedCols[groupcol->counter - 1]->clone()));
      }
    }
    else
    {
      nonSupportItem = groupItem;
    }
  }
  gwi.disableWrapping = false;

  // @bug 4756. Add internal groupby column for correlated join to the groupby list
  if (gwi.aggOnSelect && !gwi.subGroupByCols.empty())
    gwi.groupByCols.insert(gwi.groupByCols.end(), gwi.subGroupByCols.begin(), gwi.subGroupByCols.end());

  // this is window func on SELECT becuase ORDER BY has not been processed
  if (!gwi.windowFuncList.empty() && !gwi.subGroupByCols.empty())
  {
    for (uint32_t i = 0; i < gwi.windowFuncList.size(); i++)
    {
      if (gwi.windowFuncList[i]->hasWindowFunc())
      {
        vector<WindowFunctionColumn*> windowFunctions = gwi.windowFuncList[i]->windowfunctionColumnList();

        for (uint32_t j = 0; j < windowFunctions.size(); j++)
          windowFunctions[j]->addToPartition(gwi.subGroupByCols);
      }
    }
  }

  if (nonSupportItem)
  {
    if (gwi.parseErrorText.length() == 0)
    {
      Message::Args args;
      if (nonSupportItem->name.length)
        args.add("'" + string(nonSupportItem->name.str) + "'");
      else
        args.add("");
      gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_GROUP_BY, args);
    }
    setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
    return ER_CHECK_NOT_IMPLEMENTED;
  }
  if (withRollup)
  {
    SRCP rc(new RollupMarkColumn());
    gwi.groupByCols.insert(gwi.groupByCols.end(), rc);
  }
  return 0;
}

/*@brief  Process WHERE part of the query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  This function processes conditions from either JOIN->conds
 *  or SELECT_LEX->where|prep_where
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processWhere(SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep, const std::vector<COND*>& condStack)
{
  JOIN* join = select_lex.join;
  Item* icp = 0;
  bool isUpdateDelete = false;

  // Flag to indicate if this is a prepared statement
  bool isPS = gwi.thd->stmt_arena && gwi.thd->stmt_arena->is_stmt_execute();

  if (join != 0 && !isPS)
    icp = join->conds;
  else if (isPS && select_lex.prep_where)
    icp = select_lex.prep_where;

  // if icp is null, try to find the where clause other where
  if (!join && gwi.thd->lex->derived_tables)
  {
    if (select_lex.prep_where)
      icp = select_lex.prep_where;
    else if (select_lex.where)
      icp = select_lex.where;
  }
  else if (!join && ha_mcs_common::isUpdateOrDeleteStatement(gwi.thd->lex->sql_command))
  {
    isUpdateDelete = true;
  }

  if (icp)
  {
    // MariaDB bug 624 - without the fix_fields call, delete with join may error with "No query step".
    //@bug 3039. fix fields for constants
    if (!icp->fixed())
    {
      icp->fix_fields(gwi.thd, (Item**)&icp);
    }

    gwi.fatalParseError = false;
#ifdef DEBUG_WALK_COND
    std::cerr << "------------------ WHERE -----------------------" << std::endl;
    icp->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
    std::cerr << "------------------------------------------------\n" << std::endl;
#endif

    icp->traverse_cond(gp_walk, &gwi, Item::POSTFIX);

    if (gwi.fatalParseError)
    {
      return setErrorAndReturn(gwi);
    }
  }
  else if (isUpdateDelete)
  {
    // MCOL-4023 For updates/deletes, we iterate over the pushed down condStack
    if (!condStack.empty())
    {
      std::vector<COND*>::const_iterator condStackIter = condStack.begin();

      while (condStackIter != condStack.end())
      {
        COND* cond = *condStackIter++;

        cond->traverse_cond(gp_walk, &gwi, Item::POSTFIX);

        if (gwi.fatalParseError)
        {
          return setErrorAndReturn(gwi);
        }
      }
    }
    // if condStack is empty(), check the select_lex for where conditions
    // as a last resort
    else if ((icp = select_lex.where) != 0)
    {
      icp->traverse_cond(gp_walk, &gwi, Item::POSTFIX);

      if (gwi.fatalParseError)
      {
        return setErrorAndReturn(gwi);
      }
    }
  }
  else if (join && join->zero_result_cause)
  {
    gwi.rcWorkStack.push(new ConstantColumn((int64_t)0, ConstantColumn::NUM));
    (dynamic_cast<ConstantColumn*>(gwi.rcWorkStack.top()))->timeZone(gwi.timeZone);
  }

  for (Item* item : gwi.condList)
  {
    if (item && (item != icp))
    {
      item->traverse_cond(gp_walk, &gwi, Item::POSTFIX);

      if (gwi.fatalParseError)
      {
        return setErrorAndReturn(gwi);
      }
    }
  }

  // ZZ - the followinig debug shows the structure of nested outer join. should
  // use a recursive function.
#ifdef OUTER_JOIN_DEBUG
  List<TABLE_LIST>* tables = &(select_lex.top_join_list);
  List_iterator_fast<TABLE_LIST> ti(*tables);

  TABLE_LIST** table = (TABLE_LIST**)gwi.thd->alloc(sizeof(TABLE_LIST*) * tables->elements);
  for (TABLE_LIST** t = table + (tables->elements - 1); t >= table; t--)
    *t = ti++;

  DBUG_ASSERT(tables->elements >= 1);

  TABLE_LIST** end = table + tables->elements;
  for (TABLE_LIST** tbl = table; tbl < end; tbl++)
  {
    TABLE_LIST* curr;

    while ((curr = ti++))
    {
      TABLE_LIST* curr = *tbl;

      if (curr->table_name.length)
        cerr << curr->table_name.str << " ";
      else
        cerr << curr->alias.str << endl;

      if (curr->outer_join)
        cerr << " is inner table" << endl;
      else if (curr->straight)
        cerr << "straight_join" << endl;
      else
        cerr << "join" << endl;

      if (curr->nested_join)
      {
        List<TABLE_LIST>* inners = &(curr->nested_join->join_list);
        List_iterator_fast<TABLE_LIST> li(*inners);
        TABLE_LIST** inner = (TABLE_LIST**)gwi.thd->alloc(sizeof(TABLE_LIST*) * inners->elements);

        for (TABLE_LIST** t = inner + (inners->elements - 1); t >= inner; t--)
          *t = li++;

        TABLE_LIST** end1 = inner + inners->elements;

        for (TABLE_LIST** tb = inner; tb < end1; tb++)
        {
          TABLE_LIST* curr1 = *tb;
          cerr << curr1->alias.str << endl;

          if (curr1->sj_on_expr)
          {
            curr1->sj_on_expr->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
          }
        }
      }

      if (curr->sj_on_expr)
      {
        curr->sj_on_expr->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
      }
    }
  }
#endif

  uint32_t failed = 0;

  // InfiniDB bug5764 requires outer joins to be appended to the
  // end of the filter list. This causes outer join filters to
  // have a higher join id than inner join filters.
  // TODO MCOL-4680 Figure out why this is the case, and possibly
  // eliminate this requirement.
  std::stack<execplan::ParseTree*> outerJoinStack;

  if ((failed = buildJoin(gwi, select_lex.top_join_list, outerJoinStack)))
  {
    while (!outerJoinStack.empty())
    {
      delete outerJoinStack.top();
      outerJoinStack.pop();
    }
    return failed;
  }

  if (gwi.subQuery)
  {
    for (uint i = 0; i < gwi.viewList.size(); i++)
    {
      if ((failed = gwi.viewList[i]->processJoin(gwi, outerJoinStack)))
      {
        while (!outerJoinStack.empty())
        {
          delete outerJoinStack.top();
          outerJoinStack.pop();
        }
        return failed;
      }
    }
  }

  ParseTree* filters = NULL;
  ParseTree* outerJoinFilters = NULL;
  ParseTree* ptp = NULL;
  ParseTree* rhs = NULL;

  // @bug 2932. for "select * from region where r_name" case. if icp not null and
  // ptWorkStack empty, the item is in rcWorkStack.
  // MySQL 5.6 (MariaDB?). when icp is null and zero_result_cause is set, a constant 0
  // is pushed to rcWorkStack.
  if (gwi.ptWorkStack.empty() && !gwi.rcWorkStack.empty())
  {
    gwi.ptWorkStack.push(new ParseTree(gwi.rcWorkStack.top()));
    gwi.rcWorkStack.pop();
  }

  while (!gwi.ptWorkStack.empty())
  {
    filters = gwi.ptWorkStack.top();
    gwi.ptWorkStack.pop();

    if (gwi.ptWorkStack.empty())
      break;

    ptp = new ParseTree(new LogicOperator("and"));
    ptp->left(filters);
    rhs = gwi.ptWorkStack.top();
    gwi.ptWorkStack.pop();
    ptp->right(rhs);
    gwi.ptWorkStack.push(ptp);
  }

  while (!outerJoinStack.empty())
  {
    outerJoinFilters = outerJoinStack.top();
    outerJoinStack.pop();

    if (outerJoinStack.empty())
      break;

    ptp = new ParseTree(new LogicOperator("and"));
    ptp->left(outerJoinFilters);
    rhs = outerJoinStack.top();
    outerJoinStack.pop();
    ptp->right(rhs);
    outerJoinStack.push(ptp);
  }

  config::Config* cf = config::Config::makeConfig();
  string rewriteEnabled = cf->getConfig("Rewrites", "CommonLeafConjunctionsToTop");
  if (filters && rewriteEnabled != "OFF")
  {
    filters = extractCommonLeafConjunctionsToRoot(filters);
  }

  uint64_t limit = get_max_allowed_in_values(gwi.thd);

  if (filters && !checkFiltersLimit(filters, limit))
  {
    gwi.fatalParseError = true;
    setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED,
             " in clauses of this length. Number of values in the IN clause exceeded "
             "columnstore_max_allowed_in_values "
             "threshold.",
             gwi);
    delete filters;
    return ER_CHECK_NOT_IMPLEMENTED;
  }

  // Append outer join filters at the end of inner join filters.
  // JLF_ExecPlanToJobList::walkTree processes ParseTree::left
  // before ParseTree::right which is what we intend to do in the
  // below.
  if (filters && outerJoinFilters)
  {
    ptp = new ParseTree(new LogicOperator("and"));
    ptp->left(filters);
    ptp->right(outerJoinFilters);
    filters = ptp;
  }
  else if (outerJoinFilters)
  {
    filters = outerJoinFilters;
  }

  if (filters)
  {
    csep->filters(filters);
  }

  if (!gwi.rcWorkStack.empty())
  {
    while (!gwi.rcWorkStack.empty())
    {
      ReturnedColumn* t = gwi.rcWorkStack.top();
      delete t;
      gwi.rcWorkStack.pop();
    }
  }
  if (!gwi.ptWorkStack.empty())
  {
    while (!gwi.ptWorkStack.empty())
    {
      ParseTree* t = gwi.ptWorkStack.top();
      delete t;
      gwi.ptWorkStack.pop();
    }
  }

  return 0;
}

/*@brief  Process LIMIT part of a query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  Processes LIMIT and OFFSET parts
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processLimitAndOffset(SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep, bool unionSel, bool isUnion,
                          bool /*isSelectHandlerTop*/)
{
  // LIMIT processing part
  uint64_t limitNum = std::numeric_limits<uint64_t>::max();

  // non-MAIN union branch
  if (unionSel || gwi.subSelectType != CalpontSelectExecutionPlan::MAIN_SELECT)
  {
    /* Consider the following query:
       "select a from t1 where exists (select b from t2 where a=b);"
       CS first builds a hash table for t2, then pushes down the hash to
       PrimProc for a distributed hash join execution, with t1 being the
       large-side table. However, the server applies an optimization in
       Item_exists_subselect::fix_length_and_dec in sql/item_subselect.cc
       (see server commit ae476868a5394041a00e75a29c7d45917e8dfae8)
       where it sets explicit_limit to true, which causes csep->limitNum set to 1.
       This causes the hash table for t2 to only contain a single record for the
       hash join, giving less number of rows in the output result set than expected.
       We therefore do not allow limit set to 1 here for such queries.
    */
    if (gwi.subSelectType != CalpontSelectExecutionPlan::IN_SUBS &&
        gwi.subSelectType != CalpontSelectExecutionPlan::EXISTS_SUBS &&
        select_lex.master_unit()->global_parameters()->limit_params.explicit_limit)
    {
      if (select_lex.master_unit()->global_parameters()->limit_params.offset_limit)
      {
        Item_int* offset =
            (Item_int*)select_lex.master_unit()->global_parameters()->limit_params.offset_limit;
        csep->limitStart(offset->val_int());
      }

      if (select_lex.master_unit()->global_parameters()->limit_params.select_limit)
      {
        Item_int* select =
            (Item_int*)select_lex.master_unit()->global_parameters()->limit_params.select_limit;
        csep->limitNum(select->val_int());
        // MCOL-894 Activate parallel ORDER BY
        csep->orderByThreads(get_orderby_threads(gwi.thd));
      }
    }
  }
  // union with explicit select at the top level
  else if (isUnion && select_lex.limit_params.explicit_limit)
  {
    if (select_lex.braces)
    {
      if (select_lex.limit_params.offset_limit)
        csep->limitStart(((Item_int*)select_lex.limit_params.offset_limit)->val_int());

      if (select_lex.limit_params.select_limit)
        csep->limitNum(((Item_int*)select_lex.limit_params.select_limit)->val_int());
    }
  }
  // other types of queries that have explicit LIMIT
  else if (select_lex.limit_params.explicit_limit)
  {
    uint32_t limitOffset = 0;

    if (select_lex.join)
    {
      JOIN* join = select_lex.join;

      // @bug5729. After upgrade, join->unit sometimes is uninitialized pointer
      // (not null though) and will cause seg fault. Prefer checking
      // select_lex->limit_params.offset_limit if not null.
      if (join->select_lex && join->select_lex->limit_params.offset_limit &&
          join->select_lex->limit_params.offset_limit->fixed() &&
          join->select_lex->limit_params.select_limit && join->select_lex->limit_params.select_limit->fixed())
      {
        limitOffset = join->select_lex->limit_params.offset_limit->val_int();
        limitNum = join->select_lex->limit_params.select_limit->val_int();
      }
      else if (join->unit)
      {
        limitOffset = join->unit->lim.get_offset_limit();
        limitNum = join->unit->lim.get_select_limit() - limitOffset;
      }
    }
    else
    {
      if (select_lex.master_unit()->global_parameters()->limit_params.offset_limit)
      {
        Item_int* offset =
            (Item_int*)select_lex.master_unit()->global_parameters()->limit_params.offset_limit;
        limitOffset = offset->val_int();
      }

      if (select_lex.master_unit()->global_parameters()->limit_params.select_limit)
      {
        Item_int* select =
            (Item_int*)select_lex.master_unit()->global_parameters()->limit_params.select_limit;
        limitNum = select->val_int();
      }
    }

    csep->limitStart(limitOffset);
    csep->limitNum(limitNum);
  }
  // If an explicit limit is not specified, use the system variable value
  else
  {
    csep->limitNum(gwi.thd->variables.select_limit);
  }

  // We don't currently support limit with correlated subquery
  if (csep->limitNum() != (uint64_t)-1 && gwi.subQuery && !gwi.correlatedTbNameVec.empty())
  {
    gwi.fatalParseError = true;
    gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_LIMIT_SUB);
    setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
    return ER_CHECK_NOT_IMPLEMENTED;
  }

  return 0;
}

// Loop over available indexes to find and extract corresponding EI column statistics
// for the first column of the index if any.
// Statistics is stored in GWI context.
// Mock for ES 10.6
// #if MYSQL_VERSION_ID >= 110401
// void extractColumnStatistics(Item_field* ifp, gp_walk_info& gwi)
// {
//   for (uint j = 0; j < ifp->field->table->s->keys; j++)
//   {
//     for (uint i = 0; i < ifp->field->table->s->key_info[j].usable_key_parts; i++)
//     {
//       if (ifp->field->table->s->key_info[j].key_part[i].fieldnr == ifp->field->field_index + 1)
//       {
//         if (i == 0 && ifp->field->read_stats)
//         {
//           assert(ifp->field->table->s);
//           auto* histogram = dynamic_cast<Histogram_json_hb*>(ifp->field->read_stats->histogram);
//           if (histogram)
//           {
//             SchemaAndTableName tableName = {ifp->field->table->s->db.str,
//                                             ifp->field->table->s->table_name.str};
//             auto tableStatisticsMapIt = gwi.tableStatisticsMap.find(tableName);
//             if (tableStatisticsMapIt == gwi.tableStatisticsMap.end())
//             {
//               gwi.tableStatisticsMap.insert({tableName, {{ifp->field->field_name.str, *histogram}}});
//             }
//             else
//             {
//               tableStatisticsMapIt->second.insert({ifp->field->field_name.str, *histogram});
//             }
//           }
//         }
//       }
//     }
//   }
// }
// #else
// void extractColumnStatistics(Item_field* /*ifp*/, gp_walk_info& /*gwi*/)
// {
// }
// #endif

/*@brief  Process SELECT part of a query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  Processes SELECT part of a query or sub-query
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processSelect(SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep, vector<Item_field*>& funcFieldVec,
                  CalpontSelectExecutionPlan::SelectList& selectSubList)
{
  gwi.select_lex = &select_lex;
#ifdef DEBUG_WALK_COND
  {
    cerr << "------------------- SELECT --------------------" << endl;
    List_iterator_fast<Item> it(select_lex.item_list);
    Item* item;

    while ((item = it++))
    {
      debug_walk(item, 0);
    }

    cerr << "-----------------------------------------------\n" << endl;
  }
#endif

  // analyze SELECT and ORDER BY parts - do they have implicit GROUP BY induced by aggregates?
  {
    if (select_lex.group_list.first)
    {
      // we have an explicit GROUP BY.
      gwi.implicitExplicitGroupBy = true;
    }
    else
    {
      // do we have an implicit GROUP BY?
      List_iterator_fast<Item> it(select_lex.item_list);
      Item* item;

      while ((item = it++))
      {
        analyzeForImplicitGroupBy(item, gwi);
      }
      SQL_I_List<ORDER> order_list = select_lex.order_list;
      ORDER* ordercol = static_cast<ORDER*>(order_list.first);

      for (; ordercol; ordercol = ordercol->next)
      {
        analyzeForImplicitGroupBy(*(ordercol->item), gwi);
      }
    }
  }
  // populate returnedcolumnlist and columnmap
  List_iterator_fast<Item> it(select_lex.item_list);
  Item* item;

  // empty rcWorkStack and ptWorkStack. They should all be empty by now.
  clearStacks(gwi, false, true);

  // indicate the starting pos of scalar returned column, because some join column
  // has been inserted to the returned column list.
  if (gwi.subQuery)
  {
    ScalarSub* scalar = dynamic_cast<ScalarSub*>(gwi.subQuery);

    if (scalar)
      scalar->returnedColPos(gwi.additionalRetCols.size());
  }

  while ((item = it++))
  {
    string itemAlias = (item->name.length ? item->name.str : "<NULL>");

    // @bug 5916. Need to keep checking until getting concret item in case
    // of nested view.
    Item* baseItem = item;
    while (item->type() == Item::REF_ITEM)
    {
      Item_ref* ref = (Item_ref*)item;
      item = (*(ref->ref));
    }

    Item::Type itype = item->type();

    switch (itype)
    {
      case Item::FIELD_ITEM:
      {
        Item_field* ifp = (Item_field*)item;
        // extractColumnStatistics(ifp, gwi);
        // Handle * case
        if (ifp->field_name.length && string(ifp->field_name.str) == "*")
        {
          collectAllCols(gwi, ifp);
          break;
        }
        SimpleColumn* sc = buildSimpleColumn(ifp, gwi);

        if (sc)
        {
          String str;
          ifp->print(&str, QT_ORDINARY);
          string fullname(str.c_ptr());

          if (!ifp->is_explicit_name())  // no alias
          {
            sc->alias(fullname);
          }
          else  // alias
          {
            if (!itemAlias.empty())
              sc->alias(itemAlias);
          }

          // We need to look into GROUP BY columns to decide if we need to wrap a column.
          ReturnedColumn* rc = wrapIntoAggregate(sc, gwi, baseItem);

          SRCP sprc(rc);
          pushReturnedCol(gwi, baseItem, sprc);

          gwi.columnMap.insert(
              CalpontSelectExecutionPlan::ColumnMap::value_type(string(ifp->field_name.str), sprc));
          TABLE_LIST* tmp = 0;

          if (ifp->cached_table)
            tmp = ifp->cached_table;

          gwi.tableMap[make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias(),
                                       sc->isColumnStore())] = make_pair(1, tmp);
        }
        else
        {
          setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
          delete sc;
          return ER_INTERNAL_ERROR;
        }

        break;
      }

      // aggregate column
      case Item::SUM_FUNC_ITEM:
      {
        ReturnedColumn* ac = buildAggregateColumn(item, gwi);

        if (gwi.fatalParseError)
        {
          // e.g., non-support ref column
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          delete ac;
          return ER_CHECK_NOT_IMPLEMENTED;
        }

        // add this agg col to returnedColumnList
        boost::shared_ptr<ReturnedColumn> spac(ac);
        pushReturnedCol(gwi, item, spac);
        break;
      }

      case Item::FUNC_ITEM:
      {
        Item_func* ifp = static_cast<Item_func*>(item);

        // @bug4383. error out non-support stored function
        if (ifp->functype() == Item_func::FUNC_SP)
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_SP_FUNCTION_NOT_SUPPORT);
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }

        if (string(ifp->func_name()) == "xor")
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }

        uint16_t parseInfo = 0;
        vector<Item_field*> tmpVec;
        bool hasNonSupportItem = false;
        parse_item(ifp, tmpVec, hasNonSupportItem, parseInfo, &gwi);

        if (ifp->with_subquery() || string(ifp->func_name()) == string("<in_optimizer>") ||
            ifp->functype() == Item_func::NOT_ALL_FUNC || parseInfo & SUB_BIT)
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_SELECT_SUB);
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }

        // if "IN" or "BETWEEN" are in the SELECT clause, build function column
        string funcName = ifp->func_name();
        ReturnedColumn* rc;
        if (funcName == "in" || funcName == " IN " || funcName == "between")
        {
          rc = buildFunctionColumn(ifp, gwi, hasNonSupportItem, true);
        }
        else
        {
          rc = buildFunctionColumn(ifp, gwi, hasNonSupportItem);
        }

        SRCP srcp(rc);

        if (rc)
        {
          // MCOL-2178 CS has to process determenistic functions with constant arguments.
          if (!hasNonSupportItem && ifp->const_item() && !(parseInfo & AF_BIT) && tmpVec.size() == 0)
          {
            srcp.reset(buildReturnedColumn(item, gwi, gwi.fatalParseError));
            pushReturnedCol(gwi, item, srcp);

            if (ifp->name.length)
              srcp->alias(ifp->name.str);

            continue;
          }
          // FIXME: usage of pushReturnedCol instead of gwi.returnedCols.push_back(srcp) here
          // made within MCOL-5776 produced bug MCOL-5932 so, the check of equal columns is disabled
          pushReturnedCol(gwi, item, srcp);
        }
        else  // This was a vtable post-process block
        {
          hasNonSupportItem = false;
          uint32_t before_size = funcFieldVec.size();
          parse_item(ifp, funcFieldVec, hasNonSupportItem, parseInfo, &gwi);
          uint32_t after_size = funcFieldVec.size();

          // pushdown handler projection functions
          // @bug3881. set_user_var can not be treated as constant function
          // @bug5716. Try to avoid post process function for union query.
          if (!hasNonSupportItem && (after_size - before_size) == 0 && !(parseInfo & AGG_BIT) &&
              !(parseInfo & SUB_BIT))
          {
            ConstantColumn* cc = buildConstantColumnMaybeNullUsingValStr(ifp, gwi);

            SRCP srcp(cc);

            if (ifp->name.length)
              cc->alias(ifp->name.str);

            pushReturnedCol(gwi, ifp, srcp);

            // clear the error set by buildFunctionColumn
            gwi.fatalParseError = false;
            gwi.parseErrorText = "";
            break;
          }
          else if (hasNonSupportItem || parseInfo & AGG_BIT || parseInfo & SUB_BIT ||
                   (gwi.fatalParseError && gwi.subQuery))
          {
            if (gwi.parseErrorText.empty())
            {
              Message::Args args;
              args.add(ifp->func_name());
              gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORTED_FUNCTION, args);
            }

            setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
            return ER_CHECK_NOT_IMPLEMENTED;
          }
          else if (gwi.subQuery && (isPredicateFunction(ifp, &gwi) || ifp->type() == Item::COND_ITEM))
          {
            gwi.fatalParseError = true;
            gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
            setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
            return ER_CHECK_NOT_IMPLEMENTED;
          }

          //@Bug 3030 Add error check for dml statement
          if (ha_mcs_common::isUpdateOrDeleteStatement(gwi.thd->lex->sql_command))
          {
            if (after_size - before_size != 0)
            {
              gwi.parseErrorText = ifp->func_name();
              return -1;
            }
          }
          else
          {
            Message::Args args;
            args.add(ifp->func_name());
            gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORTED_FUNCTION, args);
            setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
            return ER_CHECK_NOT_IMPLEMENTED;
          }
        }

        break;
      }  // End of FUNC_ITEM

      // DRRTUY Replace the whole section with typeid() checks or use
      // static_cast here
      case Item::CONST_ITEM:
      {
        switch (item->cmp_type())

        {
          case INT_RESULT:
          case STRING_RESULT:
          case DECIMAL_RESULT:
          case REAL_RESULT:
          case TIME_RESULT:
          {
            if (ha_mcs_common::isUpdateOrDeleteStatement(gwi.thd->lex->sql_command))
            {
            }
            else
            {
              // do not push the dummy column (mysql added) to returnedCol
              if (item->name.length && string(item->name.str) == "Not_used")
                continue;

              // @bug3509. Constant column is sent to ExeMgr now.
              SRCP srcp(buildReturnedColumn(item, gwi, gwi.fatalParseError));

              if (item->name.length)
                srcp->alias(item->name.str);

              pushReturnedCol(gwi, item, srcp);
            }

            break;
          }
          default:
          {
            IDEBUG(cerr << "Warning unsupported cmp_type() in projection" << endl);
            // noop
          }
        }
        break;
      }  // CONST_ITEM ends here

      case Item::NULL_ITEM:
      {
        if (ha_mcs_common::isUpdateOrDeleteStatement(gwi.thd->lex->sql_command))
        {
        }
        else
        {
          SRCP srcp(buildReturnedColumn(item, gwi, gwi.fatalParseError));
          pushReturnedCol(gwi, item, srcp);

          if (item->name.length)
            srcp->alias(item->name.str);
        }

        break;
      }

      case Item::SUBSELECT_ITEM:
      {
        Item_subselect* sub = (Item_subselect*)item;

        if (sub->substype() != Item_subselect::SINGLEROW_SUBS)
        {
          gwi.fatalParseError = true;
          gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_SELECT_SUB);
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }

#ifdef DEBUG_WALK_COND
        cerr << "SELECT clause SUBSELECT Item: " << sub->substype() << endl;
        JOIN* join = sub->get_select_lex()->join;

        if (join)
        {
          Item_cond* cond = static_cast<Item_cond*>(join->conds);

          if (cond)
            cond->traverse_cond(debug_walk, &gwi, Item::POSTFIX);
        }

        cerr << "Finish SELECT clause subselect item traversing" << endl;
#endif
        SelectSubQuery* selectSub = new SelectSubQuery(gwi, sub);
        // selectSub->gwip(&gwi);
        SCSEP ssub = selectSub->transform();

        if (!ssub || gwi.fatalParseError)
        {
          if (gwi.parseErrorText.empty())
            gwi.parseErrorText = "Unsupported Item in SELECT subquery.";

          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);

          return ER_CHECK_NOT_IMPLEMENTED;
        }

        selectSubList.push_back(ssub);
        SimpleColumn* rc = new SimpleColumn();
        rc->colSource(rc->colSource() | SELECT_SUB);
        rc->timeZone(gwi.timeZone);

        if (sub->get_select_lex()->get_table_list())
        {
          rc->viewName(getViewName(sub->get_select_lex()->get_table_list()), lower_case_table_names);
        }
        if (sub->name.length)
          rc->alias(sub->name.str);

        gwi.returnedCols.push_back(SRCP(rc));

        break;
      }

      case Item::COND_ITEM:
      {
        gwi.fatalParseError = true;
        gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_FILTER_COND_EXP);
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
        return ER_CHECK_NOT_IMPLEMENTED;
      }

      case Item::EXPR_CACHE_ITEM:
      {
        printf("EXPR_CACHE_ITEM in getSelectPlan\n");
        gwi.fatalParseError = true;
        gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(ERR_UNKNOWN_COL);
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
        return ER_CHECK_NOT_IMPLEMENTED;
      }

      case Item::WINDOW_FUNC_ITEM:
      {
        SRCP srcp(buildWindowFunctionColumn(item, gwi, gwi.fatalParseError));

        if (!srcp || gwi.fatalParseError)
        {
          if (gwi.parseErrorText.empty())
            gwi.parseErrorText = "Unsupported Item in SELECT subquery.";

          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }

        pushReturnedCol(gwi, item, srcp);
        break;
      }
      case Item::TYPE_HOLDER:
      {
        if (!gwi.tbList.size())
        {
          gwi.parseErrorText = "subquery with VALUES";
          gwi.fatalParseError = true;
          setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
          return ER_CHECK_NOT_IMPLEMENTED;
        }
        else
        {
          std::cerr << "********** received TYPE_HOLDER *********" << std::endl;
        }
        break;
      }

      default:
      {
        break;
      }
    }
  }

  // @bug4388 normalize the project coltypes for union main select list
  if (!csep->unionVec().empty())
  {
    unsigned int unionedTypeRc = 0;

    for (uint32_t i = 0; i < gwi.returnedCols.size(); i++)
    {
      vector<CalpontSystemCatalog::ColType> coltypes;

      for (uint32_t j = 0; j < csep->unionVec().size(); j++)
      {
        CalpontSelectExecutionPlan* unionCsep =
            dynamic_cast<CalpontSelectExecutionPlan*>(csep->unionVec()[j].get());
        coltypes.push_back(unionCsep->returnedCols()[i]->resultType());

        // @bug5976. set hasAggregate true for the main column if
        // one corresponding union column has aggregate
        if (unionCsep->returnedCols()[i]->hasAggregate())
          gwi.returnedCols[i]->hasAggregate(true);
      }

      gwi.returnedCols[i]->resultType(
          CalpontSystemCatalog::ColType::convertUnionColType(coltypes, unionedTypeRc));

      if (unionedTypeRc != 0)
      {
        gwi.parseErrorText = IDBErrorInfo::instance()->errorMsg(unionedTypeRc);
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, gwi.parseErrorText, gwi);
        return ER_CHECK_NOT_IMPLEMENTED;
      }
    }
  }
  return 0;
}

/*@brief  Process ORDER BY part of a query or sub-query      */
/***********************************************************
 * DESCRIPTION:
 *  Processes ORDER BY part of a query or sub-query
 * RETURNS
 *  error id as an int
 ***********************************************************/
int processOrderBy(SELECT_LEX& select_lex, gp_walk_info& gwi, SCSEP& csep,
                   boost::shared_ptr<CalpontSystemCatalog>& csc, SRCP& minSc, const bool isUnion,
                   const bool unionSel)
{
  SQL_I_List<ORDER> order_list = select_lex.order_list;
  ORDER* ordercol = static_cast<ORDER*>(order_list.first);

  // check if window functions are in order by. InfiniDB process order by list if
  // window functions are involved, either in order by or projection.
  for (; ordercol; ordercol = ordercol->next)
  {
    if ((*(ordercol->item))->type() == Item::WINDOW_FUNC_ITEM)
      gwi.hasWindowFunc = true;
    // XXX: TODO: implement a proper analysis of what we support.
    // MCOL-2166 Looking for this sorting item in GROUP_BY items list.
    // Shouldn't look into this if query doesn't have GROUP BY or
    // aggregations
    if (select_lex.agg_func_used() && select_lex.group_list.first &&
        !sortItemIsInGrouping(*ordercol->item, select_lex.group_list.first))
    {
      std::ostringstream ostream;
      std::ostringstream& osr = ostream;
      getColNameFromItem(osr, *ordercol->item);
      Message::Args args;
      args.add(ostream.str());
      string emsg = IDBErrorInfo::instance()->errorMsg(ERR_NOT_SUPPORTED_GROUPBY_ORDERBY_EXPRESSION, args);
      gwi.parseErrorText = emsg;
      setError(gwi.thd, ER_INTERNAL_ERROR, emsg, gwi);
      return ERR_NOT_SUPPORTED_GROUPBY_ORDERBY_EXPRESSION;
    }
  }

  // re-visit the first of ordercol list
  ordercol = static_cast<ORDER*>(order_list.first);

  for (; ordercol; ordercol = ordercol->next)
  {
    ReturnedColumn* rc = NULL;

    if (ordercol->in_field_list && ordercol->counter_used)
    {
      rc = gwi.returnedCols[ordercol->counter - 1]->clone();
      rc->orderPos(ordercol->counter - 1);
      // can not be optimized off if used in order by with counter.
      // set with self derived table alias if it's derived table
      gwi.returnedCols[ordercol->counter - 1]->incRefCount();
    }
    else
    {
      Item* ord_item = *(ordercol->item);

      // ignore not_used column on order by.
      if ((ord_item->type() == Item::CONST_ITEM && ord_item->cmp_type() == INT_RESULT) &&
          ord_item->full_name() && !strcmp(ord_item->full_name(), "Not_used"))
      {
        continue;
      }
      else if (ord_item->type() == Item::CONST_ITEM && ord_item->cmp_type() == INT_RESULT)
      {
        // DRRTUY This section looks useless b/c there is no
        // way to put constant INT into an ORDER BY list
        rc = gwi.returnedCols[((Item_int*)ord_item)->val_int() - 1]->clone();
      }
      else if (ord_item->type() == Item::SUBSELECT_ITEM)
      {
        gwi.fatalParseError = true;
      }
      else if ((ord_item->type() == Item::FUNC_ITEM) &&
               (((Item_func*)ord_item)->functype() == Item_func::COLLATE_FUNC))
      {
        push_warning(gwi.thd, Sql_condition::WARN_LEVEL_NOTE, WARN_OPTION_IGNORED,
                     "COLLATE is ignored in ColumnStore");
        continue;
      }
      else
      {
        rc = buildReturnedColumn(ord_item, gwi, gwi.fatalParseError);

        rc = wrapIntoAggregate(rc, gwi, ord_item);
      }
      // @bug5501 try item_ptr if item can not be fixed. For some
      // weird dml statement state, item can not be fixed but the
      // infomation is available in item_ptr.
      if (!rc || gwi.fatalParseError)
      {
        Item* item_ptr = ordercol->item_ptr;

        while (item_ptr->type() == Item::REF_ITEM)
          item_ptr = *(((Item_ref*)item_ptr)->ref);

        rc = buildReturnedColumn(item_ptr, gwi, gwi.fatalParseError);
      }

      if (!rc)
      {
        string emsg = IDBErrorInfo::instance()->errorMsg(ERR_NON_SUPPORT_ORDER_BY);
        gwi.parseErrorText = emsg;
        setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, emsg, gwi);
        return ER_CHECK_NOT_IMPLEMENTED;
      }
    }

    if (ordercol->direction == ORDER::ORDER_ASC)
      rc->asc(true);
    else
      rc->asc(false);

    gwi.orderByCols.push_back(SRCP(rc));
  }

  // make sure columnmap, returnedcols and count(*) arg_list are not empty
  TableMap::iterator tb_iter = gwi.tableMap.begin();

  try
  {
    for (; tb_iter != gwi.tableMap.end(); tb_iter++)
    {
      if ((*tb_iter).second.first == 1)
        continue;

      CalpontSystemCatalog::TableAliasName tan = (*tb_iter).first;
      CalpontSystemCatalog::TableName tn = make_table((*tb_iter).first.schema, (*tb_iter).first.table);
      SimpleColumn* sc = getSmallestColumn(csc, tn, tan, (*tb_iter).second.second->table, gwi);
      SRCP srcp(sc);
      gwi.columnMap.insert(CalpontSelectExecutionPlan::ColumnMap::value_type(sc->columnName(), srcp));
      (*tb_iter).second.first = 1;
    }
  }
  catch (runtime_error& e)
  {
    setError(gwi.thd, ER_INTERNAL_ERROR, e.what(), gwi);
    return ER_INTERNAL_ERROR;
  }
  catch (...)
  {
    string emsg = IDBErrorInfo::instance()->errorMsg(ERR_LOST_CONN_EXEMGR);
    setError(gwi.thd, ER_INTERNAL_ERROR, emsg, gwi);
    return ER_INTERNAL_ERROR;
  }

  if (!gwi.count_asterisk_list.empty() || !gwi.no_parm_func_list.empty() || gwi.returnedCols.empty())
  {
    // get the smallest column from colmap
    CalpontSelectExecutionPlan::ColumnMap::const_iterator iter;
    int minColWidth = 0;
    CalpontSystemCatalog::ColType ct;

    try
    {
      for (iter = gwi.columnMap.begin(); iter != gwi.columnMap.end(); ++iter)
      {
        // should always not null
        SimpleColumn* sc = dynamic_cast<SimpleColumn*>(iter->second.get());

        if (sc && !(sc->joinInfo() & JOIN_CORRELATED))
        {
          ct = csc->colType(sc->oid());

          if (minColWidth == 0)
          {
            minColWidth = ct.colWidth;
            minSc = iter->second;
          }
          else if (ct.colWidth < minColWidth)
          {
            minColWidth = ct.colWidth;
            minSc = iter->second;
          }
        }
      }
    }
    catch (...)
    {
      string emsg = IDBErrorInfo::instance()->errorMsg(ERR_LOST_CONN_EXEMGR);
      setError(gwi.thd, ER_INTERNAL_ERROR, emsg, gwi);
      return ER_INTERNAL_ERROR;
    }

    if (gwi.returnedCols.empty() && gwi.additionalRetCols.empty() && minSc)
      gwi.returnedCols.push_back(minSc);
  }

  // ORDER BY translation part
  if (!isUnion && !gwi.hasWindowFunc && gwi.subSelectType == CalpontSelectExecutionPlan::MAIN_SELECT)
  {
    {
      if (unionSel)
        order_list = select_lex.master_unit()->global_parameters()->order_list;

      ordercol = static_cast<ORDER*>(order_list.first);

      for (; ordercol; ordercol = ordercol->next)
      {
        Item* ord_item = *(ordercol->item);

        if (ord_item->name.length)
        {
          // for union order by 1 case. For unknown reason, it doesn't show in_field_list
          if (ord_item->type() == Item::CONST_ITEM && ord_item->cmp_type() == INT_RESULT)
          {
          }
          else if (ord_item->type() == Item::SUBSELECT_ITEM)
          {
          }
          else
          {
          }
        }
      }
    }

    if (gwi.orderByCols.size())  // has order by
    {
      csep->hasOrderBy(true);
      // To activate LimitedOrderBy
      csep->orderByThreads(get_orderby_threads(gwi.thd));
      csep->specHandlerProcessed(true);
    }
  }

  return 0;
}

/*@brief  Translates SELECT_LEX into CSEP                  */
/***********************************************************
 * DESCRIPTION:
 *  This function takes SELECT_LEX and tries to produce
 *  a corresponding CSEP out of it. It is made of parts that
 *  process parts of the query, e.g. FROM, WHERE, SELECT,
 *  HAVING, GROUP BY, ORDER BY. FROM, WHERE, LIMIT are processed
 *  by corresponding methods. CS calls getSelectPlan()
 *  recursively to process subqueries.
 * ARGS
 *  isUnion if true CS processes UNION unit now
 *  isSelectHandlerTop removes offset at the top of SH query.
 * RETURNS
 *  error id as an int
 ***********************************************************/
int getSelectPlan(gp_walk_info& gwi, SELECT_LEX& select_lex, SCSEP& csep, bool isUnion,
                  bool isSelectHandlerTop, bool isSelectLexUnit, const std::vector<COND*>& condStack)
{
#ifdef DEBUG_WALK_COND
  cerr << "getSelectPlan()" << endl;
#endif
  int rc = 0;
  // rollup is currently not supported
  bool withRollup = select_lex.olap == ROLLUP_TYPE;

  setExecutionParams(gwi, csep);

  gwi.subSelectType = csep->subType();
  uint32_t sessionID = csep->sessionID();
  gwi.sessionid = sessionID;
  boost::shared_ptr<CalpontSystemCatalog> csc = CalpontSystemCatalog::makeCalpontSystemCatalog(sessionID);
  csc->identity(CalpontSystemCatalog::FE);
  csep->timeZone(gwi.timeZone);
  gwi.csc = csc;

  CalpontSelectExecutionPlan::SelectList derivedTbList;
  // @bug 1796. Remember table order on the FROM list.
  gwi.clauseType = FROM;
  if ((rc = processFrom(isUnion, select_lex, gwi, csep, isSelectHandlerTop, isSelectLexUnit)))
  {
    return rc;
  }

  gwi.clauseType = WHERE;
  if ((rc = processWhere(select_lex, gwi, csep, condStack)))
  {
    return rc;
  }

  gwi.clauseType = SELECT;
  SELECT_LEX* originalSelectLex = gwi.select_lex;  // XXX: SZ: should it be restored in case of error return?
  vector<Item_field*> funcFieldVec;
  CalpontSelectExecutionPlan::SelectList selectSubList;

  if ((rc = processSelect(select_lex, gwi, csep, funcFieldVec, selectSubList)))
  {
    return rc;
  }

  // Having clause handling
  gwi.clauseType = HAVING;
  std::unique_ptr<ParseTree> havingFilter;
  if ((rc = processHaving(select_lex, gwi, csep, havingFilter)))
  {
    return rc;
  }

  // for post process expressions on the select list
  // error out post process for union and sub select unit
  if (isUnion || gwi.subSelectType != CalpontSelectExecutionPlan::MAIN_SELECT)
  {
    if (funcFieldVec.size() != 0 && !gwi.fatalParseError)
    {
      string emsg("Fatal parse error in vtable mode: Unsupported Items in union or sub select unit");
      setError(gwi.thd, ER_CHECK_NOT_IMPLEMENTED, emsg);
      return ER_CHECK_NOT_IMPLEMENTED;
    }
  }

  for (uint32_t i = 0; i < funcFieldVec.size(); i++)
  {
    SimpleColumn* sc = buildSimpleColumn(funcFieldVec[i], gwi);

    if (!sc || gwi.fatalParseError)
    {
      string emsg;

      if (gwi.parseErrorText.empty())
      {
        emsg = "un-recognized column";

        if (funcFieldVec[i]->name.length)
          emsg += string(funcFieldVec[i]->name.str);
      }
      else
      {
        emsg = gwi.parseErrorText;
      }

      setError(gwi.thd, ER_INTERNAL_ERROR, emsg, gwi);
      return ER_INTERNAL_ERROR;
    }

    String str;
    funcFieldVec[i]->print(&str, QT_ORDINARY);
    sc->alias(string(str.c_ptr()));
    sc->tableAlias(sc->tableAlias());
    SRCP srcp(wrapIntoAggregate(sc, gwi, funcFieldVec[i]));
    uint32_t j = 0;

    for (; j < gwi.returnedCols.size(); j++)
    {
      if (sc->sameColumn(gwi.returnedCols[j].get()))
      {
        SimpleColumn* field = dynamic_cast<SimpleColumn*>(gwi.returnedCols[j].get());

        if (field && field->alias() == sc->alias())
          break;
      }
    }

    if (j == gwi.returnedCols.size())
    {
      gwi.returnedCols.push_back(srcp);
      // XXX: SZ: deduplicate here?
      gwi.columnMap.insert(
          CalpontSelectExecutionPlan::ColumnMap::value_type(string(funcFieldVec[i]->field_name.str), srcp));

      string fullname;
      fullname = str.c_ptr();
      TABLE_LIST* tmp = (funcFieldVec[i]->cached_table ? funcFieldVec[i]->cached_table : 0);
      gwi.tableMap[make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias(),
                                   sc->isColumnStore())] = make_pair(1, tmp);
    }
  }

  SRCP minSc;  // min width projected column. for count(*) use

  bool unionSel = (!isUnion && select_lex.master_unit()->is_unit_op()) ? true : false;

  // Group by list. not valid for union main query
  if (!unionSel)
  {
    gwi.clauseType = GROUP_BY;
    if ((rc = processGroupBy(select_lex, gwi, withRollup)))
    {
      return rc;
    }
  }

  if ((rc = processOrderBy(select_lex, gwi, csep, csc, minSc, isUnion, unionSel)))
  {
    CalpontSystemCatalog::removeCalpontSystemCatalog(sessionID);
    return rc;
  }

  // json dictionary for debug and testing options
  csep->pron(get_pron(gwi.thd));

  // We don't currently support limit with correlated subquery
  if ((rc = processLimitAndOffset(select_lex, gwi, csep, unionSel, isUnion, isSelectHandlerTop)))
  {
    return rc;
  }

  if (select_lex.options & SELECT_DISTINCT)
    csep->distinct(true);

  // add the smallest column to count(*) parm.
  // select constant in subquery case
  std::vector<AggregateColumn*>::iterator coliter;

  if (!minSc)
  {
    if (!gwi.returnedCols.empty())
      minSc = gwi.returnedCols[0];
    else if (!gwi.additionalRetCols.empty())
      minSc = gwi.additionalRetCols[0];
  }

  // @bug3523, count(*) on subquery always pick column[0].
  SimpleColumn* sc = dynamic_cast<SimpleColumn*>(minSc.get());

  if (sc && sc->schemaName().empty())
  {
    if (gwi.derivedTbList.size() >= 1)
    {
      SimpleColumn* sc1 = new SimpleColumn();
      sc1->columnName(sc->columnName());
      sc1->tableName(sc->tableName());
      sc1->tableAlias(sc->tableAlias());
      sc1->viewName(sc->viewName());
      sc1->partitions(sc->partitions());
      sc1->colPosition(0);
      sc1->timeZone(gwi.timeZone);
      minSc.reset(sc1);
    }
  }

  for (coliter = gwi.count_asterisk_list.begin(); coliter != gwi.count_asterisk_list.end(); ++coliter)
  {
    // @bug5977 @note should never throw this, but checking just in case.
    // When ExeMgr fix is ready, this should not error out...
    if (dynamic_cast<AggregateColumn*>(minSc.get()))
    {
      gwi.fatalParseError = true;
      gwi.parseErrorText = "No project column found for aggregate function";
      setError(gwi.thd, ER_INTERNAL_ERROR, gwi.parseErrorText, gwi);
      return ER_CHECK_NOT_IMPLEMENTED;
    }

    // Replace the last (presumably constant) object with minSc
    if ((*coliter)->aggParms().empty())
    {
      (*coliter)->aggParms().push_back(minSc);
    }
    else
    {
      (*coliter)->aggParms()[0] = minSc;
    }
  }

  std::vector<FunctionColumn*>::iterator funciter;

  SPTP sptp(new ParseTree(minSc.get()->clone()));

  for (funciter = gwi.no_parm_func_list.begin(); funciter != gwi.no_parm_func_list.end(); ++funciter)
  {
    FunctionParm funcParms = (*funciter)->functionParms();
    funcParms.push_back(sptp);
    (*funciter)->functionParms(funcParms);
  }

  // set sequence# for subquery localCols
  for (uint32_t i = 0; i < gwi.localCols.size(); i++)
    gwi.localCols[i]->sequence(i);

  gwi.select_lex = originalSelectLex;
  // append additionalRetCols to returnedCols
  gwi.returnedCols.insert(gwi.returnedCols.begin(), gwi.additionalRetCols.begin(),
                          gwi.additionalRetCols.end());

  csep->groupByCols(gwi.groupByCols);
  csep->withRollup(withRollup);
  csep->orderByCols(gwi.orderByCols);
  csep->returnedCols(gwi.returnedCols);
  csep->columnMap(gwi.columnMap);
  csep->having(havingFilter.release());
  csep->derivedTableList(gwi.derivedTbList);
  csep->selectSubList(selectSubList);
  csep->subSelectList(gwi.subselectList);
  return 0;
}

int cp_get_table_plan(THD* thd, SCSEP& csep, cal_table_info& ti, long timeZone)
{
  SubQueryChainHolder chainHolder;
  bool allocated = false;
  gp_walk_info* gwi;
  if (ti.condInfo)
  {
    gwi = &ti.condInfo->gwi;
  }
  else
  {
    allocated = true;
    gwi = new gp_walk_info(timeZone, &chainHolder.chain);
  }

  gwi->thd = thd;
  LEX* lex = thd->lex;
  idbassert(lex != 0);
  uint32_t sessionID = csep->sessionID();
  gwi->sessionid = sessionID;
  TABLE* table = ti.msTablePtr;
  boost::shared_ptr<CalpontSystemCatalog> csc = CalpontSystemCatalog::makeCalpontSystemCatalog(sessionID);
  csc->identity(CalpontSystemCatalog::FE);

  // get all columns that mysql needs to fetch
  MY_BITMAP* read_set = table->read_set;
  Field **f_ptr, *field;
  gwi->columnMap.clear();

  for (f_ptr = table->field; (field = *f_ptr); f_ptr++)
  {
    if (bitmap_is_set(read_set, field->field_index))
    {
      SimpleColumn* sc =
          new SimpleColumn(table->s->db.str, table->s->table_name.str, field->field_name.str, sessionID);
      string alias(table->alias.c_ptr());
      if (lower_case_table_names)
      {
        boost::algorithm::to_lower(alias);
      }
      sc->tableAlias(alias);
      sc->timeZone(gwi->timeZone);
      sc->partitions(getPartitions(table));
      assert(sc);
      boost::shared_ptr<SimpleColumn> spsc(sc);
      gwi->returnedCols.push_back(spsc);
      gwi->columnMap.insert(
          CalpontSelectExecutionPlan::ColumnMap::value_type(string(field->field_name.str), spsc));
    }
  }

  if (gwi->columnMap.empty())
  {
    CalpontSystemCatalog::TableName tn = make_table(table->s->db.str, table->s->table_name.str);
    CalpontSystemCatalog::TableAliasName tan =
        make_aliastable(table->s->db.str, table->s->table_name.str, table->alias.c_ptr());
    SimpleColumn* sc = getSmallestColumn(csc, tn, tan, table, *gwi);
    SRCP srcp(sc);
    gwi->columnMap.insert(CalpontSelectExecutionPlan::ColumnMap::value_type(sc->columnName(), srcp));
    gwi->returnedCols.push_back(srcp);
  }

  // get filter
  if (ti.condInfo)
  {
    gp_walk_info* gwi = &ti.condInfo->gwi;
    ParseTree* filters = 0;
    ParseTree* ptp = 0;
    ParseTree* rhs = 0;

    while (!gwi->ptWorkStack.empty())
    {
      filters = gwi->ptWorkStack.top();
      gwi->ptWorkStack.pop();
      SimpleFilter* sf = dynamic_cast<SimpleFilter*>(filters->data());

      if (sf && sf->op()->data() == "noop")
      {
        delete filters;
        filters = 0;

        if (gwi->ptWorkStack.empty())
          break;

        continue;
      }

      if (gwi->ptWorkStack.empty())
        break;

      ptp = new ParseTree(new LogicOperator("and"));
      ptp->left(filters);
      rhs = gwi->ptWorkStack.top();
      gwi->ptWorkStack.pop();
      ptp->right(rhs);
      gwi->ptWorkStack.push(ptp);
    }

    csep->filters(filters);
  }

  csep->returnedCols(gwi->returnedCols);
  csep->columnMap(gwi->columnMap);
  CalpontSelectExecutionPlan::TableList tblist;
  tblist.push_back(make_aliastable(table->s->db.str, table->s->table_name.str, table->alias.c_ptr(), true,
                                   lower_case_table_names));
  csep->tableList(tblist);

  // @bug 3321. Set max number of blocks in a dictionary file to be scanned for filtering
  csep->stringScanThreshold(get_string_scan_threshold(gwi->thd));

  if (allocated)
  {
    delete gwi;
  }
  return 0;
}

int cs_get_derived_plan(ha_columnstore_derived_handler* handler, THD* /*thd*/, SCSEP& csep, gp_walk_info& gwi)
{
  SELECT_LEX& select_lex = *handler->select;
  int status = getSelectPlan(gwi, select_lex, csep, false);

  if (status > 0)
    return ER_INTERNAL_ERROR;
  else if (status < 0)
    return status;

  if (csep->traceOn())
  {
    cerr << "---------------- cs_get_derived_plan EXECUTION PLAN ----------------" << endl;
    cerr << *csep << endl;
    cerr << "-------------- EXECUTION PLAN END --------------\n" << endl;
  }
  // Derived table projection and filter optimization.
  derivedTableOptimization(&gwi, csep);
  return 0;
}

int cs_get_select_plan(ha_columnstore_select_handler* handler, THD* thd, SCSEP& csep, gp_walk_info& gwi,
                       bool isSelectLexUnit)
{
  SELECT_LEX& select_lex = handler->select_lex ? *handler->select_lex : *handler->lex_unit->first_select();

  if (select_lex.where)
  {
    gwi.condList.push_back(select_lex.where);
  }

  buildTableOnExprList(&select_lex.top_join_list, gwi.tableOnExprList);

  convertOuterJoinToInnerJoin(&select_lex.top_join_list, gwi.tableOnExprList, gwi.condList,
                              handler->tableOuterJoinMap);

  int status = getSelectPlan(gwi, select_lex, csep, false, true, isSelectLexUnit);

  if (status > 0)
    return ER_INTERNAL_ERROR;
  else if (status < 0)
    return status;

  if (csep->traceOn())
  {
    cerr << "---------------- cs_get_select_plan EXECUTION PLAN ----------------" << endl;
    cerr << *csep << endl;
    cerr << "-------------- EXECUTION PLAN END --------------\n" << endl;
  }

  // Derived table projection and filter optimization.
  derivedTableOptimization(&gwi, csep);

  if (get_unstable_optimizer(thd))
  {
    optimizer::RBOptimizerContext ctx(gwi);
    bool csepWasOptimized = optimizer::optimizeCSEP(*csep, ctx);
    if (csep->traceOn() && csepWasOptimized)
    {
      cerr << "---------------- cs_get_select_plan optimized EXECUTION PLAN ----------------" << endl;
      cerr << *csep << endl;
      cerr << "-------------- EXECUTION PLAN END --------------\n" << endl;
    }
  }

  return 0;
}

}  // namespace cal_impl_if

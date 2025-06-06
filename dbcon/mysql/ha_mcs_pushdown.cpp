/*
   Copyright (c) 2019 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <typeinfo>
#include <string>

// This makes specific MDB classes' attributes public to implement
// MCOL-4740 temporary solution. Search for MCOL-4740
// to get the actual place where it is used.
#define updated_leaves \
  updated_leaves;      \
                       \
 public:

#include "ha_mcs_pushdown.h"

void update_counters_on_multi_update()
{
  if (ha_mcs_common::isMultiUpdateStatement(current_thd->lex->sql_command) &&
      !ha_mcs_common::isForeignTableUpdate(current_thd))
  {
    SELECT_LEX_UNIT* unit = &current_thd->lex->unit;
    SELECT_LEX* select_lex = unit->first_select();
    auto* multi = (select_lex->join) ? reinterpret_cast<multi_update*>(select_lex->join->result) : nullptr;

    if (multi)
    {
      multi->table_to_update = multi->update_tables ? multi->update_tables->table : 0;

      cal_impl_if::cal_connection_info* ci =
          reinterpret_cast<cal_impl_if::cal_connection_info*>(get_fe_conn_info_ptr());

      if (ci)
      {
        multi->updated = multi->found = ci->affectedRows;
      }
    }
  }
}

void check_walk(const Item* item, void* arg);

void restore_query_state(ha_columnstore_select_handler* handler)
{
  for (auto iter = handler->tableOuterJoinMap.begin(); iter != handler->tableOuterJoinMap.end(); iter++)
  {
    iter->first->outer_join = iter->second;
  }
}

void mutate_optimizer_flags(THD* thd_)
{
  // MCOL-2178 Disable all optimizer flags as it was in the fork.
  // CS restores it later in SH::scan_end() and in case of an error
  // in SH::scan_init()

  ulonglong flags_to_set = OPTIMIZER_SWITCH_MATERIALIZATION | OPTIMIZER_SWITCH_COND_PUSHDOWN_FOR_DERIVED |
                           OPTIMIZER_SWITCH_COND_PUSHDOWN_FROM_HAVING;

  if (thd_->variables.optimizer_switch == flags_to_set)
    return;

  set_original_optimizer_flags(thd_->variables.optimizer_switch, thd_);
  thd_->variables.optimizer_switch = flags_to_set;
}

void restore_optimizer_flags(THD* thd_)
{
  // MCOL-2178 restore original optimizer flags after SH, DH
  ulonglong orig_flags = get_original_optimizer_flags(thd_);
  if (orig_flags)
  {
    thd_->variables.optimizer_switch = orig_flags;
    set_original_optimizer_flags(0, thd_);
  }
}

/*@brief  find_tables - This traverses Item              */
/**********************************************************
 * DESCRIPTION:
 * This f() pushes TABLE of an Item_field to a list
 * provided. The list is used for JOIN predicate check in
 * is_joinkeys_predicate().
 * PARAMETERS:
 *    Item * Item to check
 * RETURN:
 ***********************************************************/
void find_tables(const Item* item, void* arg)
{
  if (typeid(*item) == typeid(Item_field))
  {
    Item_field* ifp = (Item_field*)item;
    List<TABLE>* tables_list = (List<TABLE>*)arg;
    tables_list->push_back(ifp->field->table);
  }
}

/*@brief  is_joinkeys_predicate - This traverses Item_func*/
/***********************************************************
 * DESCRIPTION:
 * This f() walks Item_func and checks whether it contains
 * JOIN predicate
 * PARAMETERS:
 *    Item_func * Item to walk
 * RETURN:
 *    BOOL false if Item_func isn't a JOIN predicate
 *    BOOL true otherwise
 ***********************************************************/
bool is_joinkeys_predicate(const Item_func* ifp)
{
  bool result = false;
  if (ifp->argument_count() == 2)
  {
    if (ifp->arguments()[0]->type() == Item::FIELD_ITEM && ifp->arguments()[1]->type() == Item::FIELD_ITEM)
    {
      Item_field* left = reinterpret_cast<Item_field*>(ifp->arguments()[0]);
      Item_field* right = reinterpret_cast<Item_field*>(ifp->arguments()[1]);

      // If MDB crashes here with non-fixed Item_field and field == NULL
      // there must be a check over on_expr for a different SELECT_LEX.
      // e.g. checking subquery with ON from upper query.
      if (left->field->table != right->field->table)
      {
        result = true;
      }
    }
    else
    {
      List<TABLE> llt;
      List<TABLE> rlt;
      Item* left = ifp->arguments()[0];
      Item* right = ifp->arguments()[1];
      // Search for tables inside left and right expressions
      // and compare them
      left->traverse_cond(find_tables, (void*)&llt, Item::POSTFIX);
      right->traverse_cond(find_tables, (void*)&rlt, Item::POSTFIX);
      // TODO Find the way to have more then one element or prove
      // the idea is useless.
      if (llt.elements && rlt.elements && (llt.elem(0) != rlt.elem(0)))
      {
        result = true;
      }
    }
  }
  return result;
}

/*@brief  find_nonequi_join - This traverses Item          */
/************************************************************
 * DESCRIPTION:
 * This f() walks Item and looks for a non-equi join
 * predicates
 * PARAMETERS:
 *    Item * Item to walk
 * RETURN:
 ***********************************************************/
void find_nonequi_join(const Item* item, void* arg)
{
  bool* unsupported_feature = reinterpret_cast<bool*>(arg);
  if (*unsupported_feature)
    return;

  if (item->type() == Item::FUNC_ITEM)
  {
    const Item_func* ifp = reinterpret_cast<const Item_func*>(item);
    // TODO Check for IN
    // NOT IN + correlated subquery
    if (ifp->functype() != Item_func::EQ_FUNC)
    {
      if (is_joinkeys_predicate(ifp))
        *unsupported_feature = true;
      else if (ifp->functype() == Item_func::NOT_FUNC && ifp->arguments()[0]->type() == Item::EXPR_CACHE_ITEM)
      {
        check_walk(ifp->arguments()[0], arg);
      }
    }
  }
}

/*@brief  find_join - This traverses Item                  */
/************************************************************
 * DESCRIPTION:
 * This f() walks traverses Item looking for JOIN, SEMI-JOIN
 * predicates.
 * PARAMETERS:
 *    Item * Item to traverse
 * RETURN:
 ***********************************************************/
void find_join(const Item* item, void* arg)
{
  bool* unsupported_feature = reinterpret_cast<bool*>(arg);
  if (*unsupported_feature)
    return;

  if (item->type() == Item::FUNC_ITEM)
  {
    const Item_func* ifp = reinterpret_cast<const Item_func*>(item);
    // TODO Check for IN
    // NOT IN + correlated subquery
    {
      if (is_joinkeys_predicate(ifp))
        *unsupported_feature = true;
      else if (ifp->functype() == Item_func::NOT_FUNC && ifp->arguments()[0]->type() == Item::EXPR_CACHE_ITEM)
      {
        check_walk(ifp->arguments()[0], arg);
      }
    }
  }
}

/*@brief  save_join_predicate - This traverses Item        */
/************************************************************
 * DESCRIPTION:
 * This f() walks Item and saves found JOIN predicates into
 * a List. The list will be used for a simple CROSS JOIN
 * check in create_DH.
 * PARAMETERS:
 *    Item * Item to walk
 * RETURN:
 ***********************************************************/
void save_join_predicates(const Item* item, void* arg)
{
  if (item->type() == Item::FUNC_ITEM)
  {
    const Item_func* ifp = reinterpret_cast<const Item_func*>(item);
    if (is_joinkeys_predicate(ifp))
    {
      List<Item>* join_preds_list = (List<Item>*)arg;
      join_preds_list->push_back(const_cast<Item*>(item));
    }
  }
}

/*@brief  check_walk - It traverses filter conditions      */
/************************************************************
 * DESCRIPTION:
 * It traverses filter predicates looking for unsupported
 * JOIN types: non-equi JOIN, e.g  t1.c1 > t2.c2;
 * logical OR.
 * PARAMETERS:
 *    thd - THD pointer.
 *    derived - TABLE_LIST* to work with.
 * RETURN:
 ***********************************************************/
void check_walk(const Item* item, void* arg)
{
  bool* unsupported_feature = reinterpret_cast<bool*>(arg);
  if (*unsupported_feature)
    return;

  switch (item->type())
  {
    case Item::FUNC_ITEM:
    {
      find_nonequi_join(item, arg);
      break;
    }
    case Item::EXPR_CACHE_ITEM:  // IN + correlated subquery
    {
      const Item_cache_wrapper* icw = reinterpret_cast<const Item_cache_wrapper*>(item);
      if (icw->get_orig_item()->type() == Item::FUNC_ITEM)
      {
        const Item_func* ifp = reinterpret_cast<const Item_func*>(icw->get_orig_item());
        if (ifp->argument_count() == 2 && (ifp->arguments()[0]->type() == Item::Item::SUBSELECT_ITEM ||
                                           ifp->arguments()[1]->type() == Item::Item::SUBSELECT_ITEM))
        {
          *unsupported_feature = true;
          return;
        }
      }
      break;
    }
    case Item::COND_ITEM:  // OR contains JOIN conds thats is unsupported yet
    {
      Item_cond* icp = (Item_cond*)item;
      if (is_cond_or(icp))
      {
        bool left_flag = false, right_flag = false;
        if (icp->argument_list()->elements >= 2)
        {
          Item* left;
          Item* right;
          List_iterator<Item> li(*icp->argument_list());
          left = li++;
          right = li++;
          left->traverse_cond(find_join, (void*)&left_flag, Item::POSTFIX);
          right->traverse_cond(find_join, (void*)&right_flag, Item::POSTFIX);
          if (left_flag && right_flag)
          {
            *unsupported_feature = true;
          }
        }
      }
      break;
    }
    default:
    {
      break;
    }
  }
}

/*@brief  check_user_var_func - This traverses Item        */
/************************************************************
 * DESCRIPTION:
 * This f() walks Item looking for the existence of
 * "set_user_var" or "get_user_var" functions.
 * PARAMETERS:
 *    Item * Item to traverse
 * RETURN:
 ***********************************************************/
void check_user_var_func(const Item* item, void* arg)
{
  bool* unsupported_feature = reinterpret_cast<bool*>(arg);

  if (*unsupported_feature)
    return;

  if (item->type() == Item::FUNC_ITEM)
  {
    const Item_func* ifp = reinterpret_cast<const Item_func*>(item);
    std::string funcname = ifp->func_name();
    if (funcname == "set_user_var")
    {
      *unsupported_feature = true;
    }
  }
}

void item_check(Item* item, bool* unsupported_feature)
{
  switch (item->type())
  {
    case Item::COND_ITEM:
    {
      item->traverse_cond(check_user_var_func, unsupported_feature, Item::POSTFIX);
      break;
    }
    case Item::FUNC_ITEM:
    {
      item->traverse_cond(check_user_var_func, unsupported_feature, Item::POSTFIX);
      break;
    }
    default:
    {
      item->traverse_cond(check_user_var_func, unsupported_feature, Item::POSTFIX);
      break;
    }
  }
}

bool check_user_var(SELECT_LEX* select_lex)
{
  if (!select_lex) {
    // There are definitely no user vars if select_lex is null
    return false;
  }
  List_iterator_fast<Item> it(select_lex->item_list);
  Item* item;
  bool is_user_var_func = false;

  while ((item = it++))
  {
    item_check(item, &is_user_var_func);

    if (is_user_var_func)
    {
      return true;
    }
  }

  JOIN* join = select_lex->join;

  if (join->conds)
  {
    join->conds->traverse_cond(check_user_var_func, &is_user_var_func, Item::POSTFIX);
  }

  return is_user_var_func;
}

/*@brief  create_columnstore_derived_handler- Creates handler*/
/************************************************************
 * DESCRIPTION:
 * Creates a derived handler if there is no non-equi JOIN, e.g
 * t1.c1 > t2.c2 and logical OR in the filter predicates.
 * More details in server/sql/derived_handler.h
 * PARAMETERS:
 *    thd - THD pointer.
 *    derived - TABLE_LIST* to work with.
 * RETURN:
 *    derived_handler if possible
 *    NULL in other case
 ***********************************************************/
derived_handler* create_columnstore_derived_handler(THD* thd, TABLE_LIST* table_ptr)
{
  ha_columnstore_derived_handler* handler = NULL;

  // MCOL-2178 Disable SP support in the derived_handler for now
  // Check the session variable value to enable/disable use of
  // derived_handler
  if (!get_derived_handler(thd) || (thd->lex)->sphead)
  {
    return handler;
  }

  // Disable derived handler for prepared statements
  if (thd->stmt_arena && thd->stmt_arena->is_stmt_execute())
    return handler;

  // MCOL-1482 Disable derived_handler if the multi-table update
  // statement contains a non-columnstore table.
  if (ha_mcs_common::isUpdateHasForeignTable(thd))
  {
    return handler;
  }

  SELECT_LEX_UNIT* unit = table_ptr->derived;
  SELECT_LEX* sl = unit->first_select();

  bool unsupported_feature = false;

  // Impossible HAVING or WHERE
  if (unsupported_feature || sl->having_value == Item::COND_FALSE || sl->cond_value == Item::COND_FALSE)
  {
    unsupported_feature = true;
  }

  // JOIN expression from WHERE, ON expressions
  JOIN* join = sl->join;
  // TODO DRRTUY Make a proper tree traverse
  // To search for CROSS JOIN-s we use tree invariant
  // G(V,E) where [V] = [E]+1
  List<Item> join_preds_list;
  TABLE_LIST* tl;
  for (tl = sl->get_table_list(); !unsupported_feature && tl; tl = tl->next_local)
  {
    if (tl->where)
    {
      tl->where->traverse_cond(check_walk, &unsupported_feature, Item::POSTFIX);
      tl->where->traverse_cond(save_join_predicates, &join_preds_list, Item::POSTFIX);
    }

    // Looking for JOIN with ON expression through
    // TABLE_LIST in FROM until CS meets unsupported feature
    if (tl->on_expr)
    {
      tl->on_expr->traverse_cond(check_walk, &unsupported_feature, Item::POSTFIX);
      tl->on_expr->traverse_cond(save_join_predicates, &join_preds_list, Item::POSTFIX);
    }

    // Iterate and traverse through the item list and the JOIN cond
    // and do not create DH if the unsupported (set_user_var)
    // function is present.
    if (check_user_var(tl->select_lex))
    {
      return handler;
    }
  }

  if (!unsupported_feature && !join_preds_list.elements && join && join->conds)
  {
    join->conds->traverse_cond(check_walk, &unsupported_feature, Item::POSTFIX);
    join->conds->traverse_cond(save_join_predicates, &join_preds_list, Item::POSTFIX);
  }

  // CROSS JOIN w/o conditions isn't supported until MCOL-301
  // is ready.
  if (!unsupported_feature && join && join->table_count >= 2 && !join_preds_list.elements)
  {
    unsupported_feature = true;
  }

  // CROSS JOIN with not enough JOIN predicates
  if (!unsupported_feature && join && join_preds_list.elements < join->table_count - 1)
  {
    unsupported_feature = true;
  }

  if (!unsupported_feature)
    handler = new ha_columnstore_derived_handler(thd, table_ptr);

  return handler;
}

/***********************************************************
 * DESCRIPTION:
 * derived_handler constructor
 * PARAMETERS:
 *    thd - THD pointer.
 *    tbl - tables involved.
 ***********************************************************/
ha_columnstore_derived_handler::ha_columnstore_derived_handler(THD* thd, TABLE_LIST* dt)
 : derived_handler(thd, mcs_hton)
{
  derived = dt;
  const char* timeZone = thd->variables.time_zone->get_name()->ptr();
  dataconvert::timeZoneToOffset(timeZone, strlen(timeZone), &time_zone);
}

/***********************************************************
 * DESCRIPTION:
 * derived_handler destructor
 ***********************************************************/
ha_columnstore_derived_handler::~ha_columnstore_derived_handler()
{
}

/*@brief  Initiate the query for derived_handler           */
/***********************************************************
 * DESCRIPTION:
 * Execute the query and saves derived table query.
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 ***********************************************************/
int ha_columnstore_derived_handler::init_scan()
{
  DBUG_ENTER("ha_columnstore_derived_handler::init_scan");

  mcs_handler_info mhi(reinterpret_cast<void*>(this), DERIVED);
  // this::table is the place for the result set
  int rc = ha_mcs_impl_pushdown_init(&mhi, table);

  DBUG_RETURN(rc);
}

/*@brief  Fetch next row for derived_handler               */
/***********************************************************
 * DESCRIPTION:
 * Fetches next row and saves it in the temp table
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 *
 ***********************************************************/
int ha_columnstore_derived_handler::next_row()
{
  DBUG_ENTER("ha_columnstore_derived_handler::next_row");

  int rc = ha_mcs_impl_rnd_next(table->record[0], table, time_zone);

  DBUG_RETURN(rc);
}

/*@brief  Finishes the scan and clean it up               */
/***********************************************************
 * DESCRIPTION:
 * Finishes the scan for derived handler
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 *
 ***********************************************************/
int ha_columnstore_derived_handler::end_scan()
{
  DBUG_ENTER("ha_columnstore_derived_handler::end_scan");

  int rc = ha_mcs_impl_rnd_end(table, true);

  DBUG_RETURN(rc);
}

void ha_columnstore_derived_handler::print_error(int, unsigned long)
{
}

/*@brief  create_columnstore_select_handler_- Creates handler
************************************************************
* DESCRIPTION:
* Creates a select handler if there is no non-equi JOIN, e.g
* t1.c1 > t2.c2 and logical OR in the filter predicates.
* More details in server/sql/select_handler.h
* PARAMETERS:
*    thd - THD pointer.
*    sel_lex - SELECT_LEX* that describes the query.
*    sel_unit - SELECT_LEX_UNIT* that describes the query.
* RETURN:
*    select_handler if possible
*    NULL in other case
***********************************************************/
select_handler* create_columnstore_select_handler_(THD* thd, SELECT_LEX* sel_lex, SELECT_LEX_UNIT* sel_unit)
{
  mcs_select_handler_mode_t select_handler_mode = get_select_handler_mode(thd);

  // Check the session variable value to enable/disable use of
  // select_handler
  if ((select_handler_mode == mcs_select_handler_mode_t::OFF) ||
      ((thd->lex)->sphead && !get_select_handler_in_stored_procedures(thd)))
  {
    return nullptr;
  }

  // MCOL-1482 Disable select_handler for a multi-table update
  // with a non-columnstore table as the target table of the update
  // operation.
  if (ha_mcs_common::isForeignTableUpdate(thd))
  {
    return nullptr;
  }

  // Flag to indicate if this is a prepared statement
  bool isPS = thd->stmt_arena && thd->stmt_arena->is_stmt_execute();

  // Disable processing of select_result_interceptor classes
  // which intercept and transform result set rows. E.g.:
  // select a,b into @a1, @a2 from t1;
  select_dumpvar* dumpvar = dynamic_cast<select_dumpvar*>((thd->lex)->result);
  if (dumpvar && !dumpvar->var_list.is_empty() && !isPS)
  {
    return nullptr;
  }

  // Select_handler couldn't properly process UPSERT..SELECT
  if ((thd->lex)->sql_command == SQLCOM_INSERT_SELECT && thd->lex->duplicates == DUP_UPDATE)
  {
    return nullptr;
  }

  // MCOL-5432 Disable partial pushdown of the UNION operation if the query
  // involves an order by or a limit clause.
  if (sel_lex && sel_unit &&
      (sel_unit->global_parameters()->limit_params.explicit_limit == true ||
       sel_unit->global_parameters()->order_list.elements != 0))
  {
    return nullptr;
  }

  std::vector<SELECT_LEX*> select_lex_vec;

  if (sel_unit && !sel_lex)
  {
    for (SELECT_LEX* sl = sel_unit->first_select(); sl; sl = sl->next_select())
    {
      select_lex_vec.push_back(sl);
    }
  }
  else
  {
    select_lex_vec.push_back(sel_lex);
  }

  for (size_t i = 0; i < select_lex_vec.size(); i++)
  {
    SELECT_LEX* select_lex = select_lex_vec[i];

    // Iterate and traverse through the item list and the JOIN cond
    // and do not create SH if the unsupported (set_user_var)
    // function is present.
    TABLE_LIST* table_ptr = select_lex->get_table_list();
    for (; table_ptr; table_ptr = table_ptr->next_global)
    {
      if (check_user_var(table_ptr->select_lex))
      {
        return nullptr;
      }
    }
  }

  // We apply dedicated rewrites from MDB here so MDB's data structures
  // becomes dirty and CS has to raise an error in case of any problem
  // or unsupported feature.
  ha_columnstore_select_handler* handler;

  if (sel_unit && sel_lex)  // partial pushdown of the SELECT_LEX_UNIT
  {
    handler = new ha_columnstore_select_handler(thd, sel_lex, sel_unit);
  }
  else if (sel_unit)  // complete pushdown of the SELECT_LEX_UNIT
  {
    handler = new ha_columnstore_select_handler(thd, sel_unit);
  }
  else  // Query only has a SELECT_LEX, no SELECT_LEX_UNIT
  {
    handler = new ha_columnstore_select_handler(thd, sel_lex);
  }

  for (size_t i = 0; i < select_lex_vec.size(); i++)
  {
    SELECT_LEX* select_lex = select_lex_vec[i];
    JOIN* join = select_lex->join;

    if (select_lex->first_cond_optimization && select_lex->handle_derived(thd->lex, DT_MERGE))
    {
      if (!thd->is_error())
      {
        my_printf_error(ER_INTERNAL_ERROR, "%s", MYF(0), "Error occurred in select_lex::handle_derived()");
      }

      return handler;
    }

    // This is partially taken from JOIN::optimize_inner() in sql/sql_select.cc
    if (select_lex->first_cond_optimization)
    {
      create_explain_query_if_not_exists(thd->lex, thd->mem_root);
      Query_arena *arena, backup;
      arena = thd->activate_stmt_arena_if_needed(&backup);
      COND* conds = join ? join->conds : nullptr;
      select_lex->where = conds;

      if (isPS)
      {
        select_lex->prep_where = conds ? conds->copy_andor_structure(thd) : 0;
      }

      // no join indicates that there is no tables to update
      if (join)
      {
        select_lex->update_used_tables();
      }

      if (arena)
        thd->restore_active_arena(arena, &backup);

#ifdef DEBUG_WALK_COND
      if (conds)
      {
        conds->traverse_cond(cal_impl_if::debug_walk, NULL, Item::POSTFIX);
      }
#endif
    }
  }

  // Attempt to execute the query using the select handler.
  // If query execution fails and columnstore_select_handler=AUTO,
  // fallback to table mode.
  // Skip execution for EXPLAIN queries
  if (!thd->lex->describe)
  {
    for (size_t i = 0; i < select_lex_vec.size(); i++)
    {
      SELECT_LEX* select_lex = select_lex_vec[i];
      JOIN* join = select_lex->join;

      // This is taken from JOIN::optimize()
      if (join)
        join->fields = &select_lex->item_list;

      // Instantiate handler::table, which is the place for the result set.
      if ((i == 0) && handler->prepare())
      {
        // check fallback
        if (select_handler_mode == mcs_select_handler_mode_t::AUTO)  // columnstore_select_handler=AUTO
        {
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 9999,
                       "MCS select_handler execution failed. Falling back to server execution");
          restore_query_state(handler);
          delete handler;
          return nullptr;
        }

        // error out
        if (!thd->is_error())
        {
          my_printf_error(ER_INTERNAL_ERROR, "%s", MYF(0), "Error occurred in handler->prepare()");
        }

        return handler;
      }

      // Prepare query execution
      // This is taken from JOIN::exec_inner()
      if (!select_lex->outer_select() &&                             // (1)
          select_lex != select_lex->master_unit()->fake_select_lex)  // (2)
        thd->lex->set_limit_rows_examined();

      if ((!sel_unit || sel_lex) && !join->tables_list && (join->table_count || !select_lex->with_sum_func) &&
          !select_lex->have_window_funcs())
      {
        if (!thd->is_error())
        {
          restore_query_state(handler);
          delete handler;
          return nullptr;
        }

        return handler;
      }

      if (join && !join->zero_result_cause && join->exec_const_cond && !join->exec_const_cond->val_int())
        join->zero_result_cause = "Impossible WHERE noticed after reading const tables";

      // We've called exec_const_cond->val_int(). This may have caused an error.
      if (unlikely(thd->is_error()))
      {
        // error out
        handler->pushdown_init_rc = 1;
        return handler;
      }

      if (join && join->zero_result_cause)
      {
        if (join->select_lex->have_window_funcs() && join->send_row_on_empty_set())
        {
          join->const_tables = join->table_count;
          join->first_select = sub_select_postjoin_aggr;
        }
        else
        {
          if (!thd->is_error())
          {
            restore_query_state(handler);
            delete handler;
            return nullptr;
          }

          return handler;
        }
      }

      if (join && (join->select_lex->options & OPTION_SCHEMA_TABLE) &&
          get_schema_tables_result(join, PROCESSED_BY_JOIN_EXEC))
      {
        if (!thd->is_error())
        {
          my_printf_error(ER_INTERNAL_ERROR, "%s", MYF(0), "Error occurred in get_schema_tables_result()");
        }

        return handler;
      }
    }

    handler->scan_initialized = true;
    mcs_handler_info mhi(reinterpret_cast<void*>(handler), SELECT);

    bool isSelectLexUnit = (sel_unit && !sel_lex) ? true : false;

    if ((handler->pushdown_init_rc = ha_mcs_impl_pushdown_init(&mhi, handler->table, isSelectLexUnit)))
    {
      // check fallback
      if (select_handler_mode == mcs_select_handler_mode_t::AUTO)
      {
        restore_query_state(handler);
        std::ostringstream oss;
        oss << "MCS select_handler execution failed";

        if (thd->is_error())
        {
          oss << " due to: ";
          oss << thd->get_stmt_da()->sql_errno() << ": ";
          oss << thd->get_stmt_da()->message();
          oss << " F";
          thd->clear_error();
        }
        else
        {
          oss << ", f";
        }

        oss << "alling back to server execution";
        thd->get_stmt_da()->clear_warning_info(thd->query_id);
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 9999, oss.str().c_str());
        delete handler;
        return nullptr;
      }

      if (!thd->is_error())
      {
        my_printf_error(ER_INTERNAL_ERROR, "%s", MYF(0), "Error occurred in ha_mcs_impl_pushdown_init()");
      }

      // We had an error in `ha_mcs_impl_pushdown_init`, no need to continue execution of this query.
      return handler;
    }

    for (size_t i = 0; i < select_lex_vec.size(); i++)
    {
      SELECT_LEX* select_lex = select_lex_vec[i];

      // Unset select_lex::first_cond_optimization
      if (select_lex->first_cond_optimization)
      {
        first_cond_optimization_flag_toggle(select_lex, &first_cond_optimization_flag_unset);
      }
    }
  }

  return handler;
}

select_handler* create_columnstore_select_handler(THD* thd, SELECT_LEX* select_lex, SELECT_LEX_UNIT* sel_unit)
{
  return create_columnstore_select_handler_(thd, select_lex, sel_unit);
}

select_handler* create_columnstore_unit_handler(THD* thd, SELECT_LEX_UNIT* sel_unit)
{
  if (thd->lex->sql_command == SQLCOM_CREATE_VIEW)
  {
    return nullptr;
  }

  if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare())
  {
    return nullptr;
  }

  // MCOL-5432 Disable UNION pushdown if the query involves an order by
  // or a limit clause.
  if (sel_unit->global_parameters()->limit_params.explicit_limit == true ||
      sel_unit->global_parameters()->order_list.elements != 0)
  {
    return nullptr;
  }

  return create_columnstore_select_handler_(thd, 0, sel_unit);
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * PARAMETERS:
 *    thd - THD pointer.
 *    sel_lex - semantic tree for the query.
 ***********************************************************/
ha_columnstore_select_handler::ha_columnstore_select_handler(THD* thd, SELECT_LEX* sel_lex)
 : select_handler(thd, mcs_hton, sel_lex)
 , prepared(false)
 , scan_ended(false)
 , scan_initialized(false)
 , pushdown_init_rc(0)
{
  const char* timeZone = thd->variables.time_zone->get_name()->ptr();
  dataconvert::timeZoneToOffset(timeZone, strlen(timeZone), &time_zone);
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * PARAMETERS:
 *    thd - THD pointer.
 *    sel_unit - semantic tree for the query.
 ***********************************************************/
ha_columnstore_select_handler::ha_columnstore_select_handler(THD* thd, SELECT_LEX_UNIT* sel_unit)
 : select_handler(thd, mcs_hton, sel_unit)
 , prepared(false)
 , scan_ended(false)
 , scan_initialized(false)
 , pushdown_init_rc(0)
{
  const char* timeZone = thd->variables.time_zone->get_name()->ptr();
  dataconvert::timeZoneToOffset(timeZone, strlen(timeZone), &time_zone);
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 * PARAMETERS:
 *    thd - THD pointer.
 *    sel_lex - semantic tree for the query.
 *    sel_unit - unit containing SELECT_LEX's
 ***********************************************************/
ha_columnstore_select_handler::ha_columnstore_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                                             SELECT_LEX_UNIT* sel_unit)
 : select_handler(thd, mcs_hton, sel_lex, sel_unit)
 , prepared(false)
 , scan_ended(false)
 , scan_initialized(false)
 , pushdown_init_rc(0)
{
  const char* timeZone = thd->variables.time_zone->get_name()->ptr();
  dataconvert::timeZoneToOffset(timeZone, strlen(timeZone), &time_zone);
}

/***********************************************************
 * DESCRIPTION:
 * select_handler constructor
 ***********************************************************/
ha_columnstore_select_handler::~ha_columnstore_select_handler()
{
  if (scan_initialized && !scan_ended)
  {
    end_scan();
  }
}

/*@brief  Initiate the query for select_handler           */
/***********************************************************
 * DESCRIPTION:
 * Execute the query and saves select table query.
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 ***********************************************************/
int ha_columnstore_select_handler::init_scan()
{
  DBUG_ENTER("ha_columnstore_select_handler::init_scan");
  DBUG_RETURN(pushdown_init_rc);
}

/*@brief  Fetch next row for select_handler               */
/***********************************************************
 * DESCRIPTION:
 * Fetches next row and saves it in the temp table
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 *
 ***********************************************************/
int ha_columnstore_select_handler::next_row()
{
  DBUG_ENTER("ha_columnstore_select_handler::next_row");

  int rc = ha_mcs_impl_select_next(table->record[0], table, time_zone);

  DBUG_RETURN(rc);
}

/*@brief  Finishes the scan and clean it up               */
/***********************************************************
 * DESCRIPTION:
 * Finishes the scan for select handler
 * PARAMETERS:
 *
 * RETURN:
 *    rc as int
 *
 ***********************************************************/
int ha_columnstore_select_handler::end_scan()
{
  DBUG_ENTER("ha_columnstore_select_handler::end_scan");

  scan_ended = true;

  // MCOL-4740 multi_update::send_eof(), which outputs the affected
  // number of rows to the client, is called after handler::rnd_end().
  // So we set multi_update::updated and multi_update::found here.
  update_counters_on_multi_update();

  int rc = ha_mcs_impl_rnd_end(table, true);

  DBUG_RETURN(rc);
}

// Copy of select_handler::prepare (see sql/select_handler.cc),
// with an added if guard
bool ha_columnstore_select_handler::prepare()
{
  DBUG_ENTER("ha_columnstore_select_handler::prepare");

  if (prepared)
    DBUG_RETURN(pushdown_init_rc ? true : false);

  prepared = true;

  if ((!table && !(table = create_tmp_table(thd))) || table->fill_item_list(&result_columns))
  {
    pushdown_init_rc = 1;
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);
}

#define PREFER_MY_CONFIG_H
#include <my_config.h>
// #include <cmath>
#include <iostream>
#include <sstream>
using namespace std;

#include "idb_mysql.h"

#include "errorids.h"
#include "idberrorinfo.h"
#include "exceptclasses.h"
using namespace logging;

#include "pseudocolumn.h"
#include "functioncolumn.h"
#include "constantcolumn.h"
using namespace execplan;

#include "functor.h"
#include "functor_str.h"

#include "ha_mcs.h"
#include "ha_mcs_impl_if.h"
#include "ha_mcs_sysvars.h"
using namespace cal_impl_if;

namespace
{
/*******************************************************************************
 * Pseudo column connector interface
 *
 * idbdbroot
 * idbpm
 * idbextentrelativerid
 * idbsegmentdir
 * idbsegment
 * idbpartition
 * idbextentmin
 * idbextentmax
 * idbextentid
 * idbblockid
 *
 * All the pseudo column functions are only executed in InfiniDB.
 */

void bailout(char* error, const string& funcName)
{
  string errMsg = IDBErrorInfo::instance()->errorMsg(ERR_PSEUDOCOL_IDB_ONLY, funcName);
  current_thd->get_stmt_da()->set_overwrite_status(true);
  current_thd->raise_error_printf(ER_INTERNAL_ERROR, errMsg.c_str());
  *error = 1;
}

int64_t idblocalpm()
{
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  if (ci->localPm == -1)
  {
    string module = ClientRotator::getModule();

    if (module.size() >= 3 && (module[0] == 'p' || module[0] == 'P'))
      ci->localPm = atol(module.c_str() + 2);
    else
      ci->localPm = 0;
  }

  return ci->localPm;
}

extern "C"
{
  /**
   * IDBDBROOT
   */
  my_bool idbdbroot_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbdbroot() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbdbroot_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbdbroot(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbdbroot");
    return 0;
  }

  /**
   * IDBPM
   */

  my_bool idbpm_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbpm() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbpm_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbpm(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbpm");
    return 0;
  }

  /**
   * IDBEXTENTRELATIVERID
   */

  my_bool idbextentrelativerid_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbextentrelativerid() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbextentrelativerid_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbextentrelativerid(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbextentrelativerid");
    return 0;
  }

  /**
   * IDBBLOCKID
   */

  my_bool idbblockid_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbblockid() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbblockid_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbblockid(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbblockid");
    return 0;
  }

  /**
   * IDBEXTENTID
   */

  my_bool idbextentid_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbextentid() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbextentid_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbextentid(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbextentid");
    return 0;
  }

  /**
   * IDBSEGMENT
   */

  my_bool idbsegment_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbsegment() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbsegment_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbsegment(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbsegment");
    return 0;
  }

  /**
   * IDBSEGMENTDIR
   */

  my_bool idbsegmentdir_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbsegmentdir() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbsegmentdir_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idbsegmentdir(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    bailout(error, "idbsegmentdir");
    return 0;
  }

  /**
   * IDBPARTITION
   */

  my_bool idbpartition_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbpartition() requires one argument");
      return 1;
    }

    return 0;
  }

  void idbpartition_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* idbpartition(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* /*length*/,
                           char* /*is_null*/, char* error)
  {
    bailout(error, "idbpartition");
    return result;
  }

  /**
   * IDBEXTENTMIN
   */

  my_bool idbextentmin_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbpm() requires one argument");
      return 1;
    }

    initid->maybe_null = 1;
    return 0;
  }

  void idbextentmin_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* idbextentmin(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* /*length*/,
                           char* /*is_null*/, char* error)
  {
    bailout(error, "idbextentmin");
    return result;
  }

  /**
   * IDBEXTENTMAX
   */

  my_bool idbextentmax_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 1)
    {
      strcpy(message, "idbextentmax() requires one argument");
      return 1;
    }

    initid->maybe_null = 1;
    return 0;
  }

  void idbextentmax_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* idbextentmax(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* /*length*/,
                           char* /*is_null*/, char* error)
  {
    bailout(error, "idbextentmax");
    return result;
  }

  /**
   * IDBLOCALPM
   */

  my_bool idblocalpm_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 0)
    {
      strcpy(message, "idblocalpm() should take no argument");
      return 1;
    }

    initid->maybe_null = 1;
    return 0;
  }

  void idblocalpm_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long idblocalpm(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* is_null, char* /*error*/)
  {
    longlong localpm = idblocalpm();

    if (localpm == 0)
      *is_null = 1;

    return localpm;
  }
}

}  // namespace

namespace cal_impl_if
{
ReturnedColumn* nullOnError(gp_walk_info& gwi, string& funcName)
{
  gwi.fatalParseError = true;
  gwi.parseErrorText =
      logging::IDBErrorInfo::instance()->errorMsg(logging::ERR_PSEUDOCOL_WRONG_ARG, funcName);
  return NULL;
}

uint32_t isPseudoColumn(string funcName)
{
  return execplan::PseudoColumn::pseudoNameToType(funcName);
}

execplan::ReturnedColumn* buildPseudoColumn(Item* item, gp_walk_info& gwi, bool& /*nonSupport*/,
                                            uint32_t pseudoType)
{
  if (get_fe_conn_info_ptr() == NULL)
  {
    set_fe_conn_info_ptr((void*)new cal_connection_info());
    thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
  }

  cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());

  Item_func* ifp = (Item_func*)item;

  // idblocalpm is replaced by constant
  if (pseudoType == PSEUDO_LOCALPM)
  {
    int64_t localPm = idblocalpm();
    ConstantColumn* cc;

    if (localPm)
      cc = new ConstantColumn(localPm);
    else
      cc = new ConstantColumn("", ConstantColumn::NULLDATA);
    cc->timeZone(gwi.timeZone);

    cc->alias(ifp->full_name() ? ifp->full_name() : "");
    return cc;
  }

  // convert udf item to pseudocolumn item.
  // adjust result type
  // put arg col to column map
  string funcName = ifp->func_name();

  if (ifp->argument_count() != 1 || !(ifp->arguments()) || !(ifp->arguments()[0]) ||
      ifp->arguments()[0]->type() != Item::FIELD_ITEM)
    return nullOnError(gwi, funcName);

  Item_field* field = (Item_field*)(ifp->arguments()[0]);

  // @todo rule out derive table
  if (!field->field || !field->db_name.str || strlen(field->db_name.str) == 0)
    return nullOnError(gwi, funcName);

  SimpleColumn* sc = buildSimpleColumn(field, gwi);

  if (!sc)
    return nullOnError(gwi, funcName);

  if ((pseudoType == PSEUDO_EXTENTMIN || pseudoType == PSEUDO_EXTENTMAX) &&
      (sc->colType().colDataType == CalpontSystemCatalog::VARBINARY ||
       (sc->colType().colDataType == CalpontSystemCatalog::TEXT) ||
       (sc->colType().colDataType == CalpontSystemCatalog::VARCHAR && sc->colType().colWidth > 7) ||
       (sc->colType().colDataType == CalpontSystemCatalog::CHAR && sc->colType().colWidth > 8)))
    return nullOnError(gwi, funcName);

  // put arg col to column map
  if (gwi.clauseType == SELECT || gwi.clauseType == GROUP_BY)  // select clause
  {
    SRCP srcp(sc);
    gwi.columnMap.insert(CalpontSelectExecutionPlan::ColumnMap::value_type(sc->columnName(), srcp));
    gwi.tableMap[make_aliastable(sc->schemaName(), sc->tableName(), sc->tableAlias(), sc->isColumnStore())] =
        make_pair(1, field->cached_table);
  }
  else if (!gwi.rcWorkStack.empty())
  {
    gwi.rcWorkStack.pop();
  }

  if (pseudoType == PSEUDO_PARTITION)
  {
    // parms: psueducolumn dbroot, segmentdir, segment
    SPTP sptp;
    FunctionColumn* fc = new FunctionColumn(funcName);
    funcexp::FunctionParm parms;
    PseudoColumn* dbroot = new PseudoColumn(*sc, PSEUDO_DBROOT);
    sptp.reset(new ParseTree(dbroot));
    parms.push_back(sptp);
    PseudoColumn* pp = new PseudoColumn(*sc, PSEUDO_SEGMENTDIR);
    sptp.reset(new ParseTree(pp));
    parms.push_back(sptp);
    PseudoColumn* seg = new PseudoColumn(*sc, PSEUDO_SEGMENT);
    sptp.reset(new ParseTree(seg));
    parms.push_back(sptp);
    fc->functionParms(parms);
    fc->expressionId(ci->expressionId++);
    fc->timeZone(gwi.timeZone);

    // string result type
    CalpontSystemCatalog::ColType ct;
    ct.colDataType = CalpontSystemCatalog::VARCHAR;
    ct.colWidth = 256;
    fc->resultType(ct);

    // operation type integer
    funcexp::Func_idbpartition* idbpartition = new funcexp::Func_idbpartition();
    fc->operationType(idbpartition->operationType(parms, fc->resultType()));
    fc->alias(ifp->full_name() ? ifp->full_name() : "");
    delete idbpartition;
    return fc;
  }

  PseudoColumn* pc = new PseudoColumn(*sc, pseudoType);

  // @bug5892. set alias for derived table column matching.
  pc->alias(ifp->name.length ? ifp->name.str : "");
  return pc;
}

}  // namespace cal_impl_if

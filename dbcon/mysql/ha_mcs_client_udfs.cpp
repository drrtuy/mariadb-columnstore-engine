/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (C) 2016-2022 MariaDB Corporation

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

#define NEED_CALPONT_INTERFACE
#define PREFER_MY_CONFIG_H 1
#include "ha_mcs_impl.h"

#include "ha_mcs_impl_if.h"
using namespace cal_impl_if;

#include "configcpp.h"
using namespace config;
#include "brmtypes.h"
using namespace BRM;
#include "bytestream.h"
using namespace messageqcpp;
#include "liboamcpp.h"
using namespace oam;
#include "cacheutils.h"

#include "errorcodes.h"
#include "idberrorinfo.h"
#include "errorids.h"
using namespace logging;

#include "ddlpkg.h"
#include "calpontsystemcatalog.h"

// #include "resourcemanager.h"

#include "columnstoreversion.h"
#include "ha_mcs_sysvars.h"
#include "basic/string_utils.h"

extern "C"
{
#define MAXSTRINGLENGTH 50

  const char* PmSmallSideMaxMemory = "pmmaxmemorysmallside";

  const char* SetParmsPrelude = "Updated ";
  const char* SetParmsError = "Invalid parameter: ";
  const char* InvalidParmSize = "Invalid parameter size: Input value cannot be larger than ";
  const char* MsgEMIndexSizeInitErrMsg = "mcs_emindex_size() takes no arguments";
  const char* MsgEMIndexFreeInitErrMsg = "mcs_emindex_free() takes no arguments";

  const size_t Plen = strlen(SetParmsPrelude);
  const size_t Elen = strlen(SetParmsError);

  const char* invalidParmSizeMessage(uint64_t size, size_t& len)
  {
    static char str[sizeof(InvalidParmSize) + 12] = {0};
    std::ostringstream os;
    os << InvalidParmSize << size;
    len = os.str().length();
    strcpy(str, os.str().c_str());
    return str;
  }

  my_bool setparms_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 2 || args->arg_type[0] != STRING_RESULT || args->arg_type[1] != STRING_RESULT)
    {
      sprintf(message, "%s() requires two string arguments", funcname);
      return 1;
    }

    initid->max_length = MAXSTRINGLENGTH;

    char valuestr[MAXSTRINGLENGTH];
    size_t vlen = args->lengths[1];

    memcpy(valuestr, args->args[1], vlen--);

    for (size_t i = 0; i < vlen; ++i)
      if (!isdigit(valuestr[i]))
      {
        sprintf(message, "%s() second argument must be numeric or end in G, M or K", funcname);
        return 1;
      }

    if (!isdigit(valuestr[vlen]))
    {
      switch (valuestr[vlen])
      {
        case 'G':
        case 'g':
        case 'M':
        case 'm':
        case 'K':
        case 'k':
        case '\0': break;

        default:
          sprintf(message, "%s() second argument must be numeric or end in G, M or K", funcname);
          return 1;
      }
    }

    return 0;
  }

  const char* mcssetparms(UDF_INIT* /*initid*/, UDF_ARGS* args, char* result, unsigned long* length,
                          char* /*is_null*/, char* /*error*/)
  {
    char parameter[MAXSTRINGLENGTH];
    char valuestr[MAXSTRINGLENGTH];
    size_t plen = args->lengths[0];
    size_t vlen = args->lengths[1];

    memcpy(parameter, args->args[0], plen);
    memcpy(valuestr, args->args[1], vlen);

    parameter[plen] = '\0';
    valuestr[vlen] = '\0';

    uint64_t value = Config::uFromText(valuestr);

    THD* thd = current_thd;
    uint32_t sessionID = execplan::CalpontSystemCatalog::idb_tid2sid(thd->thread_id);

    const char* msg = SetParmsError;
    size_t mlen = Elen;
    bool includeInput = true;

    std::string pstr(parameter);
    boost::algorithm::to_lower(pstr);

    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());
    idbassert(ci != 0);

    if (pstr == PmSmallSideMaxMemory)
    {
      joblist::ResourceManager* rm = joblist::ResourceManager::instance();

      if (rm->getHjTotalUmMaxMemorySmallSide() >= value)
      {
        ci->rmParms.push_back(execplan::RMParam(sessionID, execplan::PMSMALLSIDEMEMORY, value));

        msg = SetParmsPrelude;
        mlen = Plen;
      }
      else
      {
        msg = invalidParmSizeMessage(rm->getHjTotalUmMaxMemorySmallSide(), mlen);
        includeInput = false;
      }
    }

    memcpy(result, msg, mlen);

    if (includeInput)
    {
      memcpy(result + mlen, parameter, plen);
      mlen += plen;
      memcpy(result + mlen++, " ", 1);
      memcpy(result + mlen, valuestr, vlen);
      *length = mlen + vlen;
    }
    else
      *length = mlen;

    return result;
  }

  my_bool mcssetparms_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return setparms_init(initid, args, message, "MCSSETPARMS");
  }

  void mcssetparms_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* calsetparms(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                          char* is_null, char* error)
  {
    return mcssetparms(initid, args, result, length, is_null, error);
  }

  my_bool calsetparms_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return setparms_init(initid, args, message, "CALSETPARMS");
  }

  void calsetparms_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool getstats_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 0)
    {
      sprintf(message, "%s() takes no arguments", funcname);
      return 1;
    }

    initid->maybe_null = 1;
    initid->max_length = 255;

    return 0;
  }

  const char* mcsgetstats(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* length,
                          char* is_null, char* /*error*/)
  {
    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());

    unsigned long l = ci->queryStats.size();

    if (l == 0)
    {
      *is_null = 1;
      return 0;
    }

    if (l > 255)
      l = 255;

    memcpy(result, ci->queryStats.c_str(), l);
    *length = l;
    return result;
  }

  my_bool mcsgetstats_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getstats_init(initid, args, message, "MCSGETSTATS");
  }

  void mcsgetstats_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* calgetstats(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                          char* is_null, char* error)
  {
    return mcsgetstats(initid, args, result, length, is_null, error);
  }

  my_bool calgetstats_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getstats_init(initid, args, message, "CALGETSTATS");
  }

  void calgetstats_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool settrace_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 1 || args->arg_type[0] != INT_RESULT)
    {
      sprintf(message, "%s() requires one INTEGER argument", funcname);
      return 1;
    }

    return 0;
  }

  long long mcssettrace(UDF_INIT* /*initid*/, UDF_ARGS* args, char* /*is_null*/, char* /*error*/)
  {
    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());

    long long oldTrace = ci->traceFlags;
    ci->traceFlags = (uint32_t)(*((long long*)args->args[0]));
    // keep the vtablemode bit
    ci->traceFlags |= (oldTrace & execplan::CalpontSelectExecutionPlan::TRACE_TUPLE_OFF);
    ci->traceFlags |= (oldTrace & execplan::CalpontSelectExecutionPlan::TRACE_TUPLE_AUTOSWITCH);
    return oldTrace;
  }

  my_bool mcssettrace_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return settrace_init(initid, args, message, "MCSSETTRACE");
  }

  void mcssettrace_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long calsettrace(UDF_INIT* initid, UDF_ARGS* args, char* is_null, char* error)
  {
    return mcssettrace(initid, args, is_null, error);
  }

  my_bool calsettrace_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return settrace_init(initid, args, message, "CALSETTRACE");
  }

  void calsettrace_deinit(UDF_INIT* /*initid*/)
  {
  }

  // Return 1 if system is ready for reads or 0 if not.
  long long mcssystemready(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    long long rtn = 0;
    Oam oam;
    DBRM dbrm(true);

    try
    {
      if (dbrm.getSystemReady() > 0 && dbrm.getSystemQueryReady() > 0)
      {
        return 1;
      }
    }
    catch (...)
    {
      *error = 1;
    }

    return rtn;
  }

  my_bool mcssystemready_init(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*message*/)
  {
    return 0;
  }

  void mcssystemready_deinit(UDF_INIT* /*initid*/)
  {
  }

  // Return non-zero if system is read only; 0 if writeable
  long long mcssystemreadonly(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    long long rtn = 0;
    DBRM dbrm(true);

    try
    {
      if (dbrm.getSystemSuspended())
      {
        rtn = 1;
      }

      if (dbrm.isReadWrite() > 0)  // Returns 0 for writable, 5 for read only
      {
        rtn = 2;
      }
    }
    catch (...)
    {
      *error = 1;
      rtn = 1;
    }

    return rtn;
  }

  my_bool mcssystemreadonly_init(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*message*/)
  {
    return 0;
  }

  void mcssystemreadonly_deinit(UDF_INIT* /*initid*/)
  {
  }

  // Return non-zero if this is the primary UM; 0 if not primary
  long long mcssystemprimary(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* error)
  {
    long long rtn = 0;
    Oam oam;
    std::string PrimaryUMModuleName;
    std::string localModule;
    oamModuleInfo_t st;

    try
    {
      st = oam.getModuleInfo();
      localModule = boost::get<0>(st);
      PrimaryUMModuleName = config::Config::makeConfig()->getConfig("SystemConfig", "PrimaryUMModuleName");

      if (datatypes::ASCIIStringCaseInsensetiveEquals(localModule, PrimaryUMModuleName))
        rtn = 1;
      if (PrimaryUMModuleName == "unassigned")
        rtn = 1;
    }
    catch (std::runtime_error& e)
    {
      // It's difficult to return an error message from a numerical UDF
      // string msg = string("ERROR: Problem getting Primary UM Module Name. ") + e.what();
      *error = 1;
    }
    catch (...)
    {
      *error = 1;
    }
    return rtn;
  }

  my_bool mcssystemprimary_init(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*message*/)
  {
    return 0;
  }

  void mcssystemprimary_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool viewtablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count == 2 && (args->arg_type[0] != STRING_RESULT || args->arg_type[1] != STRING_RESULT))
    {
      sprintf(message, "%s() requires two string arguments", funcname);
      return 1;
    }
    else if ((args->arg_count == 1) && (args->arg_type[0] != STRING_RESULT))
    {
      sprintf(message, "%s() requires one string argument", funcname);
      return 1;
    }
    else if (args->arg_count > 2)
    {
      sprintf(message, "%s() takes one or two arguments only", funcname);
      return 1;
    }
    else if (args->arg_count == 0)
    {
      sprintf(message, "%s() requires at least one argument", funcname);
      return 1;
    }

    initid->maybe_null = 1;
    initid->max_length = 255;

    return 0;
  }

  my_bool mcsviewtablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return viewtablelock_init(initid, args, message, "MCSVIEWTABLELOCK");
  }

  const char* mcsviewtablelock(UDF_INIT* /*initid*/, UDF_ARGS* args, char* result, unsigned long* length,
                               char* /*is_null*/, char* /*error*/)
  {
    THD* thd = current_thd;

    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());
    execplan::CalpontSystemCatalog::TableName tableName;

    if (args->arg_count == 2)
    {
      tableName.schema = args->args[0];
      tableName.table = args->args[1];
    }
    else if (args->arg_count == 1)
    {
      tableName.table = args->args[0];

      if (thd->db.length)
      {
        tableName.schema = thd->db.str;
      }
      else
      {
        std::string msg("No schema information provided");
        memcpy(result, msg.c_str(), msg.length());
        *length = msg.length();
        return result;
      }
    }
    if (lower_case_table_names)
    {
      boost::algorithm::to_lower(tableName.schema);
      boost::algorithm::to_lower(tableName.table);
    }

    if (!ci->dmlProc)
    {
      ci->dmlProc = new MessageQueueClient("DMLProc");
      // cout << "viewtablelock starts a new client " << ci->dmlProc << " for session " << thd->thread_id <<
      // endl;
    }

    std::string lockinfo = ha_mcs_impl_viewtablelock(*ci, tableName);

    memcpy(result, lockinfo.c_str(), lockinfo.length());
    *length = lockinfo.length();
    return result;
  }

  void mcsviewtablelock_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool calviewtablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return viewtablelock_init(initid, args, message, "CALVIEWTABLELOCK");
  }

  const char* calviewtablelock(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                               char* is_null, char* error)
  {
    return mcsviewtablelock(initid, args, result, length, is_null, error);
  }

  void calviewtablelock_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool cleartablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if ((args->arg_count != 1) || (args->arg_type[0] != INT_RESULT))
    {
      sprintf(message, "%s() requires one integer argument (the lockID)", funcname);
      return 1;
    }

    initid->maybe_null = 1;
    initid->max_length = 255;

    return 0;
  }

  my_bool mcs_set_ddldebug_level_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if ((args->arg_count != 1) || (args->arg_type[0] != INT_RESULT) ||
        *reinterpret_cast<uint32_t*>(args->args[0]) > 3)
    {
      sprintf(message, "%s() requires one integer `debug level` ", funcname);
      return 1;
    }

    initid->maybe_null = 0;
    initid->max_length = 5;

    return 0;
  }

  const char* mcs_set_ddldebug_level(UDF_INIT* /*initid*/, UDF_ARGS* args, char* result,
                                     unsigned long* length)
  {
    uint32_t level = *reinterpret_cast<uint32_t*>(args->args[0]);
    std::unique_ptr<ddlpackage::DebugDDLStatement> stmt =
        std::make_unique<ddlpackage::DebugDDLStatement>(level);
    stmt->fSessionID = execplan::CalpontSystemCatalog::idb_tid2sid(current_thd->thread_id);
    MessageQueueClient mq("DDLProc");

    ByteStream bytestream;
    bytestream << (uint32_t)stmt->fSessionID;
    stmt->serialize(bytestream);
    ByteStream::byte b = 0;
    THD* thd = current_thd;
    string emsg;
    mq.write(bytestream);

    try
    {
      bytestream = mq.read();

      if (bytestream.length() == 0)
      {
        thd->get_stmt_da()->set_overwrite_status(true);
        thd->raise_error_printf(ER_INTERNAL_ERROR, "Lost connection to DDLProc");
        std::memcpy(result, "Error", 5);
        *length = 5;
        return result;
      }
      else
      {
        bytestream >> b;
        bytestream >> emsg;
      }
    }
    catch (runtime_error&)
    {
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->raise_error_printf(ER_INTERNAL_ERROR, "Lost connection to DDLProc");
      std::memcpy(result, "Error", 5);
      *length = 5;
      return result;
    }
    catch (...)
    {
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->raise_error_printf(ER_INTERNAL_ERROR, "Unknown error caught");
      std::memcpy(result, "Error", 5);
      *length = 5;
      return result;
    }

    *length = 2;
    std::memcpy(result, "Ok", 2);
    return result;
  }

  void mcs_set_ddldebug_level_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool mcscleartablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return cleartablelock_init(initid, args, message, "MCSCLEARTABLELOCK");
  }

  const char* mcscleartablelock(UDF_INIT* /*initid*/, UDF_ARGS* args, char* result, unsigned long* length,
                                char* /*is_null*/, char* /*error*/)
  {
    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());
    long long lockID = *reinterpret_cast<long long*>(args->args[0]);

    if (!ci->dmlProc)
    {
      ci->dmlProc = new MessageQueueClient("DMLProc");
      // cout << "cleartablelock starts a new client " << ci->dmlProc << " for session " << thd->thread_id <<
      // endl;
    }

    unsigned long long uLockID = lockID;
    std::string lockinfo = ha_mcs_impl_cleartablelock(*ci, uLockID);

    memcpy(result, lockinfo.c_str(), lockinfo.length());
    *length = lockinfo.length();
    return result;
  }

  void mcscleartablelock_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool calcleartablelock_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return cleartablelock_init(initid, args, message, "CALCLEARTABLELOCK");
  }

  const char* calcleartablelock(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                                char* is_null, char* error)
  {
    return mcscleartablelock(initid, args, result, length, is_null, error);
  }

  void calcleartablelock_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool lastinsertid_init(UDF_INIT* initid, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count == 2 && (args->arg_type[0] != STRING_RESULT || args->arg_type[1] != STRING_RESULT))
    {
      sprintf(message, "%s() requires two string arguments", funcname);
      return 1;
    }
    else if ((args->arg_count == 1) && (args->arg_type[0] != STRING_RESULT))
    {
      sprintf(message, "%s() requires one string argument", funcname);
      return 1;
    }
    else if (args->arg_count > 2)
    {
      sprintf(message, "%s() takes one or two arguments only", funcname);
      return 1;
    }
    else if (args->arg_count == 0)
    {
      sprintf(message, "%s() requires at least one argument", funcname);
      return 1;
    }

    initid->maybe_null = 1;
    initid->max_length = 255;

    return 0;
  }

  my_bool mcslastinsertid_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return lastinsertid_init(initid, args, message, "MCSLASTINSERTID");
  }

  long long mcslastinsertid(UDF_INIT* /*initid*/, UDF_ARGS* args, char* /*is_null*/, char* /*error*/)
  {
    THD* thd = current_thd;

    execplan::CalpontSystemCatalog::TableName tableName;
    uint64_t nextVal = 0;

    if (args->arg_count == 2)
    {
      tableName.schema = args->args[0];
      tableName.table = args->args[1];
    }
    else if (args->arg_count == 1)
    {
      tableName.table = args->args[0];

      if (thd->db.length)
        tableName.schema = thd->db.str;
      else
      {
        return -1;
      }
    }
    if (lower_case_table_names)
    {
      boost::algorithm::to_lower(tableName.schema);
      boost::algorithm::to_lower(tableName.table);
    }

    execplan::CalpontSystemCatalog csc;
    csc.identity(execplan::CalpontSystemCatalog::FE);

    try
    {
      nextVal = csc.nextAutoIncrValue(tableName);
    }
    catch (std::exception&)
    {
      std::string msg("No such table found during autincrement");
      setError(thd, ER_INTERNAL_ERROR, msg);
      return nextVal;
    }

    if (nextVal == AUTOINCR_SATURATED)
    {
      setError(thd, ER_INTERNAL_ERROR, IDBErrorInfo::instance()->errorMsg(ERR_EXCEED_LIMIT));
      return nextVal;
    }

    //@Bug 3559. Return a message for table without autoincrement column.
    if (nextVal == 0)
    {
      std::string msg("Autoincrement does not exist for this table.");
      setError(thd, ER_INTERNAL_ERROR, msg);
      return nextVal;
    }

    return (nextVal - 1);
  }

  void mcslastinsertid_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool callastinsertid_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return lastinsertid_init(initid, args, message, "CALLASTINSERTID");
  }

  long long callastinsertid(UDF_INIT* initid, UDF_ARGS* args, char* is_null, char* error)
  {
    return mcslastinsertid(initid, args, is_null, error);
  }

  void callastinsertid_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool flushcache_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 0)
    {
      sprintf(message, "%s() takes no arguments", funcname);
      return 1;
    }

    return 0;
  }

  void mcsflushcache_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long mcsflushcache(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* /*error*/)
  {
    return static_cast<long long>(cacheutils::flushPrimProcCache());
  }

  my_bool mcsflushcache_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return flushcache_init(initid, args, message, "MCSFLUSHCACHE");
  }

  void calflushcache_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long calflushcache(UDF_INIT* initid, UDF_ARGS* args, char* is_null, char* error)
  {
    return mcsflushcache(initid, args, is_null, error);
  }

  my_bool calflushcache_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return flushcache_init(initid, args, message, "CALFLUSHCACHE");
  }

  static const unsigned long TraceSize = 16 * 1024;

  // mysqld will call this with only 766 bytes available in result no matter what we asked for in
  // calgettrace_init()
  // if we return a pointer that is not result, mysqld will take our pointer and use it, freeing up result
  const char* mcsgettrace(UDF_INIT* /*initid*/, UDF_ARGS* args, char* /*result*/, unsigned long* length,
                          char* is_null, char* /*error*/)
  {
    const std::string* msgp;
    int flags = 0;

    if (args->arg_count > 0)
    {
      if (args->arg_type[0] == INT_RESULT)
      {
        flags = *reinterpret_cast<int*>(args->args[0]);
      }
    }

    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());

    if (flags > 0)
      // msgp = &connMap[sessionID].extendedStats;
      msgp = &ci->extendedStats;
    else
      // msgp = &connMap[sessionID].miniStats;
      msgp = &ci->miniStats;

    unsigned long l = msgp->size();

    if (l == 0)
    {
      *is_null = 1;
      return 0;
    }

    if (l > TraceSize)
      l = TraceSize;

    *length = l;
    return msgp->c_str();
  }

  my_bool gettrace_init(UDF_INIT* initid, UDF_ARGS* /*args*/, char* /*message*/, const char* /*funcname*/)
  {
#if 0

        if (args->arg_count != 0)
        {
            sprintf(message, "%s() takes no arguments", funcname);
            return 1;
        }

#endif
    initid->maybe_null = 1;
    initid->max_length = TraceSize;

    return 0;
  }

  my_bool mcsgettrace_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return gettrace_init(initid, args, message, "MCSGETTRACE");
  }

  void mcsgettrace_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* calgettrace(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                          char* is_null, char* error)
  {
    return mcsgettrace(initid, args, result, length, is_null, error);
  }

  my_bool calgettrace_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return gettrace_init(initid, args, message, "CALGETTRACE");
  }

  void calgettrace_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool getversion_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 0)
    {
      sprintf(message, "%s() takes no arguments", funcname);
      return 1;
    }

    return 0;
  }

  const char* mcsgetversion(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* length,
                            char* /*is_null*/, char* /*error*/)
  {
    std::string version(columnstore_version);
    *length = version.size();
    memcpy(result, version.c_str(), *length);
    return result;
  }

  my_bool mcsgetversion_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getversion_init(initid, args, message, "MCSGETVERSION");
  }

  void mcsgetversion_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* calgetversion(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                            char* is_null, char* error)
  {
    return mcsgetversion(initid, args, result, length, is_null, error);
  }

  my_bool calgetversion_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getversion_init(initid, args, message, "CALGETVERSION");
  }

  void calgetversion_deinit(UDF_INIT* /*initid*/)
  {
  }

  my_bool getsqlcount_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message, const char* funcname)
  {
    if (args->arg_count != 0)
    {
      sprintf(message, "%s() takes no arguments", funcname);
      return 1;
    }

    return 0;
  }

  const char* mcsgetsqlcount(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* result, unsigned long* length,
                             char* /*is_null*/, char* /*error*/)
  {
    if (get_fe_conn_info_ptr() == NULL)
    {
      set_fe_conn_info_ptr((void*)new cal_connection_info());
      thd_set_ha_data(current_thd, mcs_hton, get_fe_conn_info_ptr());
    }

    cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(get_fe_conn_info_ptr());
    idbassert(ci != 0);

    MessageQueueClient* mqc = 0;
    mqc = new MessageQueueClient("ExeMgr1");

    ByteStream msg;
    ByteStream::quadbyte runningSql, waitingSql;
    ByteStream::quadbyte qb = 5;
    msg << qb;
    mqc->write(msg);

    // get ExeMgr response
    msg.restart();
    msg = mqc->read();

    if (msg.length() == 0)
    {
      memcpy(result, "Lost connection to ExeMgr", *length);
      return result;
    }

    msg >> runningSql;
    msg >> waitingSql;
    delete mqc;

    char ans[128];
    sprintf(ans, "Running SQL statements %d, Waiting SQL statments %d", runningSql, waitingSql);
    *length = strlen(ans);
    memcpy(result, ans, *length);
    return result;
  }

  my_bool mcsgetsqlcount_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getstats_init(initid, args, message, "MCSGETSQLCOUNT");
  }

  void mcsgetsqlcount_deinit(UDF_INIT* /*initid*/)
  {
  }

  const char* calgetsqlcount(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* length,
                             char* is_null, char* error)
  {
    return mcsgetsqlcount(initid, args, result, length, is_null, error);
  }

  my_bool calgetsqlcount_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
  {
    return getstats_init(initid, args, message, "CALGETSQLCOUNT");
  }

  void calgetsqlcount_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long mcs_emindex_size(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* /*error*/)
  {
    DBRM dbrm;
    return dbrm.EMIndexShmemSize();
  }

  my_bool mcs_emindex_size_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 0)
    {
      strcpy(message, MsgEMIndexSizeInitErrMsg);
      return 1;
    }

    return 0;
  }

  void mcs_emindex_size_deinit(UDF_INIT* /*initid*/)
  {
  }

  long long mcs_emindex_free(UDF_INIT* /*initid*/, UDF_ARGS* /*args*/, char* /*is_null*/, char* /*error*/)
  {
    DBRM dbrm;
    return dbrm.EMIndexShmemFree();
  }

  my_bool mcs_emindex_free_init(UDF_INIT* /*initid*/, UDF_ARGS* args, char* message)
  {
    if (args->arg_count != 0)
    {
      strcpy(message, MsgEMIndexFreeInitErrMsg);
      return 1;
    }

    return 0;
  }

  void mcs_emindex_free_deinit(UDF_INIT* /*initid*/)
  {
  }

}  // extern "C"

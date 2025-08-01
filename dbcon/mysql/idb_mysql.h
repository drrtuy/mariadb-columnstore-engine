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

/* One include file to deal with all the MySQL pollution of the
   global namespace

   Don't include ANY mysql headers anywhere except here!

   WARN: if any cmake build target uses this include file,
   GenError from server must be added to the target dependencies
   to generate mysqld_error.h used below
*/

#pragma once

#ifdef TEST_MCSCONFIG_H
#error mcsconfig.h was included before idb_mysql.h
#endif

// #define INFINIDB_DEBUG
// #define DEBUG_WALK_COND

#define MYSQL_SERVER 1  // needed for definition of struct THD in mysql_priv.h
#define USE_CALPONT_REGEX

#undef LOG_INFO

#ifdef _DEBUG
#ifndef SAFE_MUTEX
#define SAFE_MUTEX
#endif
#ifndef SAFEMALLOC
#define SAFEMALLOC
#endif
#ifndef ENABLED_DEBUG_SYNC
#define ENABLED_DEBUG_SYNC
#endif

#define DBUG_ON 1
#undef DBUG_OFF
#else
#undef SAFE_MUTEX
#undef SAFEMALLOC
#undef ENABLED_DEBUG_SYNC
#undef DBUG_ON
#define DBUG_OFF 1
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "sql_plugin.h"
#include "sql_table.h"
#include "sql_select.h"
#include "mysqld_error.h"
#include "item_windowfunc.h"
#include "sql_cte.h"
#include "tztime.h"
#include "derived_handler.h"
#include "select_handler.h"
#include "rpl_rli.h"
#include "my_dbug.h"
#include "sql_show.h"
#if MYSQL_VERSION_ID >= 110401
#include "opt_histogram_json.h"
#else
// Mock Histogram_bucket for MySQL < 11.4
struct Histogram_bucket
{
  std::string start_value;

  double cum_fract;

  longlong ndv;
};

class Histogram_json_hb
{
  std::vector<Histogram_bucket> buckets;

  std::string last_bucket_end_endp;

public:
  const std::vector<Histogram_bucket>& get_json_histogram() const
  {
    return buckets;
  }

  const std::string& get_last_bucket_end_endp() const
  {
    return last_bucket_end_endp;
  }
};
#endif
#pragma GCC diagnostic pop

// Now clean up the pollution as best we can...
#include "mcsconfig_conflicting_defs_undef.h"
#undef min
#undef max
#undef UNKNOWN
#undef test
#undef pthread_mutex_init
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_mutex_destroy
#undef pthread_mutex_wait
#undef pthread_mutex_timedwait
#undef pthread_mutex_t
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef pthread_mutex_trylock
#undef sleep
#undef getpid
#undef SIZEOF_LONG
#undef DEBUG
#undef set_bits
#undef likely

namespace
{
inline char* idb_mysql_query_str(THD* thd)
{
#if MYSQL_VERSION_ID >= 50172
  return thd->query();
#else
  return thd->query;
#endif
}
}  // namespace

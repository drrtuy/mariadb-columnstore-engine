/* Copyright (C) 2014 InfiniDB, Inc.
   Copyright (c) 2016-2020 MariaDB

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

// $Id: jobstep.cpp 9414 2013-04-22 22:18:30Z xlou $
#include <iostream>
#include <string>
using namespace std;

#include <stdlib.h>
#include <boost/thread.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
namespace bu = boost::uuids;

#include "configcpp.h"
using namespace config;

#include "calpontsystemcatalog.h"
#include "calpontselectexecutionplan.h"
#include "messagelog.h"
#include "messageids.h"
#include "timestamp.h"
#include "oamcache.h"
#include "jobstep.h"
#include "jlf_common.h"
using namespace logging;

#include "querytele.h"
using namespace querytele;

namespace
{
int toInt(const string& val)
{
  if (val.length() == 0)
    return -1;

  return static_cast<int>(config::Config::fromText(val));
}
}  // namespace

namespace joblist
{
boost::mutex JobStep::fLogMutex;  //=PTHREAD_MUTEX_INITIALIZER;

threadpool::ThreadPool JobStep::jobstepThreadPool(defaultJLThreadPoolSize, 0);

ostream& operator<<(ostream& os, const JobStep* rhs)
{
  os << rhs->toString();
  return os;
}

//------------------------------------------------------------------------------
// constructor
//------------------------------------------------------------------------------
JobStep::JobStep(const JobInfo& j)
 : fSessionId(j.sessionId)
 , fTxnId(j.txnId)
 , fVerId(j.verId)
 , fStatementId(j.statementId)
 , fStepId(0)
 , fTupleId(-1)
 , fTraceFlags(0)
 , fCardinality(0)
 , fDelayedRunFlag(false)
 , fDelivery(false)
 , fOnClauseFilter(false)
 , fDie(false)
 , fWaitToRunStepCnt(0)
 , fPriority(1)
 , fErrorInfo(j.errorInfo)
 , fLogger(j.logger)
 , fLocalQuery(j.localQuery)
 , fQueryUuid(j.uuid)
 , fProgress(0)
 , fStartTime(-1)
 , fTimeZone(j.timeZone)
 , fMaxPmJoinResultCount(j.maxPmJoinResultCount)
{
  QueryTeleServerParms tsp;
  string teleServerHost(Config::makeConfig()->getConfig("QueryTele", "Host"));

  if (!teleServerHost.empty())
  {
    int teleServerPort = toInt(Config::makeConfig()->getConfig("QueryTele", "Port"));

    if (teleServerPort > 0)
    {
      tsp.host = teleServerHost;
      tsp.port = teleServerPort;
    }
  }

  fQtc.serverParms(tsp);
  // fStepUuid = bu::random_generator()();
  fStepUuid = QueryTeleClient::genUUID();
}

//------------------------------------------------------------------------------
// Log a syslog msg for the start of this specified job step
//------------------------------------------------------------------------------
void JobStep::syslogStartStep(uint32_t subSystem, const string& stepName) const
{
  LoggingID logId(subSystem, sessionId(), txnId());
  MessageLog msgLog(logId);

  Message msgStartStep(M0030);
  Message::Args args;
  args.add((uint64_t)statementId());  // statement id for this job step
  args.add((int)stepId());            // step id for this job step
  args.add(stepName);                 // step name for this job step
  msgStartStep.format(args);
  msgLog.logDebugMessage(msgStartStep);
}

//------------------------------------------------------------------------------
// Log a syslog message for the end of this specified job step
//------------------------------------------------------------------------------
void JobStep::syslogEndStep(uint32_t subSystem, uint64_t blockedDLInput, uint64_t blockedDLOutput,
                            uint64_t msgBytesInput, uint64_t msgBytesOutput) const
{
  LoggingID logId(subSystem, sessionId(), txnId());
  MessageLog msgLog(logId);

  Message msgEndStep(M0031);
  Message::Args args;
  args.add((uint64_t)statementId());  // statement id for this job step
  args.add((int)stepId());            // step id for this job step
  args.add(blockedDLInput);           // blocked datalist input (ex: fifo)
  args.add(blockedDLOutput);          // blocked datalist output(ex: fifo)
  args.add(msgBytesInput);            // incoming msg byte count
  args.add(msgBytesOutput);           // outgoing msg byte count
  msgEndStep.format(args);
  msgLog.logDebugMessage(msgEndStep);
}

//------------------------------------------------------------------------------
// Log a syslog message for the physical vs cache block I/O counts
//------------------------------------------------------------------------------
void JobStep::syslogReadBlockCounts(uint32_t subSystem, uint64_t physicalReadCount, uint64_t cacheReadCount,
                                    uint64_t casualPartBlocks) const
{
  LoggingID logId(subSystem, sessionId(), txnId());
  MessageLog msgLog(logId);

  Message msgEndStep(M0032);
  Message::Args args;
  args.add((uint64_t)statementId());  // statement id for this job step
  args.add((int)stepId());            // step id for this job step
  args.add((int)oid());               // step id for this job step
  args.add(physicalReadCount);        // blocked datalist input (ex: fifo)
  args.add(cacheReadCount);           // blocked datalist output(ex: fifo)
  args.add(casualPartBlocks);         // casual partition block hits
  msgEndStep.format(args);
  msgLog.logDebugMessage(msgEndStep);
}

//------------------------------------------------------------------------------
// Log a syslog msg for the effective start/end times for this step
// (lastWriteTime denotes when the EndOfInput marker was written out).
//------------------------------------------------------------------------------
void JobStep::syslogProcessingTimes(uint32_t subSystem, const struct timeval& firstReadTime,
                                    const struct timeval& lastReadTime, const struct timeval& firstWriteTime,
                                    const struct timeval& lastWriteTime) const
{
  LoggingID logId(subSystem, sessionId(), txnId());
  MessageLog msgLog(logId);
  Message msgStartStep(M0046);
  Message::Args args;

  args.add((uint64_t)statementId());              // statement id for this job step
  args.add((int)stepId());                        // step id for this job step
  args.add(JSTimeStamp::format(firstReadTime));   // when first DL input element read
  args.add(JSTimeStamp::format(lastReadTime));    // when last DL input element read
  args.add(JSTimeStamp::format(firstWriteTime));  // when first DL output elem written
  args.add(JSTimeStamp::format(lastWriteTime));   // when EndOfInput written to DL out
  msgStartStep.format(args);
  msgLog.logDebugMessage(msgStartStep);
}

bool JobStep::traceOn() const
{
  return fTraceFlags & execplan::CalpontSelectExecutionPlan::TRACE_LOG;
}

//////////////////////////////////////////////////////////////////////
// DESCRIPTION:
//  The m() rethrows a query runtime exception and handles it across
//  all steps in a uniform
//  way.
// PARAMETERS:
//  e                   ptr to the exception raised
//  errorCode           error code to log
//  critErrorCode       is this a crit IDBExcept or not
//  methodName          method name to log
//////////////////////////////////////////////////////////////////////
void JobStep::handleException(std::exception_ptr e, const int errorCode, const unsigned infoErrorCode,
                              const std::string& methodName)
{
  try
  {
    std::rethrow_exception(e);
  }
  catch (logging::OutOfMemoryExcept& exc)
  {
   std::cerr << methodName << " caught a OutOfMemory exception. " << std::endl;
    catchHandler(methodName + " caught " + exc.what(), errorCode, fErrorInfo, fSessionId);
  }
  // Add it here for now to handle potential bad_alloc exceptions
  catch (std::bad_alloc& exc)
  {
   std::cerr << methodName << " caught a bad_alloc exception. " << std::endl;
    catchHandler(methodName + " caught " + exc.what(), errorCode, fErrorInfo, fSessionId);
  }
  catch (const IDBExcept& iex)
  {
    std::cerr << methodName << " caught a internal exception. " << std::endl;

    catchHandler(methodName + " " + iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
                 (iex.errorCode() == infoErrorCode ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
  }
  catch (boost::exception& e)
  {
    std::cerr << methodName << " caught a boost::exception. " << std::endl;
    catchHandler(methodName + " caught " + boost::diagnostic_information(e), errorCode, fErrorInfo,
                 fSessionId);
  }
  catch (const std::exception& ex)
  {
    std::cerr << methodName << " caught an exception. " << std::endl;
    catchHandler(methodName + " caught " + ex.what(), errorCode, fErrorInfo, fSessionId);
  }
  catch (...)
  {
    std::ostringstream oss;

    std::cerr << methodName << " caught an unknown exception." << std::endl;
    catchHandler(methodName + " caught an unknown exception ", errorCode, fErrorInfo, fSessionId);
  }
}

}  // namespace joblist

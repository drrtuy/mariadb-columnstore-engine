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

/***********************************************************************
 *   $Id: primproc.cpp 2147 2013-08-14 20:44:44Z bwilkinson $
 *
 *
 ***********************************************************************/
#include <unistd.h>
#include <string>
#include <iostream>
#ifdef QSIZE_DEBUG
#include <iomanip>
#include <fstream>
#endif
#include <csignal>
#include <sys/time.h>
#include <sys/resource.h>
#include <tr1/unordered_set>

#include <clocale>
#include <iterator>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
// #define NDEBUG
#include <cassert>
using namespace std;

#include <boost/thread.hpp>
using namespace boost;

#include "configcpp.h"
using namespace config;

#include "messageids.h"
using namespace logging;

#include "primproc.h"
#include "primitiveserver.h"
#include "MonitorProcMem.h"
#include "pp_logger.h"
#include "umsocketselector.h"
using namespace primitiveprocessor;

#include "archcheck.h"
using namespace archcheck;

#include "liboamcpp.h"
using namespace oam;

#include "IDBPolicy.h"
using namespace idbdatafile;

#include "cgroupconfigurator.h"

#include "crashtrace.h"
#include "installdir.h"

#include "mariadb_my_sys.h"

#include "spinlock.h"
#include "service.h"
#include "serviceexemgr.h"

namespace primitiveprocessor
{
extern uint32_t BPPCount;
extern uint32_t blocksReadAhead;
extern uint32_t defaultBufferSize;
extern uint32_t connectionsPerUM;
extern uint32_t highPriorityThreads;
extern uint32_t medPriorityThreads;
extern uint32_t lowPriorityThreads;
extern int directIOFlag;
extern int noVB;

DebugLevel gDebugLevel;
Logger* mlp;

bool isDebug(const DebugLevel level)
{
  return level <= gDebugLevel;
}

}  // namespace primitiveprocessor

namespace
{
int toInt(const string& val)
{
  if (val.length() == 0)
    return -1;

  return static_cast<int>(Config::fromText(val));
}

void setupSignalHandlers()
{
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigaction(SIGUSR2, &ign, 0);

  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGPIPE);
  sigaddset(&sigset, SIGUSR2);
  sigprocmask(SIG_BLOCK, &sigset, 0);
}

int8_t setupCwd(Config* /*cf*/)
{
  string workdir = startup::StartUp::tmpDir();

  if (workdir.length() == 0)
    workdir = ".";

  int8_t rc = chdir(workdir.c_str());

  if (rc < 0 || access(".", W_OK) != 0)
    rc = chdir("/tmp");

  return rc;
}

int setupResources()
{
  struct rlimit rlim;

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
  {
    return -1;
  }

  rlim.rlim_cur = rlim.rlim_max = 65536;

  if (setrlimit(RLIMIT_NOFILE, &rlim) != 0)
  {
    return -2;
  }

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
  {
    return -3;
  }

  if (rlim.rlim_cur != 65536)
  {
    return -4;
  }

  return 0;
}

#ifdef QSIZE_DEBUG
class QszMonThd
{
 public:
  QszMonThd(PrimitiveServer* psp, ofstream* qszlog) : fPsp(psp), fQszLog(qszlog)
  {
  }

  ~QszMonThd()
  {
  }

  void operator()()
  {
    utils::setThreadName("QszMonThd");
    for (;;)
    {
      uint32_t qd = fPsp->getProcessorThreadPool()->getWaiting();

      if (fQszLog)
      {
        // Get a timestamp for output.
        struct tm tm;
        struct timeval tv;

        gettimeofday(&tv, 0);
        localtime_r(&tv.tv_sec, &tm);

        ostringstream oss;
        oss << setfill('0') << setw(2) << tm.tm_hour << ':' << setw(2) << tm.tm_min << ':' << setw(2)
            << tm.tm_sec << '.' << setw(4) << tv.tv_usec / 100;

        *fQszLog << oss.str() << ' ' << qd << endl;
      }

      struct timespec req = {0, 1000 * 100};  // 100 usec

      nanosleep(&req, 0);
    }
  }

 private:
  // defaults okay
  // QszMonThd(const QszMonThd& rhs);
  // QszMonThd& operator=(const QszMonThd& rhs);
  const PrimitiveServer* fPsp;
  ofstream* fQszLog;
};
#endif

// #define DUMP_CACHE_CONTENTS
#ifdef DUMP_CACHE_CONTENTS
void* waitForSIGUSR1(void* p)
{
  utils::setThreadName("waitForSIGUSR1");
#if defined(__LP64__) || defined(_MSC_VER)
  ptrdiff_t tmp = reinterpret_cast<ptrdiff_t>(p);
  int cacheCount = static_cast<int>(tmp);
#else
  int cacheCount = reinterpret_cast<int>(p);
#endif
  sigset_t oset;
  int rec_sig;
  int32_t rpt_state = 0;
  sigfillset(&oset);
  sigdelset(&oset, SIGUSR1);
  sigdelset(&oset, SIGUSR2);
  pthread_sigmask(SIG_SETMASK, &oset, 0);

  sigemptyset(&oset);
  sigaddset(&oset, SIGUSR1);
  sigaddset(&oset, SIGUSR2);

  for (;;)
  {
    sigwait(&oset, &rec_sig);

    if (rec_sig == SIGUSR1)
    {
      ostringstream out;

      for (int i = 0; i < cacheCount; i++)
      {
        BRPp[i]->formatLRUList(out);
        cout << out.str() << "###" << endl;
      }
    }
    else if (rec_sig == SIGUSR2)
    {
      // is reporting currently on?
      rpt_state = BRPp[0]->ReportingFrequency();

      if (rpt_state > 0)
        rpt_state = 0;  // turn reporting off
      else
        rpt_state = 1;  // fbm will set to the value from config file

      for (int i = 0; i < cacheCount; i++)
        BRPp[i]->setReportingFrequency(rpt_state);

      cout << "@@@" << endl;
    }
  }

  return 0;
}
#endif

}  // namespace

ServicePrimProc* ServicePrimProc::fInstance = nullptr;
ServicePrimProc* ServicePrimProc::instance()
{
  if (!fInstance)
    fInstance = new ServicePrimProc();

  return fInstance;
}

int ServicePrimProc::Child()
{
  Config* cf = Config::makeConfig();

  setupSignalHandlers();

  int err = 0;
  err = setupCwd(cf);

  // Initialize the charset library
  MY_INIT("PrimProc");

  mlp = new primitiveprocessor::Logger();

  if (!m_debug)
    err = setupResources();
  string errMsg;

  switch (err)
  {
    case -1:
    case -3: errMsg = "Error getting file limits, please see non-root install documentation"; break;

    case -2: errMsg = "Error setting file limits, please see non-root install documentation"; break;

    case -4:
      errMsg = "Could not install file limits to required value, please see non-root install documentation";
      break;

    case -5: errMsg = "Could not change into working directory"; break;

    default: break;
  }

  if (err < 0)
  {
    Oam oam;
    mlp->logMessage(errMsg);
    cerr << errMsg << endl;

    NotifyServiceInitializationFailed();

    // Free up resources allocated by MY_INIT() above.
    my_end(0);

    return 2;
  }
  bool runningWithExeMgr = true;
  auto* rm = joblist::ResourceManager::instance(runningWithExeMgr, cf);

  utils::USpaceSpinLock startupRaceLock(getStartupRaceFlag());
  std::thread exeMgrThread(
      [this, rm]()
      {
        exemgr::Opt opt;
        exemgr::globServiceExeMgr = new exemgr::ServiceExeMgr(opt, rm);
        // primitive delay to avoid 'not connected to PM' log error messages
        // from EM. PrimitiveServer::start() releases SpinLock after sockets
        // are available.
        utils::USpaceSpinLock startupRaceLock(this->getStartupRaceFlag());
        exemgr::globServiceExeMgr->Child();
      });

  int serverThreads = 1;
  int serverQueueSize = 10;
  int processorWeight = 8 * 1024;
  int processorQueueSize = 10 * 1024;
  int64_t BRPBlocksPct = 70;
  uint32_t BRPBlocks = 1887437;
  int BRPThreads = 16;
  int cacheCount = 1;
  int maxBlocksPerRead = 256;  // 1MB
  bool rotatingDestination = false;
  uint32_t deleteBlocks = 128;
  bool PTTrace = false;
  string strTemp;
  int priority = -1;
  const string primitiveServers("PrimitiveServers");
  const string jobListStr("JobList");
  const string dbbc("DBBC");
  const string ExtentMapStr("ExtentMap");
  uint64_t extentRows = 8 * 1024 * 1024;
  uint64_t MaxExtentSize = 0;
  double prefetchThreshold;
  uint64_t PMSmallSide = 67108864;
  BPPCount = 16;
  int numCores = -1;
  int configNumCores = -1;
  uint32_t highPriorityPercentage, medPriorityPercentage, lowPriorityPercentage;
  utils::CGroupConfigurator cg;

  gDebugLevel = primitiveprocessor::NONE;

  int temp = toInt(cf->getConfig(primitiveServers, "ServerThreads"));

  if (temp > 0)
    serverThreads = temp;

  temp = toInt(cf->getConfig(primitiveServers, "ServerQueueSize"));

  if (temp > 0)
    serverQueueSize = temp;

  temp = toInt(cf->getConfig(primitiveServers, "ProcessorThreshold"));

  if (temp > 0)
    processorWeight = temp;

  temp = toInt(cf->getConfig(primitiveServers, "ProcessorQueueSize"));

  if (temp > 0)
    processorQueueSize = temp;

  temp = toInt(cf->getConfig(primitiveServers, "DebugLevel"));

  if (temp > 0)
    gDebugLevel = (DebugLevel)temp;

  highPriorityPercentage = 0;
  temp = toInt(cf->getConfig(primitiveServers, "HighPriorityPercentage"));

  if (temp >= 0)
    highPriorityPercentage = temp;

  medPriorityPercentage = 0;
  temp = toInt(cf->getConfig(primitiveServers, "MediumPriorityPercentage"));

  if (temp >= 0)
    medPriorityPercentage = temp;

  lowPriorityPercentage = 0;
  temp = toInt(cf->getConfig(primitiveServers, "LowPriorityPercentage"));

  if (temp >= 0)
    lowPriorityPercentage = temp;

  temp = toInt(cf->getConfig(ExtentMapStr, "ExtentRows"));

  if (temp > 0)
    extentRows = temp;

  temp = toInt(cf->getConfig(primitiveServers, "ConnectionsPerPrimProc"));

  if (temp > 0)
    connectionsPerUM = temp;
  else
    connectionsPerUM = 1;

  // set to smallest extent size
  // do not allow to read beyond the end of an extent
  const int MaxReadAheadSz = (extentRows) / BLOCK_SIZE;
  // Max size for a primitive message column buffer. Must be big enough
  // to accomodate 8192 values of the widest non-dict column + 8192 RIDs + ohead.
  defaultBufferSize = 150 * 1024;

  // This parm controls whether we rotate through the output sockets
  // when deciding where to send response messages, or whether to simply
  // send the response to the socket of origin.  Should normally be set
  // to 'y', for install types 1 and 3.
  string strVal = cf->getConfig(primitiveServers, "RotatingDestination");
  // XXX: Permanently disable for now...
  strVal = "N";

  if ((strVal == "y") || (strVal == "Y"))
  {
    rotatingDestination = true;

    // Disable destination rotation if UM and PM are running on same
    // server, because we could accidentally end up sending DMLProc
    // responses to ExeMgr and vice versa, if we rotated socket dest.
    temp = toInt(cf->getConfig("Installation", "ServerTypeInstall"));

    if ((temp == oam::INSTALL_COMBINE_DM_UM_PM) || (temp == oam::INSTALL_COMBINE_PM_UM))
      rotatingDestination = false;
  }

  string strBlockPct = cf->getConfig(dbbc, "NumBlocksPct");
  temp = atoi(strBlockPct.c_str());

  bool absCache = false;
  if (temp > 0)
  {
    BRPBlocksPct = temp;
    /* MCOL-1847.  Did the user specify this as an absolute? */
    int len = strBlockPct.length();
    if ((strBlockPct[len - 1] >= 'a' && strBlockPct[len - 1] <= 'z') ||
        (strBlockPct[len - 1] >= 'A' && strBlockPct[len - 1] <= 'Z'))
    {
      absCache = true;
      BRPBlocksPct = Config::fromText(strBlockPct);
    }
  }
  if (absCache)
    BRPBlocks = BRPBlocksPct / 8192;
  else
    BRPBlocks = ((BRPBlocksPct / 100.0) * (double)cg.getTotalMemory()) / 8192;
#if 0
    temp = toInt(cf->getConfig(dbbc, "NumThreads"));

    if (temp > 0)
        BRPThreads = temp;

#endif
  temp = toInt(cf->getConfig(dbbc, "NumCaches"));

  if (temp > 0)
    cacheCount = temp;

  temp = toInt(cf->getConfig(dbbc, "NumDeleteBlocks"));

  if (temp > 0)
    deleteBlocks = temp;

  if ((uint32_t)(.01 * BRPBlocks) < deleteBlocks)
    deleteBlocks = (uint32_t)(.01 * BRPBlocks);

  temp = toInt(cf->getConfig(primitiveServers, "ColScanBufferSizeBlocks"));

  if (temp > (int)MaxReadAheadSz || temp < 1)
    maxBlocksPerRead = MaxReadAheadSz;
  else if (temp > 0)
    maxBlocksPerRead = temp;

  temp = toInt(cf->getConfig(primitiveServers, "ColScanReadAheadBlocks"));

  if (temp > (int)MaxReadAheadSz || temp < 0)
    blocksReadAhead = MaxReadAheadSz;
  else if (temp > 0)
  {
    // make sure we've got an integral factor of extent size
    for (; (MaxExtentSize % temp) != 0; ++temp)
      ;

    blocksReadAhead = temp;
  }

  temp = toInt(cf->getConfig(primitiveServers, "PTTrace"));

  if (temp > 0)
    PTTrace = true;

  temp = toInt(cf->getConfig(primitiveServers, "PrefetchThreshold"));

  if (temp < 0 || temp > 100)
    prefetchThreshold = 0;
  else
    prefetchThreshold = temp / 100.0;

  int maxPct = 0;  // disable by default
  temp = toInt(cf->getConfig(primitiveServers, "MaxPct"));

  if (temp >= 0)
    maxPct = temp;

  // We could use this same mechanism for other growing buffers.
  int aggPct = 95;
  temp = toInt(cf->getConfig("SystemConfig", "MemoryCheckPercent"));

  if (temp >= 0)
    aggPct = temp;

  //...Start the thread to monitor our memory usage
  new boost::thread(utils::MonitorProcMem(maxPct, aggPct, 28));

  // config file priority is 40..1 (highest..lowest)
  string sPriority = cf->getConfig(primitiveServers, "Priority");

  if (sPriority.length() > 0)
    temp = toInt(sPriority);
  else
    temp = 21;

  // convert config file value to setpriority(2) value (-20..19, -1 is the default)
  if (temp > 0)
    priority = 20 - temp;
  else if (temp < 0)
    priority = 19;

  if (priority < -20)
    priority = -20;

  setpriority(PRIO_PROCESS, 0, priority);
  //..Instantiate UmSocketSelector singleton.  Disable rotating destination
  //..selection if no UM IP addresses are in the Calpo67108864LLnt.xml file.
  UmSocketSelector* pUmSocketSelector = UmSocketSelector::instance();

  if (rotatingDestination)
  {
    if (pUmSocketSelector->ipAddressCount() < 1)
      rotatingDestination = false;
  }

  // See if we want to override the calculated #cores
  temp = toInt(cf->getConfig(primitiveServers, "NumCores"));

  if (temp > 0)
    configNumCores = temp;

  if (configNumCores <= 0)
  {
    // count the actual #cores

    numCores = cg.getNumCores();

    if (numCores == 0)
      numCores = 8;
  }
  else
    numCores = configNumCores;

  // based on the #cores, calculate some thread parms
  if (numCores > 0)
  {
    BRPThreads = 2 * numCores;
    // there doesn't seem much benefit to having more than this, and sometimes it causes problems.
    // DBBC.NumThreads can override this cap
    BRPThreads = std::min(BRPThreads, 32);
  }

  // the default is ~10% low, 30% medium, 60% high, (where 2*cores = 100%)
  if (highPriorityPercentage == 0 && medPriorityPercentage == 0 && lowPriorityPercentage == 0)
  {
    lowPriorityThreads = max(1, (2 * numCores) / 10);
    medPriorityThreads = max(1, (2 * numCores) / 3);
    highPriorityThreads = (2 * numCores) - lowPriorityThreads - medPriorityThreads;
  }
  else
  {
    uint32_t totalThreads =
        (uint32_t)((lowPriorityPercentage + medPriorityPercentage + highPriorityPercentage) / 100.0 *
                   (2 * numCores));

    if (totalThreads == 0)
      totalThreads = 1;

    lowPriorityThreads = (uint32_t)(lowPriorityPercentage / 100.0 * (2 * numCores));
    medPriorityThreads = (uint32_t)(medPriorityPercentage / 100.0 * (2 * numCores));
    highPriorityThreads = totalThreads - lowPriorityThreads - medPriorityThreads;
  }

  BPPCount = highPriorityThreads + medPriorityThreads + lowPriorityThreads;

  // let the user override if they want
  temp = toInt(cf->getConfig(primitiveServers, "BPPCount"));

  if (temp > 0 && temp < (int)BPPCount)
    BPPCount = temp;

  temp = toInt(cf->getConfig(dbbc, "NumThreads"));

  if (temp > 0)
    BRPThreads = temp;

  // @bug4598, switch for O_DIRECT to support gluster fs.
  // directIOFlag == O_DIRECT, by default
  strVal = cf->getConfig(primitiveServers, "DirectIO");

  if ((strVal == "n") || (strVal == "N"))
    directIOFlag = 0;

  IDBPolicy::configIDBPolicy();

  // no versionbuffer if using HDFS for performance reason
  if (IDBPolicy::useHdfs())
    noVB = 1;

  cout << "Starting PrimitiveServer: st = " << serverThreads << ", sq = " << serverQueueSize
       << ", pw = " << processorWeight << ", pq = " << processorQueueSize << ", nb = " << BRPBlocks
       << ", nt = " << BRPThreads << ", nc = " << cacheCount << ", ra = " << blocksReadAhead
       << ", db = " << deleteBlocks << ", mb = " << maxBlocksPerRead << ", rd = " << rotatingDestination
       << ", tr = " << PTTrace << ", ss = " << PMSmallSide << ", bp = " << BPPCount << endl;

  PrimitiveServer server(serverThreads, serverQueueSize, processorWeight, processorQueueSize,
                         rotatingDestination, BRPBlocks, BRPThreads, cacheCount, maxBlocksPerRead,
                         blocksReadAhead, deleteBlocks, PTTrace, prefetchThreshold, PMSmallSide);

#ifdef QSIZE_DEBUG
  thread* qszMonThd;

  if (gDebugLevel >= STATS)
  {
    ofstream* qszLog = new ofstream("/var/log/mariadb/columnstore/trace/ppqsz.dat");

    if (!qszLog->good())
    {
      qszLog->close();
      delete qszLog;
      qszLog = 0;
    }

    qszMonThd = new thread(QszMonThd(&server, qszLog));
  }

#endif

#ifdef DUMP_CACHE_CONTENTS
  {
    // Need to use pthreads API here...
    pthread_t thd1;
    pthread_attr_t attr1;
    pthread_attr_init(&attr1);
    pthread_attr_setdetachstate(&attr1, PTHREAD_CREATE_DETACHED);
    pthread_create(&thd1, &attr1, waitForSIGUSR1, reinterpret_cast<void*>(cacheCount));
  }
#endif

  primServerThreadPool = server.getProcessorThreadPool();

  server.start(this, startupRaceLock);

  cerr << "server.start() exited!" << endl;

  // Free up resources allocated by MY_INIT() above.
  my_end(0);

  return 1;
}

int main(int argc, char** argv)
{
  if (checkArchitecture() != arcitecture::SSE4_2 && checkArchitecture() != arcitecture::ASIMD)
  {
    std::cerr << "Unsupported CPU architecture. ARM Advanced SIMD or x86_64 SSE4.2 required; aborting. \n";
    return 1;
  }
  Opt opt(argc, argv);
  // Set locale language
  setlocale(LC_ALL, "");
  setlocale(LC_NUMERIC, "C");
  // This is unset due to the way we start it
  program_invocation_short_name = const_cast<char*>("PrimProc");

  ServicePrimProc::instance()->setOpt(opt);
  return ServicePrimProc::instance()->Run();
}

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

/*******************************************************************************
 * $Id: we_bulkload.h 4631 2013-05-02 15:21:09Z dcathey $
 *
 *******************************************************************************/
/** @file */

#pragma once
#include <pthread.h>
#include <fstream>
#include <string>
#include <vector>
#include <sys/time.h>

#include <we_log.h>
#include <we_colopbulk.h>
#include <we_xmljob.h>
#include <we_convertor.h>
#include <writeengine.h>

#include <we_brm.h>

#include "we_tableinfo.h"
#include "brmtypes.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "boost/ptr_container/ptr_vector.hpp"
#pragma GCC diagnostic pop

#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/nil_generator.hpp>

/** Namespace WriteEngine */
namespace WriteEngine
{
/** Class BulkLoad */
class BulkLoad : public FileOp
{
 public:
  /**
   * @brief BulkLoad constructor
   */
  BulkLoad();

  /**
   * @brief BulkLoad destructor
   */
  ~BulkLoad() override;

  /**
   * @brief Load job information
   */
  int loadJobInfo(const std::string& fullFileName, bool bUseTempJobFile, int argc, char** argv,
                  bool bLogInfo2ToConsole, bool bValidateColumnList);

  /**
   * @brief Pre process jobs to validate and assign values to the job structure
   */
  int preProcess(Job& job, int tableNo, std::shared_ptr<TableInfo>& tableInfo);

  /**
   * @brief Print job information
   */
  void printJob();

  /**
   * @brief Process job
   */
  int processJob();

  /**
   * @brief Set Debug level for this BulkLoad object and any data members
   */
  void setAllDebug(DebugLevel level);

  /**
   * @brief Update next autoincrement value for specified OID.
   *  @param columnOID  oid of autoincrement column to be updated
   *  @param nextValue next autoincrement value to assign to tableOID
   */
  static int updateNextValue(OID columnOID, uint64_t nextValue);

  // Accessors and mutators
  void addToCmdLineImportFileList(const std::string& importFile);
  const std::string& getAlternateImportDir() const;
  const std::string& getErrorDir() const;
  long getTimeZone() const;
  const std::string& getJobDir() const;
  const std::string& getSchema() const;
  const std::string& getTempJobDir() const;
  const std::string& getS3Key() const;
  const std::string& getS3Secret() const;
  const std::string& getS3Bucket() const;
  const std::string& getS3Host() const;
  const std::string& getS3Region() const;
  bool getTruncationAsError() const;
  BulkModeType getBulkLoadMode() const;
  bool getContinue() const;
  boost::uuids::uuid getJobUUID() const
  {
    return fUUID;
  }

  int setAlternateImportDir(const std::string& loadDir, std::string& errMsg);
  void setImportDataMode(ImportDataMode importMode);
  void setColDelimiter(char delim);
  void setBulkLoadMode(BulkModeType bulkMode, const std::string& rptFileName);
  void setEnclosedByChar(char enChar);
  void setEscapeChar(char esChar);
  void setSkipRows(size_t skipRows);
  void setKeepRbMetaFiles(bool keepMeta);
  void setMaxErrorCount(int maxErrors);
  void setNoOfParseThreads(int parseThreads);
  void setNoOfReadThreads(int readThreads);
  void setNullStringMode(bool bMode);
  void setParseErrorOnTable(int tableId, bool lockParseMutex);
  void setParserNum(int parser);
  void setProcessName(const std::string& processName);
  void setReadBufferCount(int noOfReadBuffers);
  void setReadBufferSize(int readBufferSize);
  void setTxnID(BRM::TxnID txnID);
  void setVbufReadSize(int vbufReadSize);
  void setTruncationAsError(bool bTruncationAsError);
  void setJobUUID(const std::string& jobUUID);
  void setErrorDir(const std::string& errorDir);
  void setTimeZone(long timeZone);
  void setS3Key(const std::string& key);
  void setS3Secret(const std::string& secret);
  void setS3Bucket(const std::string& bucket);
  void setS3Host(const std::string& host);
  void setS3Region(const std::string& region);
  void setUsername(const std::string& username);
  // Timer functions
  void startTimer();
  void stopTimer();
  double getTotalRunTime() const;

  void disableTimeOut(const bool disableTimeOut);
  bool disableTimeOut() const;

  static void disableConsoleOutput(const bool noConsoleOutput)
  {
    fNoConsoleOutput = noConsoleOutput;
  }
  static bool disableConsoleOutput()
  {
    return fNoConsoleOutput;
  }

  // Add error message into appropriate BRM updater
  static bool addErrorMsg2BrmUpdater(const std::string& tablename, const std::ostringstream& oss);
  void setDefaultJobUUID();

 private:
  //--------------------------------------------------------------------------
  // Private Data Members
  //--------------------------------------------------------------------------
  XMLJob fJobInfo;  // current job information

  boost::scoped_ptr<ColumnOp> fColOp{new ColumnOpBulk()};  // column operation

  std::string fRootDir;      // job process root directory
  std::string fJobFileName;  // job description file name

  Log fLog;  // logger

  int fNumOfParser{0};  // total number of parser
  char fColDelim{0}; // delimits col values within a row

  int fNoOfBuffers{-1};                                       // Number of read buffers
  int fBufferSize{-1};                                        // Read buffer size
  int fFileVbufSize{-1};                                      // Internal file system buffer size
  long long fMaxErrors{MAX_ERRORS_DEFAULT};                   // Max allowable errors per job
  std::string fAlternateImportDir;                            // Alternate bulk import directory
  std::string fErrorDir;                                      // Opt. where error records record
  std::string fProcessName;                                   // Application process name
  static std::vector<std::shared_ptr<TableInfo>> fTableInfo;  // Vector of Table information
  int fNoOfParseThreads{3};                                   // Number of parse threads
  int fNoOfReadThreads{1};                                    // Number of read threads
  boost::thread_group fReadThreads;                           // Read thread group
  boost::thread_group fParseThreads;                          // Parse thread group
  boost::mutex fReadMutex;                                    // Manages table selection by each
                                                              //   read thread
  boost::mutex fParseMutex;  // Manages table/buffer/column
                             //   selection by each parsing thread
  BRM::TxnID fTxnID;                                // TransID acquired from SessionMgr
  bool fKeepRbMetaFiles{false};                     // Keep/delete bulkRB metadata files
  bool fNullStringMode{false};                      // Treat "NULL" as NULL value
  char fEnclosedByChar{0};                          // Char used to enclose column value
  char fEscapeChar{0};                              // Escape char within enclosed value
  size_t fSkipRows{0};                              // Header rows to skip
  timeval fStartTime{0, 0};                         // job start time
  timeval fEndTime{0, 0};                           // job end time
  double fTotalTime{0.0};                           // elapsed time for current phase
  std::vector<std::string> fCmdLineImportFiles;     // Import Files from cmd line
  BulkModeType fBulkMode{BULK_MODE_LOCAL};          // Distributed bulk mode (1,2, or 3)
  std::string fBRMRptFileName;                      // Name of distributed mode rpt file
  bool fbTruncationAsError{false};                  // Treat string truncation as error
  ImportDataMode fImportDataMode{IMPORT_DATA_TEXT}; // Importing text or binary data
  bool fbContinue{false};                           // true when read and parse r running
  //
  static boost::mutex* fDDLMutex;  // Insure only 1 DDL op at a time

  static const std::string DIR_BULK_JOB;              // Bulk job directory
  static const std::string DIR_BULK_TEMP_JOB;         // Dir for tmp job files
  static const std::string DIR_BULK_IMPORT;           // Bulk job import dir
  static const std::string DIR_BULK_LOG;              // Bulk job log directory
  bool fDisableTimeOut{false};                        // disable timeout when waiting for table lock
  boost::uuids::uuid fUUID{boost::uuids::nil_generator()()}; // job UUID
  static bool fNoConsoleOutput;                       // disable output to console
  long fTimeZone{dataconvert::systemTimeZoneOffset()};// Timezone offset (in seconds) relative to UTC,
                                                      // to use for TIMESTAMP data type. For example,
                                                      // for EST which is UTC-5:00, offset will be -18000s.
  std::string fS3Key;                                 // S3 Key
  std::string fS3Secret;                              // S3 Secret
  std::string fS3Host;                                // S3 Host
  std::string fS3Bucket;                              // S3 Bucket
  std::string fS3Region;                              // S3 Region
  std::string fUsername{"mysql"};                     // data files owner name mysql by default

  //--------------------------------------------------------------------------
  // Private Functions
  //--------------------------------------------------------------------------

  // Spawn the worker threads.
  void spawnWorkers();

  // Checks if all tables have the status set
  bool allTablesDone(Status status);

  // Lock the table for read. Called by the read thread.
  int lockTableForRead(int id);

  // Get column for parsing. Called by the parse thread.
  // @bug 2099 - Temporary hack to diagnose deadlock. Added report parm below.
  bool lockColumnForParse(int id,              // thread id
                          int& tableId,        // selected table id
                          int& columnId,       // selected column id
                          int& myParseBuffer,  // selected parse buffer
                          bool report);

  // Map specified DBRoot to it's first segment file number
  int mapDBRootToFirstSegment(OID columnOid, uint16_t dbRoot, uint16_t& segment);

  // The thread method for the read thread.
  void read(int id);

  // The thread method for the parse thread.
  void parse(int id);

  // Sleep method
  void sleepMS(long int ms);

  // Initialize auto-increment column for specified schema and table.
  int preProcessAutoInc(const std::string& fullTableName,  // schema.table
                        ColumnInfo* colInfo);              // ColumnInfo associated with AI column

  // Determine starting HWM and LBID after block skipping added to HWM
  int preProcessHwmLbid(const ColumnInfo* info, int minWidth, uint32_t partition, uint16_t segment, HWM& hwm,
                        BRM::LBID_t& lbid, bool& bSkippedToNewExtent);

  // Rollback any tables that are left in a locked state at EOJ.
  int rollbackLockedTables();

  // Rollback a table left in a locked state.
  int rollbackLockedTable(TableInfo& tableInfo);

  // Save metadata info required for shared-nothing bulk rollback.
  int saveBulkRollbackMetaData(Job& job,              // current job
                               TableInfo* tableInfo,  // TableInfo for table of interest
                               const std::vector<DBRootExtentInfo>& segFileInfo,  // vector seg file info
                               const std::vector<BRM::EmDbRootHWMInfo_v>& dbRootHWMInfoPM);

  // Manage/validate the list of 1 or more import data files
  int manageImportDataFileList(Job& job,               // current job
                               int tableNo,            // table number of current job
                               TableInfo* tableInfo);  // TableInfo for table of interest

  // Break up list of file names into a vector of filename strings
  int buildImportDataFileList(const std::string& location, const std::string& filename,
                              std::vector<std::string>& importFileNames);
};

//------------------------------------------------------------------------------
// Inline functions
//------------------------------------------------------------------------------
inline void BulkLoad::addToCmdLineImportFileList(const std::string& importFile)
{
  fCmdLineImportFiles.push_back(importFile);
}

inline const std::string& BulkLoad::getAlternateImportDir() const
{
  return fAlternateImportDir;
}

inline const std::string& BulkLoad::getErrorDir() const
{
  return fErrorDir;
}

inline long BulkLoad::getTimeZone() const
{
  return fTimeZone;
}

inline const std::string& BulkLoad::getJobDir() const
{
  return DIR_BULK_JOB;
}

inline const std::string& BulkLoad::getSchema() const
{
  return fJobInfo.getJob().schema;
}

inline const std::string& BulkLoad::getTempJobDir() const
{
  return DIR_BULK_TEMP_JOB;
}

inline const std::string& BulkLoad::getS3Key() const
{
  return fS3Key;
}

inline const std::string& BulkLoad::getS3Secret() const
{
  return fS3Secret;
}

inline const std::string& BulkLoad::getS3Bucket() const
{
  return fS3Bucket;
}

inline const std::string& BulkLoad::getS3Host() const
{
  return fS3Host;
}

inline const std::string& BulkLoad::getS3Region() const
{
  return fS3Region;
}

inline bool BulkLoad::getTruncationAsError() const
{
  return fbTruncationAsError;
}

inline BulkModeType BulkLoad::getBulkLoadMode() const
{
  return fBulkMode;
}

inline bool BulkLoad::getContinue() const
{
  return fbContinue;
}

inline void BulkLoad::printJob()
{
  if (isDebug(DEBUG_1))
    fJobInfo.printJobInfo(fLog);
  else
    fJobInfo.printJobInfoBrief(fLog);
}

inline void BulkLoad::setAllDebug(DebugLevel level)
{
  setDebugLevel(level);
  fLog.setDebugLevel(level);
}

inline void BulkLoad::setColDelimiter(char delim)
{
  fColDelim = delim;
}

inline void BulkLoad::setBulkLoadMode(BulkModeType bulkMode, const std::string& rptFileName)
{
  fBulkMode = bulkMode;
  fBRMRptFileName = rptFileName;
}

inline void BulkLoad::setEnclosedByChar(char enChar)
{
  fEnclosedByChar = enChar;
}

inline void BulkLoad::setEscapeChar(char esChar)
{
  fEscapeChar = esChar;
}

inline void BulkLoad::setSkipRows(size_t skipRows)
{
  fSkipRows = skipRows;
}

inline void BulkLoad::setImportDataMode(ImportDataMode importMode)
{
  fImportDataMode = importMode;
}

inline void BulkLoad::setKeepRbMetaFiles(bool keepMeta)
{
  fKeepRbMetaFiles = keepMeta;
}

inline void BulkLoad::setMaxErrorCount(int maxErrors)
{
  fMaxErrors = maxErrors;
}

inline void BulkLoad::setNoOfParseThreads(int parseThreads)
{
  fNoOfParseThreads = parseThreads;
}

inline void BulkLoad::setNoOfReadThreads(int readThreads)
{
  fNoOfReadThreads = readThreads;
}

inline void BulkLoad::setNullStringMode(bool bMode)
{
  fNullStringMode = bMode;
}

inline void BulkLoad::setParserNum(int parser)
{
  fNumOfParser = parser;
}

inline void BulkLoad::setProcessName(const std::string& processName)
{
  fProcessName = processName;
}

inline void BulkLoad::setReadBufferCount(int noOfReadBuffers)
{
  fNoOfBuffers = noOfReadBuffers;
}

inline void BulkLoad::setReadBufferSize(int readBufferSize)
{
  fBufferSize = readBufferSize;
}

inline void BulkLoad::setTxnID(BRM::TxnID txnID)
{
  fTxnID = txnID;
}

inline void BulkLoad::setVbufReadSize(int vbufReadSize)
{
  fFileVbufSize = vbufReadSize;
}

inline void BulkLoad::setTruncationAsError(bool bTruncationAsError)
{
  fbTruncationAsError = bTruncationAsError;
}

inline void BulkLoad::setErrorDir(const std::string& errorDir)
{
  fErrorDir = errorDir;
}

inline void BulkLoad::setTimeZone(long timeZone)
{
  fTimeZone = timeZone;
}

inline void BulkLoad::setS3Key(const std::string& key)
{
  fS3Key = key;
}

inline void BulkLoad::setS3Secret(const std::string& secret)
{
  fS3Secret = secret;
}

inline void BulkLoad::setS3Bucket(const std::string& bucket)
{
  fS3Bucket = bucket;
}

inline void BulkLoad::setS3Host(const std::string& host)
{
  fS3Host = host;
}

inline void BulkLoad::setS3Region(const std::string& region)
{
  fS3Region = region;
}

inline void BulkLoad::setUsername(const std::string& username)
{
  fUsername = username;
}

inline void BulkLoad::startTimer()
{
  gettimeofday(&fStartTime, nullptr);
}

inline void BulkLoad::stopTimer()
{
  gettimeofday(&fEndTime, nullptr);
  fTotalTime = (fEndTime.tv_sec + (fEndTime.tv_usec / 1000000.0)) -
               (fStartTime.tv_sec + (fStartTime.tv_usec / 1000000.0));
}

inline double BulkLoad::getTotalRunTime() const
{
  return fTotalTime;
}

inline void BulkLoad::disableTimeOut(const bool disableTimeOut)
{
  fDisableTimeOut = disableTimeOut;
}

inline bool BulkLoad::disableTimeOut() const
{
  return fDisableTimeOut;
}

}  // namespace WriteEngine

#undef EXPORT

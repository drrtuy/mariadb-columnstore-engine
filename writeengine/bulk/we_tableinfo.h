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
 * $Id: we_tableinfo.h 4648 2013-05-29 21:42:40Z rdempsey $
 *
 *******************************************************************************/
#pragma once

#include <sys/time.h>
#include <fstream>
#include <utility>
#include <vector>

#include <boost/thread/mutex.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <boost/ptr_container/ptr_vector.hpp>
#pragma GCC diagnostic pop
#include <boost/uuid/uuid.hpp>

#include <libmarias3/marias3.h>

#include "we_type.h"
#include "we_colop.h"
#include "we_fileop.h"
#include "we_blockop.h"
#include "we_brm.h"
#include "we_colbufmgr.h"
#include "we_columninfo.h"
#include "we_bulkloadbuffer.h"
#include "we_rbmetawriter.h"
#include "we_log.h"
#include "we_brmreporter.h"
#include "we_extentstripealloc.h"
#include "messagelog.h"
#include "brmtypes.h"
#include "querytele.h"
#include "oamcache.h"

namespace WriteEngine
{
/* @brief Class which maintains the information for a table.
 */
class TableInfo : public WeUIDGID
{
 private:
  //--------------------------------------------------------------------------
  // Private Data Members
  //--------------------------------------------------------------------------

  int fTableId;         // Table id
  int fBufferSize;      // Size of buffer used by BulkLoadBuffer
  size_t fFileBufSize;  // Size of fFileBuffer passed to setvbuf
  //   to read import files.  Comes from
  //   writeBufferSize tag in job xml file
  char fColDelim;             // Used to delimit col values in a row
  volatile Status fStatusTI;  // Status of table.  Made volatile to
  //   insure BulkLoad methods can access
  //   (thru getStatusTI()) correctly w/o
  //   having to go through a mutex lock.
  int fReadBufCount;  // Number of read buffers
  //   (size of fBuffers vector)
  unsigned fNumberOfColumns;  // Number of ColumnInfo objs in this tbl
  //   (size of fColumns vector)
  FILE* fHandle;           // Handle to the input load file
  int fCurrentReadBuffer;  // Id of current buffer being popu-
  //   lated by the read thread
  RID fTotalReadRows;      // Total number of rows read
  unsigned fTotalErrRows;  // Total error rows among all input
  //   for this table.  Is volatile to
  //   insure parser & reader threads
  //   see the latest value.
  int fMaxErrorRows;   // Maximum error rows
  int fLastBufferId;        // Id of the last buffer
  char* fFileBuffer;        // File buffer passed to setvbuf()
  int fCurrentParseBuffer;  // Id of leading current buffer being
  //   parsed.  There can be more than 1
  //   buffer being parsed concurrently.
  unsigned fNumberOfColsParsed;                // Number of columns completed parsing
  boost::ptr_vector<ColumnInfo> fColumns;      // Columns of the table
  boost::ptr_vector<BulkLoadBuffer> fBuffers;  // Array of read buffers.  Used
  //   to pass data from the read
  //   thread to the write thread(s)

  /* fSyncUpdatesTI is the mutex used to synchronize updates to TableInfo
   * (excluding fErrorRows and fErrDataRows)
   *
   * This mutex is also used to coordinate access to fColumnLocks and
   * fParseComplete (in BulkLoadBuffer) for the buffers within a table.
   * See bulkloadBuffer.h for more information.
   *
   * As the controlling class, TableInfo is the one that is always
   * getting/setting the status of the BulkLoadBuffer objects, so
   * fSyncUpdatesTI is also used to set/get the BulkLoadBuffer status.
   */
  boost::mutex fSyncUpdatesTI;

  boost::mutex fErrorRptInfoMutex;  // Used to synhronize access to
  //   fRejectDataFile & fRejectErrFile
  int fLocker;                             // Read thread id reading this table
  std::vector<std::string> fLoadFileList;  // Load files
  std::string fFileName;                   // Current load file
  std::string fTableName;                  // File name of the table
  OID fTableOID;                           // OID of the table
  std::string fjobFileName;                // Job file name
  int fJobId;                              // Job ID number
  Log* fLog;                               // Object used for logging
  timeval fStartTime;                      // Time when reading and processing began for this table
  const BRM::TxnID fTxnID;                 // Transaction id for the build load
  RBMetaWriter fRBMetaWriter;              // Manages the writing of bulk roll-
  //   back meta data for this table
  std::string fProcessName;  // Name of application process used
  //   in db table locks
  bool fKeepRbMetaFile;  // Keep or delete bulk rollback meta
  //   data file
  bool fbTruncationAsError;        // Treat string truncation as error
  ImportDataMode fImportDataMode;  // Import data in text or binary mode
  long fTimeZone;                  // Timezone offset (in seconds) relative to UTC,
                                   // to use for TIMESTAMP data type. For example,
                                   // for EST which is UTC-5:00, offset will be -18000s.

  volatile bool fTableLocked;  // Do we have db table lock

  bool fReadFromStdin;    // Read import file from STDIN
  bool fReadFromS3;       // Read import file from S3
  std::string fS3Host;    // S3 hostname
  std::string fS3Key;     // S3 key
  std::string fS3Secret;  // S3 secret key
  std::string fS3Bucket;  // S3 bucket
  std::string fS3Region;
  ms3_st* ms3;  // S3 object
  size_t fS3ReadLength;
  size_t fS3ParseLength;
  bool fNullStringMode;  // Treat "NULL" as a null value
  char fEnclosedByChar;  // Character to enclose col values
  char fEscapeChar;      // Escape character used in conjunction with fEnclosedByChar
  size_t fSkipRows;      // Header rows to skip
  size_t fSkipRowsCur;   // Header rows left oto skip in the current file
  bool fProcessingBegun;                 // Has processing begun on this tbl
  BulkModeType fBulkMode;                // Distributed bulk mode (1,2, or 3)
  std::string fBRMRptFileName;           // Name of distributed mode rpt file
  BRMReporter fBRMReporter;              // Object used to report BRM updates
  uint64_t fTableLockID;                 // Unique table lock ID
  std::vector<uint16_t> fOrigDbRootIds;  // List of DBRoots at start of job

  std::string fErrorDir;               // Opt dir for *.err and *.bad files
  std::vector<std::string> fErrFiles;  // List of *.err files for this table
  std::vector<std::string> fBadFiles;  // List of *.bad files for this table
  std::ofstream fRejectDataFile;       // File containing rejected rows
  std::ofstream fRejectErrFile;        // File containing errmsgs for bad rows
  std::string fRejectDataFileName;     // Filename for current fRejectDataFile
  std::string fRejectErrFileName;      // Filename for current fRejectErrFile
  unsigned int fRejectDataCnt;         // Running row count in current bad file
  unsigned int fRejectErrCnt;          // Running count in current err msg file

  ExtentStripeAlloc fExtentStrAlloc;  // Extent stripe allocator for this tbl
  querytele::QueryTeleClient fQtc;    // Query Tele client

  oam::OamCache* fOamCachePtr;              // OamCache: ptr is copyable
  boost::uuids::uuid fJobUUID;              // Job UUID
  std::vector<BRM::LBID_t> fDictFlushBlks;  // dict blks to be flushed from cache

  //--------------------------------------------------------------------------
  // Private Functions
  //--------------------------------------------------------------------------

  int changeTableLockState();            // Change state of table lock to cleanup
  void closeTableFile();                 // Close current tbl file; free buffer
  void closeOpenDbFiles();               // Close DB files left open at job's end
  int confirmDBFileChanges();            // Confirm DB file changes (on HDFS)
  void deleteTempDBFileChanges();        // Delete DB temp swap files (on HDFS)
  int finishBRM();                       // Finish reporting updates for BRM
  void freeProcessingBuffers();          // Free up Processing Buffers
  bool isBufferAvailable(bool report);   // Is tbl buffer available for reading
  int openTableFile();                   // Open data file and set the buffer
  void reportTotals(double elapsedSec);  // Report summary totals
  void sleepMS(long int ms);             // Sleep method
  // Compare column HWM with the examplar HWM.
  int compareHWMs(const int smallestColumnId, const int widerColumnId, const uint32_t smallerColumnWidth,
                  const uint32_t widerColumnWidth, const std::vector<DBRootExtentInfo>& segFileInfo,
                  int& colIdx);

  int synchronizeAutoInc();  // Sychronize AutoInc in BRM with syscat

  // Write the list of errors for this table
  void writeErrorList(const std::vector<std::pair<RID, std::string> >* errorRows,
                      const std::vector<std::string>* errorDatRows, bool bCloseFile);

  // Write out rejected rows, and corresponding error messages
  void writeBadRows(const std::vector<std::string>* errorDatRows, bool bCloseFile);
  void writeErrReason(const std::vector<std::pair<RID, std::string> >* errorRows, bool bCloseFile);

  // Disable copy constructor and assignment operator
  TableInfo(const TableInfo& tableInfo);  //
  TableInfo& operator=(const TableInfo& info);

 public:
  //-------------------------------------------------------------------------
  // Public Functions
  //-------------------------------------------------------------------------

  /** @brief Default constructor
   */
  TableInfo(Log* logger, const BRM::TxnID, const std::string& processName, OID tableOID,
            const std::string& tableName, bool bKeepRbMetaFile);

  /** @brief Default destructor
   */
  ~TableInfo() override;

  /** @brief Acquire the DB table lock for this table
   */
  int acquireTableLock(bool disableTimeOut = false);

  /** @brief Get current table lock ID for this table
   */
  uint64_t getTableLockID() const;

  /** @brief Release the DB table lock for this table
   */
  int releaseTableLock();

  /** @brief Allocate an extent for the specified OID and DBRoot, using the
   *  internal "stripe" allocator.
   *  @param columnOID Allocate next extent for this column
   *  @param dbRoot    Allocate extent on this DBRoot
   *  @param partition (in/out) If DBRoot is empty, this is an input arg,
   *                   else it is assigned by BRM and returned as output
   *  @param segment   (out) Segment number of extent created by BRM
   *  @param startLbid (out) Starting LBID for extent created by BRM
   *  @param allocSize (out) Num blocks allocated to extent by BRM
   *  @param hwm       (out) FBO for extent created by BRM
   *  @param errMsg    (out) Error message
   */
  int allocateBRMColumnExtent(OID columnOID, uint16_t dbRoot, uint32_t& partition, uint16_t& segment,
                              BRM::LBID_t& startLbid, int& allocSize, HWM& hwm, std::string& errMsg);

  /** @brief Delete the bulk rollback metadata file.
   */
  void deleteMetaDataRollbackFile();

  /** @brief Get binary import mode.
   */
  ImportDataMode getImportDataMode() const;

  /** @brief Get timezone.
   */
  long getTimeZone() const;

  /** @brief Get number of buffers
   */
  int getNumberOfBuffers() const;

  /** @brief Set the buffer size
   *  @param Buffer size
   */
  void setBufferSize(const int bufSize);

  /** @brief Set the file buffer size.
   *  @param Buffer size
   */
  void setFileBufferSize(const int fileBufSize);

  /** @brief Set the delimiter used to delimit column values within a row
   */
  void setColDelimiter(const char delim);

  /** @brief Get table status
   */
  Status getStatusTI() const;

  /** @brief Get current parse buffer
   */
  int getCurrentParseBuffer() const;

  /** @brief Get the number of columns
   */
  int getNumberOfColumns() const;

  /** @brief get the file name
   */
  std::string getFileName() const;

  /** @brief Get the number of maximum allowed error rows
   */
  int getMaxErrorRows() const;

  /** @brief retrieve the tuncation as error setting for this
   *  import. When set, this causes char and varchar strings
   *  that are longer than the column definition to be treated
   *  as errors instead of warnings.
   */
  bool getTruncationAsError() const;

  /** @brief set the maximum number of error rows allowed
   */
  void setMaxErrorRows(int maxErrorRows);

  /** @brief Set mode to treat "NULL" string as NULL value or not.
   */
  void setNullStringMode(bool bMode);

  /** @brief Set binary import data mode (text or binary).
   */
  void setImportDataMode(ImportDataMode importMode);

  /** @brief Set timezone.
   */
  void setTimeZone(long timeZone);

  /** @brief Enable distributed mode, saving BRM updates in rptFileName
   */
  void setBulkLoadMode(BulkModeType bulkMode, const std::string& rptFileName);

  /** @brief Set character optionally used to enclose input column values.
   */
  void setEnclosedByChar(char enChar);

  /** @brief Set escape char to use in conjunction with enclosed by char.
   */
  void setEscapeChar(char esChar);

  /** @brief Set how many header rows should be skipped.
   */
  void setSkipRows(size_t skipRows);

  /** @brief Has processing begun for this table.
   */
  bool hasProcessingBegun();

  /** @brief set the table id
   */
  void setTableId(const int& id);

  /** @brief get the file name
   */
  std::string getTableName() const;

  /** @brief get the table OID
   */
  OID getTableOID();

  /** @brief Set the directory for *.err and *.bad files. May be
   *  	   empty string, in which case we use current dir.
   */
  void setErrorDir(const std::string& errorDir);

  /** @brief get the bulk rollback meta data writer object for this table
   */
  RBMetaWriter* rbMetaWriter();

  /** @brief Add column information to the table
   */
  void addColumn(ColumnInfo* info);

  /** @brief Initialize the buffer list
   *  @param noOfBuffers Number of buffers to create for this table
   *  @param jobFieldRefList List of fields in this import
   *  @param fixedBinaryRecLen In binary mode, this is the fixed record length
   *         used to read the buffer; in text mode, this value is not used.
   */
  int initializeBuffers(int noOfBuffers, const JobFieldRefList& jobFieldRefList,
                        unsigned int fixedBinaryRecLen);

  /** @brief Read the table data into the read buffer
   */
  int readTableData();

  /** @brief parse method
   */
  int parseColumn(const int& columnId, const int& bufferId, double& processingTime);

  /** @brief update the buffer status for column
   */
  int setParseComplete(const int& columnId, const int& bufferId, double processingTime);

  /** @brief update the status to reflect a parsing error
   */
  void setParseError();

  /** @brief Check if buffer ready for parsing.
   */
  bool bufferReadyForParse(const int& bufferId, bool report) const;

  /** @brief Check if a column is available for parsing in the buffer
   *  and return the column id if available
   */
  int getColumnForParse(const int& id, const int& bufferId, bool report);

  /** @brief Do we have a db lock with the session manager for this table.
   */
  bool isTableLocked();

  /** @brief Lock the table for reading
   */
  bool lockForRead(const int& locker);

  /** @brief Rollback changes made to "this" table by the current import job
   */
  int rollbackWork();

  /** @brief set list of import files and STDIN usage flag
   */
  void setLoadFilesInput(bool bReadFromStdin, bool bReadFromS3, const std::vector<std::string>& files,
                         const std::string& s3host, const std::string& s3key, const std::string& s3secret,
                         const std::string& s3bucket, const std::string& s3region);

  /** @brief set job file name under process.
   */
  void setJobFileName(const std::string& jobFileName);

  /** @brief set job ID for this import.
   */
  void setJobId(int jobId);

  /** @brief set truncation as error for this import.
   *  When set, this causes char and varchar strings that are
   *  longer than the column definition to be treated as errors
   *  instead of warnings.
   */
  void setTruncationAsError(bool bTruncationAsError);

  /** @brief log message to data_mods.log file.
   */
  void logToDataMods(const std::string& jobFile, const std::string& messageText);

  /** @brief Validate consistency of current HWMs for this table's columns.
   *  If jobTable argument is provided, then it will be used to get additional
   *  column info, else this table's fColumns vector is used.
   *  "stage" indicates validation stage ("Starting" or "Ending" HWMs).
   */
  int validateColumnHWMs(const JobTable* jobTable, const std::vector<DBRootExtentInfo>& segFileInfo,
                         const char* stage);

  /** @brief Initialize the bulk rollback meta data writer for this table.
   */
  int initBulkRollbackMetaData();

  /** @brief Save meta data information for bulk rollback.
   * This is the Shared-Nothing version of this function.
   *  @param job Input Job information
   *  @param segFileInfo vector of starting segment files for each column
   *  @param dbRootHWMInfoColVec Vector of last local HWMs for each DBRoot
   *  on this PM.
   */
  int saveBulkRollbackMetaData(Job& job, const std::vector<DBRootExtentInfo>& segFileInfo,
                               const std::vector<BRM::EmDbRootHWMInfo_v>& dbRootHWMInfoColVec);

  /** @brief Mark table as complete
   */
  void markTableComplete();

  void setJobUUID(const boost::uuids::uuid& jobUUID);

  bool readFromSTDIN();

 public:
  friend class BulkLoad;
  friend class ColumnInfo;
  friend class ColumnInfoCompressed;
};

//------------------------------------------------------------------------------
// Inline functions
//------------------------------------------------------------------------------
inline int TableInfo::getCurrentParseBuffer() const
{
  return fCurrentParseBuffer;
}

inline std::string TableInfo::getFileName() const
{
  return fFileName;
}

inline ImportDataMode TableInfo::getImportDataMode() const
{
  return fImportDataMode;
}

inline long TableInfo::getTimeZone() const
{
  return fTimeZone;
}

inline int TableInfo::getNumberOfBuffers() const
{
  return fReadBufCount;
}

inline int TableInfo::getNumberOfColumns() const
{
  return fNumberOfColumns;
}

inline Status TableInfo::getStatusTI() const
{
  return fStatusTI;
}

inline int TableInfo::getMaxErrorRows() const
{
  return fMaxErrorRows;
}

inline uint64_t TableInfo::getTableLockID() const
{
  return fTableLockID;
}

inline std::string TableInfo::getTableName() const
{
  return fTableName;
}

inline OID TableInfo::getTableOID()
{
  return fTableOID;
}

inline bool TableInfo::getTruncationAsError() const
{
  return fbTruncationAsError;
}

inline bool TableInfo::hasProcessingBegun()
{
  return fProcessingBegun;
}

inline bool TableInfo::isTableLocked()
{
  return fTableLocked;
}

inline void TableInfo::markTableComplete()
{
  boost::mutex::scoped_lock lock(fSyncUpdatesTI);
  fStatusTI = WriteEngine::PARSE_COMPLETE;
}

inline RBMetaWriter* TableInfo::rbMetaWriter()
{
  return &fRBMetaWriter;
}

inline void TableInfo::setBufferSize(const int bufSize)
{
  fBufferSize = bufSize;
}

inline void TableInfo::setColDelimiter(const char delim)
{
  fColDelim = delim;
}

inline void TableInfo::setBulkLoadMode(BulkModeType bulkMode, const std::string& rptFileName)
{
  fBulkMode = bulkMode, fBRMRptFileName = rptFileName;
}

inline void TableInfo::setEnclosedByChar(char enChar)
{
  fEnclosedByChar = enChar;
}

inline void TableInfo::setEscapeChar(char esChar)
{
  fEscapeChar = esChar;
}

inline void TableInfo::setSkipRows(size_t skipRows)
{
  fSkipRows = skipRows;
}


inline void TableInfo::setFileBufferSize(const int fileBufSize)
{
  fFileBufSize = fileBufSize;
}

inline void TableInfo::setImportDataMode(ImportDataMode importMode)
{
  fImportDataMode = importMode;
}

inline void TableInfo::setTimeZone(long timeZone)
{
  fTimeZone = timeZone;
}

inline void TableInfo::setJobFileName(const std::string& jobFileName)
{
  fjobFileName = jobFileName;
}

inline void TableInfo::setJobId(int jobId)
{
  fJobId = jobId;
}

inline void TableInfo::setLoadFilesInput(bool bReadFromStdin, bool bReadFromS3,
                                         const std::vector<std::string>& files, const std::string& s3host,
                                         const std::string& s3key, const std::string& s3secret,
                                         const std::string& s3bucket, const std::string& s3region)
{
  fReadFromStdin = bReadFromStdin;
  fReadFromS3 = bReadFromS3;
  fLoadFileList = files;
  fS3Host = s3host;
  fS3Key = s3key;
  fS3Secret = s3secret;
  fS3Bucket = s3bucket;
  fS3Region = s3region;
}

inline void TableInfo::setMaxErrorRows(int maxErrorRows)
{
  fMaxErrorRows = maxErrorRows;
}

inline void TableInfo::setNullStringMode(bool bMode)
{
  fNullStringMode = bMode;
}

inline void TableInfo::setTableId(const int& id)
{
  fTableId = id;
}

inline void TableInfo::setTruncationAsError(bool bTruncationAsError)
{
  fbTruncationAsError = bTruncationAsError;
}

inline void TableInfo::setJobUUID(const boost::uuids::uuid& jobUUID)
{
  fJobUUID = jobUUID;
}

inline void TableInfo::setErrorDir(const std::string& errorDir)
{
  fErrorDir = errorDir;

  if (fErrorDir.length() > 0 && *(--(fErrorDir.end())) != '/')
    fErrorDir.push_back('/');
}
}  // namespace WriteEngine

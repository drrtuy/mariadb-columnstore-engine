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

//  $Id: writeengine.h 4726 2013-08-07 03:38:36Z bwilkinson $

/** @file */

#pragma once
#include <cstdio>
#include <string>

// the header file for fd
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
// end
#include <boost/lexical_cast.hpp>
#include <tr1/unordered_set>

#include "we_brm.h"
#include "we_colop.h"
#include "we_index.h"
#include "we_tablemetadata.h"
#include "we_dbrootextenttracker.h"
#include "we_rbmetawriter.h"
#include "brmtypes.h"
#include "we_chunkmanager.h"

#define IO_BUFF_SIZE 81920

#define EXPORT
/** Namespace WriteEngine */
namespace WriteEngine
{
//... Total compression operation: un_compresssed, compressed
const int UN_COMPRESSED_OP = 0;
const int COMPRESSED_OP_1 = 1;
const int COMPRESSED_OP_2 = 2;
const int TOTAL_COMPRESS_OP = 3;

//...Forward class declarations
class Log;

// Bug4312. During transactions, we need to mark each extent modified as invalid.
// In order to prevent thrashing, marking the same extent everytime we get an lbid
// for an extent, we remember the starting lbid for each extent marked for the
// transaction. We also add a sequence number so we can age them out of the list
// for truly long running transactions.
struct TxnLBIDRec
{
  std::tr1::unordered_set<BRM::LBID_t> m_LBIDSet;
  std::vector<BRM::LBID_t> m_LBIDs;
  std::vector<execplan::CalpontSystemCatalog::ColDataType> m_ColDataTypes;

  TxnLBIDRec() = default;
  ~TxnLBIDRec() = default;
  void AddLBID(BRM::LBID_t lbid, const execplan::CalpontSystemCatalog::ColDataType& colDataType)
  {
    if (m_LBIDSet.insert(lbid).second)
    {
      m_LBIDs.push_back(lbid);
      m_ColDataTypes.push_back(colDataType);
    }
  }
};

typedef boost::shared_ptr<TxnLBIDRec> SP_TxnLBIDRec_t;
typedef std::set<BRM::LBID_t> dictLBIDRec_t;

/** @brief Range information for 1 or 2 extents changed by DML operation. */
struct ColSplitMaxMinInfo
{
  ExtCPInfo fSplitMaxMinInfo[2];      /** @brief internal to write engine: min/max ranges for data in one and,
                                         possible, second extent. */
  ExtCPInfo* fSplitMaxMinInfoPtrs[2]; /** @brief pointers to CPInfos in fSplitMaxMinInfo above */
  ColSplitMaxMinInfo(execplan::CalpontSystemCatalog::ColDataType colDataType, int colWidth)
   : fSplitMaxMinInfo{ExtCPInfo(colDataType, colWidth), ExtCPInfo(colDataType, colWidth)}
  {
    fSplitMaxMinInfoPtrs[0] = fSplitMaxMinInfoPtrs[1] = nullptr;  // disable by default.
  }
};

typedef std::vector<ColSplitMaxMinInfo> ColSplitMaxMinInfoList;

/** Class WriteEngineWrapper */
class WriteEngineWrapper : public WEObj
{
 public:
  /**
   * @brief Constructor
   */
  EXPORT WriteEngineWrapper();

  EXPORT WriteEngineWrapper(const WriteEngineWrapper& rhs);
  /**
   * @brief Default Destructor
   */
  EXPORT ~WriteEngineWrapper();

  /************************************************************************
   * Interface definitions
   ************************************************************************/
  /**
   * @brief Performs static/global initialization for BRMWrapper.
   * Should be called once from the main thread.
   */
  EXPORT static void init(unsigned subSystemID);

  /**
   * @brief Build a index from an oid file (NOTE: this is write engine internal used function, just for test
   * purpose and not for generic use
   */
  int buildIndex(const OID& /*colOid*/, const OID& /*treeOid*/, const OID& /*listOid*/,
                 execplan::CalpontSystemCatalog::ColDataType /*colDataType*/, int /*width*/, int /*hwm*/, bool /*resetFile*/,
                 uint64_t& /*totalRows*/, int /*maxRow*/ = IDX_DEFAULT_READ_ROW)
  {
    return -1;
  }

  /**
   * @brief Build a index from a file
   */
  int buildIndex(const std::string& /*sourceFileName*/, const OID& /*treeOid*/, const OID& /*listOid*/,
                 execplan::CalpontSystemCatalog::ColDataType /*colDataType*/, int /*width*/, int /*hwm*/, bool /*resetFile*/,
                 uint64_t& /*totalRows*/, const std::string& /*indexName*/, Log* /*pLogger*/,
                 int /*maxRow*/ = IDX_DEFAULT_READ_ROW)
  {
    return -1;
  }

  /**
   * @brief Close a index file
   */
  void closeIndex()
  {
  }

  /**
   * @brief Close a dictionary
   */
  int closeDctnry(const TxnID& /*txnid*/, int i, bool realClose = true)
  {
    return m_dctnry[op(i)]->closeDctnry(realClose);
  }

  /**
   * @brief Commit transaction
   */
  int commit(const TxnID& txnid)
  {
    m_txnLBIDMap.erase(txnid);
    return BRMWrapper::getInstance()->commit(txnid);
  }

  /**
   * @brief Convert interface value list to internal value array
   */
  EXPORT void convertValArray(const size_t totalRow,
                              const execplan::CalpontSystemCatalog::ColType& cscColType,
                              const ColType colType, ColTupleList& curTupleList, void* valArray,
                              bool bFromList = true);
  /**
   * @brief Updates range information given old range information, old values, new values and column
   * information.
   */
  EXPORT void updateMaxMinRange(const size_t totalNewRow, const size_t totalOldRow,
                                const execplan::CalpontSystemCatalog::ColType& cscColType,
                                const ColType colType, const void* valArray, const void* oldValArray,
                                ExtCPInfo* maxMin, bool canStartWithInvalidRange);

  /**
   * @brief Create a column, include object ids for column data and bitmap files
   * @param dataOid column datafile object id
   * @param dataType column data type
   * @param dataWidth column width
   * @param dbRoot DBRoot under which file is to be located (1-based)
   * @param partition Starting partition number for segment file path (0-based).
   * @param compressionType compression type
   */
  EXPORT int createColumn(const TxnID& txnid, const OID& dataOid,
                          execplan::CalpontSystemCatalog::ColDataType dataType, int dataWidth,
                          uint16_t dbRoot, uint32_t partition = 0, int compressionType = 0);

  // BUG931
  /**
   * @brief Fill a new column with default value using row-ids from a reference column
   *
   * @param txnid Transaction id
   * @param dataOid OID of the new column
   * @param dataType Data-type of the new column
   * @param dataWidth Width of the new column
   * @param defaultVal Default value to be filled in the new column
   * @param refColOID OID of the reference column
   * @param refColDataType Data-type of the referecne column
   * @param refColWidth Width of the reference column
   */
  EXPORT int fillColumn(const TxnID& txnid, const OID& dataOid,
                        const execplan::CalpontSystemCatalog::ColType& colType, ColTuple defaultVal,
                        const OID& refColOID, execplan::CalpontSystemCatalog::ColDataType refColDataType,
                        int refColWidth, int refCompressionType, bool isNULL, int compressionType,
                        const std::string& defaultValStr, const OID& dictOid = 0, bool autoincrement = false);

  /**
   * @brief Create a index related files, include object ids for index tree and list files

   * @param treeOid index tree file object id
   * @param listOid index list file object id
   */
  int createIndex(const TxnID& /*txnid*/, const OID& /*treeOid*/, const OID& /*listOid*/)
  {
    int rc = -1;
    return rc;
  }

  /**
   * @brief Create dictionary
   * @param dctnryOid dictionary signature file object id
   * @param partition Starting partition number for segment file path (0-based).
   * @param segment segment number
   * @param compressionType compression type
   */
  EXPORT int createDctnry(const TxnID& txnid, const OID& dctnryOid, int colWidth, uint16_t dbRoot,
                          uint32_t partiotion = 0, uint16_t segment = 0, int compressionType = 0);

  /**
   * @brief Delete a list of rows from a table
   * @param colStructList column struct list
   * @param colOldValueList column old values list (return value)
   * @param rowIdList row id list
   */
  EXPORT int deleteRow(const TxnID& txnid, const std::vector<CSCTypesList>& colExtentsColType,
                       std::vector<ColStructList>& colExtentsStruct, std::vector<void*>& colOldValueList,
                       std::vector<RIDList>& ridLists, const int32_t tableOid, bool hasAUXCol = false);

  /**
   * @brief Delete a list of rows from a table
   * @param colStructList column struct list
   * @param rowIdList row id list
   */

  EXPORT int deleteBadRows(const TxnID& txnid, ColStructList& colStructs, RIDList& ridList,
                           DctnryStructList& dctnryStructList);

  /**
   * @brief delete a dictionary signature and its token
   * @param dctnryStruct dictionary structure
   * @param dctnryTuple dictionary tuple
   */
  // ITER17_Obsolete
  // int deleteToken(const TxnID& txnid, Token& token); // Files need already open
  // int deleteToken(const TxnID& txnid, DctnryStruct& dctnryStruct, Token& token);

  /**
   * @brief Drop a column, include object ids for column data file
   * @param dataOid column datafile object id
   */
  int dropColumn(const TxnID& /*txnid*/, const OID dataOid)
  {
    return m_colOp[0]->dropColumn((FID)dataOid);
  }

  /**
   * @brief Drop files
   * @param dataOids column and dictionary datafile object id
   */
  int dropFiles(const TxnID& /*txnid*/, const std::vector<int32_t>& dataOids)
  {
    return m_colOp[0]->dropFiles(dataOids);
  }

  /**
   * @brief Delete files for one partition
   * @param dataOids column and dictionary datafile object id
   */
  int deletePartitions(const std::vector<OID>& dataOids, const std::vector<BRM::PartitionInfo>& partitions)
  {
    return m_colOp[0]->dropPartitions(dataOids, partitions);
  }

  int deleteOIDsFromExtentMap(const TxnID& /*txnid*/, const std::vector<int32_t>& dataOids)
  {
    return m_colOp[0]->deleteOIDsFromExtentMap(dataOids);
  }

  /**
   * @brief Create a index related files, include object ids for index tree and list files
   * @param treeOid index tree file object id
   * @param listOid index list file object id
   */
  int dropIndex(const TxnID& /*txnid*/, const OID& /*treeOid*/, const OID& /*listOid*/)
  {
    return -1;
  }

  /**
   * @brief Drop a dictionary
   * @param dctnryOid dictionary signature file object id
   * @param treeOid dictionary tree file object id
   * @param listOid index list file object id
   */
  int dropDctnry(const TxnID& /*txnid*/, const OID& dctnryOid, const OID& /*treeOid*/, const OID& /*listOid*/)
  {
    return m_dctnry[0]->dropDctnry(dctnryOid);
  }

  /**
   * @brief Flush VM write cache
   * @param None
   */
  EXPORT void flushVMCache() const;

  /**
   * @brief Insert values into a table
   * @param colStructList column structure list
   * @param colValueList column value list
   * @param dicStringListt dictionary values list
   * @param dbRootExtentTrackers dbrootTrackers
   * @param bFirstExtentOnThisPM true when there is no extent on this PM
   * @param insertSelect if insert with select, the hwm block is skipped
   * @param isAutoCommitOn if autocommit on, only the hwm block is versioned,
   *        else eveything is versioned
   * @param tableOid used to get table meta data
   * @param isFirstBatchPm to track if this batch is first batch for this PM.
   */
  EXPORT int insertColumnRecs(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                              ColStructList& colStructList, ColValueList& colValueList,
                              DctnryStructList& dctnryStructList, DictStrList& dictStrList,
                              std::vector<boost::shared_ptr<DBRootExtentTracker> >& dbRootExtentTrackers,
                              RBMetaWriter* fRBMetaWriter, bool bFirstExtentOnThisPM,
                              bool insertSelect = false, bool isAutoCommitOn = false, OID tableOid = 0,
                              bool isFirstBatchPm = false);

  EXPORT int insertColumnRecsBinary(
      const TxnID& txnid, ColStructList& colStructList, std::vector<uint64_t>& colValueList,
      DctnryStructList& dctnryStructList, DictStrList& dictStrList,
      std::vector<boost::shared_ptr<DBRootExtentTracker> >& dbRootExtentTrackers, RBMetaWriter* fRBMetaWriter,
      bool bFirstExtentOnThisPM, bool insertSelect = false, bool isAutoCommitOn = false, OID tableOid = 0,
      bool isFirstBatchPm = false);

  /**
   * @brief Insert values into systables
   * @param colStructList column structure list
   * @param colValueList column value list
   * @param dicStringListt dictionary values list
   */
  EXPORT int insertColumnRec_SYS(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                                 ColStructList& colStructList, ColValueList& colValueList,
                                 DctnryStructList& dctnryStructList, DictStrList& dictStrList,
                                 const int32_t tableOid);

  /**
   * @brief Insert a row
   * @param colStructList column structure list
   * @param colValueList column value list
   * @param dicStringListt dictionary values list
   */
  EXPORT int insertColumnRec_Single(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                                    ColStructList& colStructList, ColValueList& colValueList,
                                    DctnryStructList& dctnryStructList, DictStrList& dictStrList,
                                    const int32_t tableOid);
  /**
   * @brief Open dictionary
   * @param txnid relevant transaction
   * @param dctnryStruct dictionary column to open
   * @param useTmpSuffix Bulk HDFS usage: use *.tmp file suffix
   */
  // @bug 5572 - HDFS usage: add *.tmp file backup flag
  int openDctnry(const TxnID& txnid, DctnryStruct dctnryStruct, bool useTmpSuffix)
  {
    int compress_op = op(dctnryStruct.fCompressionType);
    m_dctnry[compress_op]->setTransId(txnid);
    return m_dctnry[compress_op]->openDctnry(dctnryStruct.dctnryOid, dctnryStruct.fColDbRoot,
                                             dctnryStruct.fColPartition, dctnryStruct.fColSegment,
                                             useTmpSuffix);
  }

  /**
   * @brief Rollback transaction (common portion)
   */
  EXPORT int rollbackCommon(const TxnID& txnid, int sessionId);

  /**
   * @brief Rollback transaction
   */
  EXPORT int rollbackTran(const TxnID& txnid, int sessionId);

  /**
   * @brief Rollback transaction
   */
  EXPORT int rollbackBlocks(const TxnID& txnid, int sessionId);

  /**
   * @brief Rollback transaction
   */
  EXPORT int rollbackVersion(const TxnID& txnid, int sessionId);

  /**
   * @brief Set the IsInsert flag in the ChunkManagers.
   * This forces flush at end of block. Used only for bulk insert.
   */
  void setIsInsert(bool bIsInsert)
  {
    m_colOp[COMPRESSED_OP_1]->chunkManager()->setIsInsert(bIsInsert);
    m_dctnry[COMPRESSED_OP_1]->chunkManager()->setIsInsert(true);
    m_colOp[COMPRESSED_OP_2]->chunkManager()->setIsInsert(bIsInsert);
    m_dctnry[COMPRESSED_OP_2]->chunkManager()->setIsInsert(true);
  }

  /**
   * @brief Get the IsInsert flag as set in the ChunkManagers.
   * Since both chunk managers are supposed to be in lockstep as regards the
   * isInsert flag, we need only grab one.
   *
   */
  bool getIsInsert()
  {
    return m_colOp[COMPRESSED_OP_1]->chunkManager()->getIsInsert();
  }

  std::tr1::unordered_map<TxnID, SP_TxnLBIDRec_t>& getTxnMap()
  {
    return m_txnLBIDMap;
  };
  std::tr1::unordered_map<TxnID, dictLBIDRec_t>& getDictMap()
  {
    return m_dictLBIDMap;
  };
  /**
   * @brief Flush the ChunkManagers.
   */
  int flushChunks(int rc, const std::map<FID, FID>& columOids)
  {
    std::vector<int32_t> compressedOpIds = {COMPRESSED_OP_1, COMPRESSED_OP_2};

    for (const auto compressedOpId : compressedOpIds)
    {
      auto rtn = m_colOp[compressedOpId]->chunkManager()->flushChunks(rc, columOids);
      if (rtn != NO_ERROR)
        return rtn;

      rtn = m_dctnry[compressedOpId]->chunkManager()->flushChunks(rc, columOids);
      if (rtn != NO_ERROR)
        return rtn;
    }

    return NO_ERROR;
  }

  /**
   * @brief Set the transaction id into all fileops
   */
  void setTransId(const TxnID& txnid)
  {
    for (int i = 0; i < TOTAL_COMPRESS_OP; i++)
    {
      m_colOp[i]->setTransId(txnid);
      m_dctnry[i]->setTransId(txnid);
    }
  }

  /**
   * @brief Set the fIsBulk id into all fileops
   */
  void setBulkFlag(bool isBulk)
  {
    for (int i = 0; i < TOTAL_COMPRESS_OP; i++)
    {
      m_colOp[i]->setBulkFlag(isBulk);
      m_dctnry[i]->setBulkFlag(isBulk);
    }
  }

  /**
   * @brief Set the fIsFix  into all fileops
   */
  void setFixFlag(bool isFix = false)
  {
    for (int i = 0; i < TOTAL_COMPRESS_OP; i++)
    {
      m_colOp[i]->setFixFlag(isFix);
      m_dctnry[i]->setFixFlag(isFix);
    }
  }

  /**
   * @brief let chunkmanager start transaction.
   *
   */
  int startTransaction(const TxnID& txnid)
  {
    int rc = 0;
    rc = m_colOp[COMPRESSED_OP_1]->chunkManager()->startTransaction(txnid);
    // if ( rc == 0)
    //	rc = m_dctnry[COMPRESSED_OP]->chunkManager()->startTransaction(txnid);
    return rc;
  }

  /**
   * @brief let chunkmanager confirm transaction.
   *
   */
  int confirmTransaction(const TxnID& txnid)
  {
    int rc = 0;
    rc = m_colOp[COMPRESSED_OP_1]->chunkManager()->confirmTransaction(txnid);
    return rc;
  }

  /**
   * @brief let chunkmanager end transaction.
   *
   */
  int endTransaction(const TxnID& txnid, bool success)
  {
    int rc = 0;
    rc = m_colOp[COMPRESSED_OP_1]->chunkManager()->endTransaction(txnid, success);
    // if ( rc == 0)
    //	rc = m_dctnry[COMPRESSED_OP]->chunkManager()->endTransaction(txnid, success);
    return rc;
  }

  /**
   * @brief Tokenize a dictionary signature into a token
   * @param dctnryStruct dictionary structure
   * @param dctnryTuple dictionary tuple
   * @param useTmpSuffix Bulk HDFS usage: use *.tmp file suffix
   */
  EXPORT int tokenize(const TxnID& txnid, DctnryTuple&, int compType);  // Files need open first
  EXPORT int tokenize(const TxnID& txnid, DctnryStruct& dctnryStruct, DctnryTuple& dctnryTuple,
                      bool useTmpSuffix);

  /**
   * @brief Update values into a column (New one)
   * @param colStructList column structure list
   * @param colValueList column value list
   * @param colOldValueList column old values list (return value)
   * @param ridList row id list
   */
  EXPORT int updateColumnRec(const TxnID& txnid, const std::vector<CSCTypesList>& colExtentsColType,
                             std::vector<ColStructList>& colExtentsStruct, ColValueList& colValueList,
                             std::vector<void*>& colOldValueList, std::vector<RIDList>& ridLists,
                             std::vector<DctnryStructList>& dctnryExtentsStruct,
                             DctnryValueList& dctnryValueList, const int32_t tableOid,
                             bool hasAUXCol = false);

  /**
   * @brief Update values into columns
   * @param colStructList column structure list
   * @param colValueList column value list
   * @param ridList row id list
   */

  EXPORT int updateColumnRecs(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                              std::vector<ColStruct>& colStructList, ColValueList& colValueList,
                              const RIDList& ridLists, const int32_t tableOid);

  /**
   * @brief Release specified table lock.
   * @param lockID Table lock id to be released.
   * @param errorMsg Return error message
   */
  EXPORT int clearTableLockOnly(uint64_t lockID, std::string& errorMsg);

  /**
   * @brief Rollback the specified table
   * @param tableOid Table to be rolled back
   * @param lockID Table lock id of the table to be rolled back.
   *        Currently used for logging only.
   * @param tableName Name of table associated with tableOid.
   *        Currently used for logging only.
   * @param applName Application that is driving this bulk rollback.
   *        Currently used for logging only.
   * @param debugConsole Enable logging to console
   * @param errorMsg Return error message
   */
  EXPORT int bulkRollback(OID tableOid, uint64_t lockID, const std::string& tableName,
                          const std::string& applName, bool debugConsole, std::string& errorMsg);

  /**
   * @brief update SYSCOLUMN next value
   * @param oidValueMap
   */
  EXPORT int updateNextValue(const TxnID txnId, const OID& columnoid, const uint64_t nextVal,
                             const uint32_t sessionID, const uint16_t dbRoot);

  /**
   * @brief write active datafiles to disk
   *
   */
  EXPORT int flushDataFiles(int rc, const TxnID txnId, std::map<FID, FID>& columnOids);

  /**
   * @brief Process versioning for batch insert - only version the hwm block.
   */
  EXPORT int processBatchVersions(const TxnID& txnid, std::vector<Column> columns,
                                  std::vector<BRM::LBIDRange>& rangeList);

  EXPORT void writeVBEnd(const TxnID& txnid, std::vector<BRM::LBIDRange>& rangeList);

  /************************************************************************
   * Future implementations
   ************************************************************************/
  /**
   * @brief Begin transaction
   */
  // todo: add implementation when we work on version control
  // int beginTran(const TransID transOid) { return NO_ERROR; }

  /**
   * @brief End transaction
   */
  // todo: add implementation when we work on version control
  // int endTran(const TransID transOid) { return NO_ERROR; }
  // WIP
  void setDebugLevel(const DebugLevel level)
  {
    WEObj::setDebugLevel(level);

    for (int i = 0; i < TOTAL_COMPRESS_OP; i++)
    {
      m_colOp[i]->setDebugLevel(level);
      m_dctnry[i]->setDebugLevel(level);
    }
  }  // todo: cleanup

  /************************************************************************
   * Internal use definitions
   ************************************************************************/
 private:
  /**
   * @brief Check whether the passing parameters are valid
   */
  int checkValid(const TxnID& txnid, const ColStructList& colStructList, const ColValueList& colValueList,
                 const RIDList& ridList) const;

  /**
   * @brief Find the smallest column for this table
   */
  void findSmallestColumn(uint32_t& colId, ColStructList colStructList);

  /**
   * @brief Convert interface column type to an internal column type
   */
  void convertValue(const execplan::CalpontSystemCatalog::ColType& cscColType, ColType colType,
                    void* valArray, size_t pos, boost::any& data, bool fromList = true);

  /**
   * @brief Convert column value to its internal representation
   *
   * @param colType Column data-type
   * @param value Memory pointer for storing output value. Should be pre-allocated
   * @param data Column data
   */
  void convertValue(const execplan::CalpontSystemCatalog::ColType& cscColType, const ColType colType,
                    void* value, boost::any& data);

  /**
   * @brief Print input value from DDL/DML processors
   */
  void printInputValue(const ColStructList& colStructList, const ColValueList& colValueList,
                       const RIDList& ridList, const DctnryStructList& dctnryStructList,
                       const DictStrList& dictStrList) const;

  /**
   * @brief Process version buffer
   */
  int processVersionBuffer(IDBDataFile* pFile, const TxnID& txnid, const ColStruct& colStruct, int width,
                           int totalRow, const RID* rowIdArray, std::vector<BRM::LBIDRange>& rangeList);

  /**
   * @brief Process version buffers for update and delete @Bug 1886,2870
   */
  int processVersionBuffers(IDBDataFile* pFile, const TxnID& txnid, const ColStruct& colStruct, int width,
                            int totalRow, const RIDList& ridList, std::vector<BRM::LBIDRange>& rangeList);

  int processBeginVBCopy(const TxnID& txnid, const std::vector<ColStruct>& colStructList,
                         const RIDList& ridList, std::vector<BRM::VBRange>& freeList,
                         std::vector<std::vector<uint32_t> >& fboLists,
                         std::vector<std::vector<BRM::LBIDRange> >& rangeLists,
                         std::vector<BRM::LBIDRange>& rangeListTot);

  /**
   * @brief Common methods to write values to a column
   */
  int writeColumnRec(const TxnID& txnid, const CSCTypesList& cscColTypes, const ColStructList& colStructList,
                     ColValueList& colValueList, RID* rowIdArray, const ColStructList& newColStructList,
                     ColValueList& newColValueList, const int32_t tableOid, bool useTmpSuffix,
                     bool versioning = true, ColSplitMaxMinInfoList* maxMins = nullptr);

  int writeColumnRecBinary(const TxnID& txnid, const ColStructList& colStructList,
                           std::vector<uint64_t>& colValueList, RID* rowIdArray,
                           const ColStructList& newColStructList, std::vector<uint64_t>& newColValueList,
                           const int32_t tableOid, bool useTmpSuffix, bool versioning = true);

  //@Bug 1886,2870 pass the address of ridList vector
  int writeColumnRecUpdate(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                           const ColStructList& colStructList, const ColValueList& colValueList,
                           std::vector<void*>& colOldValueList, const RIDList& ridList,
                           const int32_t tableOid, bool convertStructFlag = true,
                           ColTupleList::size_type nRows = 0, std::vector<ExtCPInfo*>* cpInfos = nullptr,
                           bool hasAUXCol = false);

  // For update column from column to use
  int writeColumnRecords(const TxnID& txnid, const CSCTypesList& cscColTypeList,
                         std::vector<ColStruct>& colStructList, ColValueList& colValueList,
                         const RIDList& ridLists, const int32_t tableOid, bool versioning = true,
                         std::vector<ExtCPInfo*>* cpInfos = nullptr);

  /**
   * @brief util method to convert rowid to a column file
   *
   */
  int convertRidToColumn(RID& rid, uint16_t& dbRoot, uint32_t& partition, uint16_t& segment,
                         const RID filesPerColumnPartition, const RID extentsPerSegmentFile,
                         const RID extentRows, uint16_t startDBRoot, unsigned dbrootCnt);

  void AddDictToList(const TxnID txnid, std::vector<BRM::LBID_t>& lbids);
  void RemoveTxnFromDictMap(const TxnID txnid);

  // Bug 4312: We use a hash map to hold the set of starting LBIDS for a given
  // txn so that we don't waste time marking the same extent as invalid. This
  // list should be trimmed if it gets too big.
  int AddLBIDtoList(const TxnID txnid, const ColStruct& colStruct, const int fbo,
                    ExtCPInfo* cpInfo = nullptr  // provide CPInfo pointer if you want max/min updated.
  );

  // Get CPInfo for given starting LBID and column description structure.
  int GetLBIDRange(const BRM::LBID_t startingLBID, const ColStruct& colStruct, ExtCPInfo& cpInfo);

  // mark extents of the transaction as invalid. erase transaction from txn->lbidsrec map if requested.
  int markTxnExtentsAsInvalid(const TxnID txnid, bool erase = false);

  // write LBID's new ranges.
  int setExtentsNewMaxMins(const ColSplitMaxMinInfoList& maxMins, bool haveSplit);

  int RemoveTxnFromLBIDMap(const TxnID txnid);

  int op(int compressionType)
  {
    switch (compressionType)
    {
      case 1:
      case 2: return COMPRESSED_OP_1;
      case 3: return COMPRESSED_OP_2;
    }

    return 0;
  }

  // This is a Map of sets of LBIDS for each transaction. A Transaction's list will be removed upon commit or
  // rollback.
  std::tr1::unordered_map<TxnID, SP_TxnLBIDRec_t> m_txnLBIDMap;

  // MCOL-1160: We need to track dictionary LBIDs so we can tell PrimProc
  // to flush the blocks after an API bulk-write.
  std::tr1::unordered_map<TxnID, dictLBIDRec_t> m_dictLBIDMap;

  ColumnOp* m_colOp[TOTAL_COMPRESS_OP];  // column operations
  Dctnry* m_dctnry[TOTAL_COMPRESS_OP];   // dictionary operations
  OpType m_opType;                       // operation type
};

}  // namespace WriteEngine

#undef EXPORT

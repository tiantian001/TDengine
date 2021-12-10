/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "dataBlockMgt.h"

// #include "astGenerator.h"
// #include "parserInt.h"
#include "catalog.h"
#include "parserUtil.h"
#include "queryInfoUtil.h"
// #include "ttoken.h"
// #include "function.h"
// #include "ttime.h"
// #include "tglobal.h"
#include "taosmsg.h"

#define IS_RAW_PAYLOAD(t) \
  (((int)(t)) == PAYLOAD_TYPE_RAW)  // 0: K-V payload for non-prepare insert, 1: rawPayload for prepare insert

static int32_t rowDataCompar(const void *lhs, const void *rhs) {
  TSKEY left = *(TSKEY *)lhs;
  TSKEY right = *(TSKEY *)rhs;

  if (left == right) {
    return 0;
  } else {
    return left > right ? 1 : -1;
  }
}

void setBoundColumnInfo(SParsedDataColInfo* pColList, SSchema* pSchema, int32_t numOfCols) {
  pColList->numOfCols = numOfCols;
  pColList->numOfBound = numOfCols;
  pColList->orderStatus = ORDER_STATUS_ORDERED;  // default is ORDERED for non-bound mode
  pColList->boundedColumns = calloc(pColList->numOfCols, sizeof(int32_t));
  pColList->cols = calloc(pColList->numOfCols, sizeof(SBoundColumn));
  pColList->colIdxInfo = NULL;
  pColList->flen = 0;
  pColList->allNullLen = 0;

  int32_t nVar = 0;
  for (int32_t i = 0; i < pColList->numOfCols; ++i) {
    uint8_t type = pSchema[i].type;
    if (i > 0) {
      pColList->cols[i].offset = pColList->cols[i - 1].offset + pSchema[i - 1].bytes;
      pColList->cols[i].toffset = pColList->flen;
    }
    pColList->flen += TYPE_BYTES[type];
    switch (type) {
      case TSDB_DATA_TYPE_BINARY:
        pColList->allNullLen += (VARSTR_HEADER_SIZE + CHAR_BYTES);
        ++nVar;
        break;
      case TSDB_DATA_TYPE_NCHAR:
        pColList->allNullLen += (VARSTR_HEADER_SIZE + TSDB_NCHAR_SIZE);
        ++nVar;
        break;
      default:
        break;
    }
    pColList->boundedColumns[i] = pSchema[i].colId;
  }
  pColList->allNullLen += pColList->flen;
  pColList->extendedVarLen = (uint16_t)(nVar * sizeof(VarDataOffsetT));
}

int32_t schemaIdxCompar(const void *lhs, const void *rhs) {
  uint16_t left = *(uint16_t *)lhs;
  uint16_t right = *(uint16_t *)rhs;

  if (left == right) {
    return 0;
  } else {
    return left > right ? 1 : -1;
  }
}

int32_t boundIdxCompar(const void *lhs, const void *rhs) {
  uint16_t left = *(uint16_t *)POINTER_SHIFT(lhs, sizeof(uint16_t));
  uint16_t right = *(uint16_t *)POINTER_SHIFT(rhs, sizeof(uint16_t));

  if (left == right) {
    return 0;
  } else {
    return left > right ? 1 : -1;
  }
}

void destroyBoundColumnInfo(SParsedDataColInfo* pColList) {
  tfree(pColList->boundedColumns);
  tfree(pColList->cols);
  tfree(pColList->colIdxInfo);
}

static int32_t createDataBlock(size_t defaultSize, int32_t rowSize, int32_t startOffset, SName* name,
                           const STableMeta* pTableMeta, STableDataBlocks** dataBlocks) {
  STableDataBlocks* dataBuf = (STableDataBlocks*)calloc(1, sizeof(STableDataBlocks));
  if (dataBuf == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  dataBuf->nAllocSize = (uint32_t)defaultSize;
  dataBuf->headerSize = startOffset;

  // the header size will always be the startOffset value, reserved for the subumit block header
  if (dataBuf->nAllocSize <= dataBuf->headerSize) {
    dataBuf->nAllocSize = dataBuf->headerSize * 2;
  }

  //dataBuf->pData = calloc(1, dataBuf->nAllocSize);
  dataBuf->pData = malloc(dataBuf->nAllocSize);
  if (dataBuf->pData == NULL) {
    tfree(dataBuf);
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }
  memset(dataBuf->pData, 0, sizeof(SSubmitBlk));

  //Here we keep the tableMeta to avoid it to be remove by other threads.
  dataBuf->pTableMeta = tableMetaDup(pTableMeta);

  SParsedDataColInfo* pColInfo = &dataBuf->boundColumnInfo;
  SSchema* pSchema = getTableColumnSchema(dataBuf->pTableMeta);
  setBoundColumnInfo(pColInfo, pSchema, dataBuf->pTableMeta->tableInfo.numOfColumns);

  dataBuf->ordered  = true;
  dataBuf->prevTS   = INT64_MIN;
  dataBuf->rowSize  = rowSize;
  dataBuf->size     = startOffset;
  dataBuf->tsSource = -1;
  dataBuf->vgId     = dataBuf->pTableMeta->vgId;

  tNameAssign(&dataBuf->tableName, name);

  assert(defaultSize > 0 && pTableMeta != NULL && dataBuf->pTableMeta != NULL);

  *dataBlocks = dataBuf;
  return TSDB_CODE_SUCCESS;
}

int32_t getDataBlockFromList(SHashObj* pHashList, int64_t id, int32_t size, int32_t startOffset, int32_t rowSize,
                                SName* name, const STableMeta* pTableMeta, STableDataBlocks** dataBlocks,
                                SArray* pBlockList) {
  *dataBlocks = NULL;
  STableDataBlocks** t1 = (STableDataBlocks**)taosHashGet(pHashList, (const char*)&id, sizeof(id));
  if (t1 != NULL) {
    *dataBlocks = *t1;
  }

  if (*dataBlocks == NULL) {
    int32_t ret = createDataBlock((size_t)size, rowSize, startOffset, name, pTableMeta, dataBlocks);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }

    taosHashPut(pHashList, (const char*)&id, sizeof(int64_t), (char*)dataBlocks, POINTER_BYTES);
    if (pBlockList) {
      taosArrayPush(pBlockList, dataBlocks);
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t getRowExpandSize(STableMeta* pTableMeta) {
  int32_t  result = TD_MEM_ROW_DATA_HEAD_SIZE;
  int32_t  columns = getNumOfColumns(pTableMeta);
  SSchema* pSchema = getTableColumnSchema(pTableMeta);
  for (int32_t i = 0; i < columns; i++) {
    if (IS_VAR_DATA_TYPE((pSchema + i)->type)) {
      result += TYPE_BYTES[TSDB_DATA_TYPE_BINARY];
    }
  }
  return result;
}

/**
 * TODO: Move to tdataformat.h and refactor when STSchema available.
 *    - fetch flen and toffset from STSChema and remove param spd
 */
static FORCE_INLINE void convertToSDataRow(SMemRow dest, SMemRow src, SSchema *pSchema, int nCols,
                                           SParsedDataColInfo *spd) {
  ASSERT(isKvRow(src));
  SKVRow   kvRow = memRowKvBody(src);
  SDataRow dataRow = memRowDataBody(dest);

  memRowSetType(dest, SMEM_ROW_DATA);
  dataRowSetVersion(dataRow, memRowKvVersion(src));
  dataRowSetLen(dataRow, (TDRowLenT)(TD_DATA_ROW_HEAD_SIZE + spd->flen));

  int32_t kvIdx = 0;
  for (int i = 0; i < nCols; ++i) {
    SSchema *schema = pSchema + i;
    void *   val = tdGetKVRowValOfColEx(kvRow, schema->colId, &kvIdx);
    tdAppendDataColVal(dataRow, val != NULL ? val : getNullValue(schema->type), true, schema->type,
                       (spd->cols + i)->toffset);
  }
}

// TODO: Move to tdataformat.h and refactor when STSchema available.
static FORCE_INLINE void convertToSKVRow(SMemRow dest, SMemRow src, SSchema *pSchema, int nCols, int nBoundCols,
                                         SParsedDataColInfo *spd) {
  ASSERT(isDataRow(src));

  SDataRow dataRow = memRowDataBody(src);
  SKVRow   kvRow = memRowKvBody(dest);

  memRowSetType(dest, SMEM_ROW_KV);
  memRowSetKvVersion(kvRow, dataRowVersion(dataRow));
  kvRowSetNCols(kvRow, nBoundCols);
  kvRowSetLen(kvRow, (TDRowLenT)(TD_KV_ROW_HEAD_SIZE + sizeof(SColIdx) * nBoundCols));

  int32_t toffset = 0, kvOffset = 0;
  for (int i = 0; i < nCols; ++i) {
    if ((spd->cols + i)->valStat == VAL_STAT_HAS) {
      SSchema *schema = pSchema + i;
      toffset = (spd->cols + i)->toffset;
      void *val = tdGetRowDataOfCol(dataRow, schema->type, toffset + TD_DATA_ROW_HEAD_SIZE);
      tdAppendKvColVal(kvRow, val, true, schema->colId, schema->type, kvOffset);
      kvOffset += sizeof(SColIdx);
    }
  }
}

// TODO: Move to tdataformat.h and refactor when STSchema available.
static FORCE_INLINE void convertSMemRow(SMemRow dest, SMemRow src, STableDataBlocks *pBlock) {
  STableMeta *        pTableMeta = pBlock->pTableMeta;
  STableComInfo       tinfo = getTableInfo(pTableMeta);
  SSchema *           pSchema = getTableColumnSchema(pTableMeta);
  SParsedDataColInfo *spd = &pBlock->boundColumnInfo;

  ASSERT(dest != src);

  if (isDataRow(src)) {
    // TODO: Can we use pBlock -> numOfParam directly?
    ASSERT(spd->numOfBound > 0);
    convertToSKVRow(dest, src, pSchema, tinfo.numOfColumns, spd->numOfBound, spd);
  } else {
    convertToSDataRow(dest, src, pSchema, tinfo.numOfColumns, spd);
  }
}

void destroyDataBlock(STableDataBlocks* pDataBlock, bool removeMeta) {
  if (pDataBlock == NULL) {
    return;
  }

  tfree(pDataBlock->pData);

  if (removeMeta) {
    char name[TSDB_TABLE_FNAME_LEN] = {0};
    tNameExtractFullName(&pDataBlock->tableName, name);

    // taosHashRemove(tscTableMetaMap, name, strnlen(name, TSDB_TABLE_FNAME_LEN));
  }

  if (!pDataBlock->cloned) {
    tfree(pDataBlock->params);

    // free the refcount for metermeta
    if (pDataBlock->pTableMeta != NULL) {
      tfree(pDataBlock->pTableMeta);
    }

    destroyBoundColumnInfo(&pDataBlock->boundColumnInfo);
  }

  tfree(pDataBlock);
}

void* destroyBlockArrayList(SArray* pDataBlockList) {
  if (pDataBlockList == NULL) {
    return NULL;
  }

  size_t size = taosArrayGetSize(pDataBlockList);
  for (int32_t i = 0; i < size; i++) {
    void* d = taosArrayGetP(pDataBlockList, i);
    destroyDataBlock(d, false);
  }

  taosArrayDestroy(pDataBlockList);
  return NULL;
}

// data block is disordered, sort it in ascending order
void sortRemoveDataBlockDupRowsRaw(STableDataBlocks *dataBuf) {
  SSubmitBlk *pBlocks = (SSubmitBlk *)dataBuf->pData;

  // size is less than the total size, since duplicated rows may be removed yet.
  assert(pBlocks->numOfRows * dataBuf->rowSize + sizeof(SSubmitBlk) == dataBuf->size);

  if (!dataBuf->ordered) {
    char *pBlockData = pBlocks->data;
    qsort(pBlockData, pBlocks->numOfRows, dataBuf->rowSize, rowDataCompar);

    int32_t i = 0;
    int32_t j = 1;

    while (j < pBlocks->numOfRows) {
      TSKEY ti = *(TSKEY *)(pBlockData + dataBuf->rowSize * i);
      TSKEY tj = *(TSKEY *)(pBlockData + dataBuf->rowSize * j);

      if (ti == tj) {
        ++j;
        continue;
      }

      int32_t nextPos = (++i);
      if (nextPos != j) {
        memmove(pBlockData + dataBuf->rowSize * nextPos, pBlockData + dataBuf->rowSize * j, dataBuf->rowSize);
      }

      ++j;
    }

    dataBuf->ordered = true;

    pBlocks->numOfRows = i + 1;
    dataBuf->size = sizeof(SSubmitBlk) + dataBuf->rowSize * pBlocks->numOfRows;
  }

  dataBuf->prevTS = INT64_MIN;
}

// data block is disordered, sort it in ascending order
int sortRemoveDataBlockDupRows(STableDataBlocks *dataBuf, SBlockKeyInfo *pBlkKeyInfo) {
  SSubmitBlk *pBlocks = (SSubmitBlk *)dataBuf->pData;
  int16_t     nRows = pBlocks->numOfRows;

  // size is less than the total size, since duplicated rows may be removed yet.

  // allocate memory
  size_t nAlloc = nRows * sizeof(SBlockKeyTuple);
  if (pBlkKeyInfo->pKeyTuple == NULL || pBlkKeyInfo->maxBytesAlloc < nAlloc) {
    char *tmp = realloc(pBlkKeyInfo->pKeyTuple, nAlloc);
    if (tmp == NULL) {
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }
    pBlkKeyInfo->pKeyTuple = (SBlockKeyTuple *)tmp;
    pBlkKeyInfo->maxBytesAlloc = (int32_t)nAlloc;
  }
  memset(pBlkKeyInfo->pKeyTuple, 0, nAlloc);

  int32_t         extendedRowSize = getExtendedRowSize(dataBuf);
  SBlockKeyTuple *pBlkKeyTuple = pBlkKeyInfo->pKeyTuple;
  char *          pBlockData = pBlocks->data;
  int             n = 0;
  while (n < nRows) {
    pBlkKeyTuple->skey = memRowKey(pBlockData);
    pBlkKeyTuple->payloadAddr = pBlockData;

    // next loop
    pBlockData += extendedRowSize;
    ++pBlkKeyTuple;
    ++n;
  }

  if (!dataBuf->ordered) {
    pBlkKeyTuple = pBlkKeyInfo->pKeyTuple;
    qsort(pBlkKeyTuple, nRows, sizeof(SBlockKeyTuple), rowDataCompar);

    pBlkKeyTuple = pBlkKeyInfo->pKeyTuple;
    int32_t i = 0;
    int32_t j = 1;
    while (j < nRows) {
      TSKEY ti = (pBlkKeyTuple + i)->skey;
      TSKEY tj = (pBlkKeyTuple + j)->skey;

      if (ti == tj) {
        ++j;
        continue;
      }

      int32_t nextPos = (++i);
      if (nextPos != j) {
        memmove(pBlkKeyTuple + nextPos, pBlkKeyTuple + j, sizeof(SBlockKeyTuple));
      }
      ++j;
    }

    dataBuf->ordered = true;
    pBlocks->numOfRows = i + 1;
  }

  dataBuf->size = sizeof(SSubmitBlk) + pBlocks->numOfRows * extendedRowSize;
  dataBuf->prevTS = INT64_MIN;

  return 0;
}

// Erase the empty space reserved for binary data
static int trimDataBlock(void* pDataBlock, STableDataBlocks* pTableDataBlock, SBlockKeyTuple* blkKeyTuple, int8_t schemaAttached, bool isRawPayload) {
  // TODO: optimize this function, handle the case while binary is not presented
  STableMeta*     pTableMeta = pTableDataBlock->pTableMeta;
  STableComInfo   tinfo = getTableInfo(pTableMeta);
  SSchema*        pSchema = getTableColumnSchema(pTableMeta);

  SSubmitBlk* pBlock = pDataBlock;
  memcpy(pDataBlock, pTableDataBlock->pData, sizeof(SSubmitBlk));
  pDataBlock = (char*)pDataBlock + sizeof(SSubmitBlk);

  int32_t flen = 0;  // original total length of row

  // schema needs to be included into the submit data block
  if (schemaAttached) {
    int32_t numOfCols = getNumOfColumns(pTableDataBlock->pTableMeta);
    for(int32_t j = 0; j < numOfCols; ++j) {
      STColumn* pCol = (STColumn*) pDataBlock;
      pCol->colId = htons(pSchema[j].colId);
      pCol->type  = pSchema[j].type;
      pCol->bytes = htons(pSchema[j].bytes);
      pCol->offset = 0;

      pDataBlock = (char*)pDataBlock + sizeof(STColumn);
      flen += TYPE_BYTES[pSchema[j].type];
    }

    int32_t schemaSize = sizeof(STColumn) * numOfCols;
    pBlock->schemaLen = schemaSize;
  } else {
    if (isRawPayload) {
      for (int32_t j = 0; j < tinfo.numOfColumns; ++j) {
        flen += TYPE_BYTES[pSchema[j].type];
      }
    }
    pBlock->schemaLen = 0;
  }

  char* p = pTableDataBlock->pData + sizeof(SSubmitBlk);
  pBlock->dataLen = 0;
  int32_t numOfRows = htons(pBlock->numOfRows);

  if (isRawPayload) {
    for (int32_t i = 0; i < numOfRows; ++i) {
      SMemRow memRow = (SMemRow)pDataBlock;
      memRowSetType(memRow, SMEM_ROW_DATA);
      SDataRow trow = memRowDataBody(memRow);
      dataRowSetLen(trow, (uint16_t)(TD_DATA_ROW_HEAD_SIZE + flen));
      dataRowSetVersion(trow, pTableMeta->sversion);

      int toffset = 0;
      for (int32_t j = 0; j < tinfo.numOfColumns; j++) {
        tdAppendColVal(trow, p, pSchema[j].type, toffset);
        toffset += TYPE_BYTES[pSchema[j].type];
        p += pSchema[j].bytes;
      }

      pDataBlock = (char*)pDataBlock + memRowTLen(memRow);
      pBlock->dataLen += memRowTLen(memRow);
    }
  } else {
    for (int32_t i = 0; i < numOfRows; ++i) {
      char* payload = (blkKeyTuple + i)->payloadAddr;
      if (isNeedConvertRow(payload)) {
        convertSMemRow(pDataBlock, payload, pTableDataBlock);
        TDRowTLenT rowTLen = memRowTLen(pDataBlock);
        pDataBlock = POINTER_SHIFT(pDataBlock, rowTLen);
        pBlock->dataLen += rowTLen;
      } else {
        TDRowTLenT rowTLen = memRowTLen(payload);
        memcpy(pDataBlock, payload, rowTLen);
        pDataBlock = POINTER_SHIFT(pDataBlock, rowTLen);
        pBlock->dataLen += rowTLen;
      }
    }
  }

  int32_t len = pBlock->dataLen + pBlock->schemaLen;
  pBlock->dataLen = htonl(pBlock->dataLen);
  pBlock->schemaLen = htonl(pBlock->schemaLen);

  return len;
}

static void extractTableNameList(SHashObj* pHashObj, bool freeBlockMap) {
  // pInsertParam->numOfTables = (int32_t) taosHashGetSize(pInsertParam->pTableBlockHashList);
  // if (pInsertParam->pTableNameList == NULL) {
  //   pInsertParam->pTableNameList = malloc(pInsertParam->numOfTables * POINTER_BYTES);
  // }

  // STableDataBlocks **p1 = taosHashIterate(pInsertParam->pTableBlockHashList, NULL);
  // int32_t i = 0;
  // while(p1) {
  //   STableDataBlocks* pBlocks = *p1;
  //   pInsertParam->pTableNameList[i++] = tNameDup(&pBlocks->tableName);
  //   p1 = taosHashIterate(pInsertParam->pTableBlockHashList, p1);
  // }

  // if (freeBlockMap) {
  //   pInsertParam->pTableBlockHashList = tscDestroyBlockHashTable(pInsertParam->pTableBlockHashList, false);
  // }
}

int32_t mergeTableDataBlocks(SHashObj* pHashObj, int8_t schemaAttached, uint8_t payloadType, bool freeBlockMap) {
  const int INSERT_HEAD_SIZE = sizeof(SMsgDesc) + sizeof(SSubmitMsg);
  int       code = 0;
  bool      isRawPayload = IS_RAW_PAYLOAD(payloadType);
  SHashObj* pVnodeDataBlockHashList = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, false);
  SArray*   pVnodeDataBlockList = taosArrayInit(8, POINTER_BYTES);

  STableDataBlocks** p = taosHashIterate(pHashObj, NULL);
  STableDataBlocks* pOneTableBlock = *p;
  SBlockKeyInfo blkKeyInfo = {0};  // share by pOneTableBlock
  while (pOneTableBlock) {
    SSubmitBlk* pBlocks = (SSubmitBlk*) pOneTableBlock->pData;
    if (pBlocks->numOfRows > 0) {
      STableDataBlocks* dataBuf = NULL;
      int32_t ret = getDataBlockFromList(pVnodeDataBlockHashList, pOneTableBlock->vgId, TSDB_PAYLOAD_SIZE,
                                  INSERT_HEAD_SIZE, 0, &pOneTableBlock->tableName, pOneTableBlock->pTableMeta, &dataBuf, pVnodeDataBlockList);
      if (ret != TSDB_CODE_SUCCESS) {
        taosHashCleanup(pVnodeDataBlockHashList);
        destroyBlockArrayList(pVnodeDataBlockList);
        tfree(blkKeyInfo.pKeyTuple);
        return ret;
      }

      // the maximum expanded size in byte when a row-wise data is converted to SDataRow format
      int32_t           expandSize = isRawPayload ? getRowExpandSize(pOneTableBlock->pTableMeta) : 0;
      int64_t destSize = dataBuf->size + pOneTableBlock->size + pBlocks->numOfRows * expandSize +
                         sizeof(STColumn) * getNumOfColumns(pOneTableBlock->pTableMeta);

      if (dataBuf->nAllocSize < destSize) {
        dataBuf->nAllocSize = (uint32_t)(destSize * 1.5);
        char* tmp = realloc(dataBuf->pData, dataBuf->nAllocSize);
        if (tmp != NULL) {
          dataBuf->pData = tmp;
        } else {  // failed to allocate memory, free already allocated memory and return error code
          taosHashCleanup(pVnodeDataBlockHashList);
          destroyBlockArrayList(pVnodeDataBlockList);
          tfree(dataBuf->pData);
          tfree(blkKeyInfo.pKeyTuple);
          return TSDB_CODE_TSC_OUT_OF_MEMORY;
        }
      }

      if (isRawPayload) {
        sortRemoveDataBlockDupRowsRaw(pOneTableBlock);
      } else {
        if ((code = sortRemoveDataBlockDupRows(pOneTableBlock, &blkKeyInfo)) != 0) {
          taosHashCleanup(pVnodeDataBlockHashList);
          destroyBlockArrayList(pVnodeDataBlockList);
          tfree(dataBuf->pData);
          tfree(blkKeyInfo.pKeyTuple);
          return code;
        }
        ASSERT(blkKeyInfo.pKeyTuple != NULL && pBlocks->numOfRows > 0);
      }

      int32_t len = pBlocks->numOfRows *
                        (isRawPayload ? (pOneTableBlock->rowSize + expandSize) : getExtendedRowSize(pOneTableBlock)) +
                    sizeof(STColumn) * getNumOfColumns(pOneTableBlock->pTableMeta);

      pBlocks->tid = htonl(pBlocks->tid);
      pBlocks->uid = htobe64(pBlocks->uid);
      pBlocks->sversion = htonl(pBlocks->sversion);
      pBlocks->numOfRows = htons(pBlocks->numOfRows);
      pBlocks->schemaLen = 0;

      // erase the empty space reserved for binary data
      int32_t finalLen = trimDataBlock(dataBuf->pData + dataBuf->size, pOneTableBlock, blkKeyInfo.pKeyTuple, schemaAttached, isRawPayload);
      assert(finalLen <= len);

      dataBuf->size += (finalLen + sizeof(SSubmitBlk));
      assert(dataBuf->size <= dataBuf->nAllocSize);

      // the length does not include the SSubmitBlk structure
      pBlocks->dataLen = htonl(finalLen);
      dataBuf->numOfTables += 1;

      pBlocks->numOfRows = 0;
    }

    p = taosHashIterate(pHashObj, p);
    if (p == NULL) {
      break;
    }

    pOneTableBlock = *p;
  }

  extractTableNameList(pHashObj, freeBlockMap);

  // free the table data blocks;
  // pInsertParam->pDataBlocks = pVnodeDataBlockList;
  taosHashCleanup(pVnodeDataBlockHashList);
  tfree(blkKeyInfo.pKeyTuple);

  return TSDB_CODE_SUCCESS;
}

int32_t allocateMemIfNeed(STableDataBlocks *pDataBlock, int32_t rowSize, int32_t * numOfRows) {
  size_t    remain = pDataBlock->nAllocSize - pDataBlock->size;
  const int factor = 5;
  uint32_t nAllocSizeOld = pDataBlock->nAllocSize;
  
  // expand the allocated size
  if (remain < rowSize * factor) {
    while (remain < rowSize * factor) {
      pDataBlock->nAllocSize = (uint32_t)(pDataBlock->nAllocSize * 1.5);
      remain = pDataBlock->nAllocSize - pDataBlock->size;
    }

    char *tmp = realloc(pDataBlock->pData, (size_t)pDataBlock->nAllocSize);
    if (tmp != NULL) {
      pDataBlock->pData = tmp;
      memset(pDataBlock->pData + pDataBlock->size, 0, pDataBlock->nAllocSize - pDataBlock->size);
    } else {
      // do nothing, if allocate more memory failed
      pDataBlock->nAllocSize = nAllocSizeOld;
      *numOfRows = (int32_t)(pDataBlock->nAllocSize - pDataBlock->headerSize) / rowSize;
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }
  }

  *numOfRows = (int32_t)(pDataBlock->nAllocSize - pDataBlock->headerSize) / rowSize;
  return TSDB_CODE_SUCCESS;
}

int32_t initMemRowBuilder(SMemRowBuilder *pBuilder, uint32_t nRows, uint32_t nCols, uint32_t nBoundCols, int32_t allNullLen) {
  ASSERT(nRows >= 0 && nCols > 0 && (nBoundCols <= nCols));
  if (nRows > 0) {
    // already init(bind multiple rows by single column)
    if (pBuilder->compareStat == ROW_COMPARE_NEED && (pBuilder->rowInfo != NULL)) {
      return TSDB_CODE_SUCCESS;
    }
  }

  // default compareStat is  ROW_COMPARE_NO_NEED
  if (nBoundCols == 0) {  // file input
    pBuilder->memRowType = SMEM_ROW_DATA;
    return TSDB_CODE_SUCCESS;
  } else {
    float boundRatio = ((float)nBoundCols / (float)nCols);

    if (boundRatio < KVRatioKV) {
      pBuilder->memRowType = SMEM_ROW_KV;
      return TSDB_CODE_SUCCESS;
    } else if (boundRatio > KVRatioData) {
      pBuilder->memRowType = SMEM_ROW_DATA;
      return TSDB_CODE_SUCCESS;
    }
    pBuilder->compareStat = ROW_COMPARE_NEED;

    if (boundRatio < KVRatioPredict) {
      pBuilder->memRowType = SMEM_ROW_KV;
    } else {
      pBuilder->memRowType = SMEM_ROW_DATA;
    }
  }

  pBuilder->kvRowInitLen = TD_MEM_ROW_KV_HEAD_SIZE + nBoundCols * sizeof(SColIdx);

  if (nRows > 0) {
    pBuilder->rowInfo = calloc(nRows, sizeof(SMemRowInfo));
    if (pBuilder->rowInfo == NULL) {
      return TSDB_CODE_TSC_OUT_OF_MEMORY;
    }

    for (int i = 0; i < nRows; ++i) {
      (pBuilder->rowInfo + i)->dataLen = TD_MEM_ROW_DATA_HEAD_SIZE + allNullLen;
      (pBuilder->rowInfo + i)->kvLen = pBuilder->kvRowInitLen;
    }
  }

  return TSDB_CODE_SUCCESS;
}

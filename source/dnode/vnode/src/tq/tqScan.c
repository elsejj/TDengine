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

#include "tq.h"

int32_t tqAddBlockDataToRsp(const SSDataBlock* pBlock, SMqDataRsp* pRsp, int32_t numOfCols, int8_t precision) {
  int32_t dataStrLen = sizeof(SRetrieveTableRsp) + blockGetEncodeSize(pBlock);
  void*   buf = taosMemoryCalloc(1, dataStrLen);
  if (buf == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  SRetrieveTableRsp* pRetrieve = (SRetrieveTableRsp*)buf;
  pRetrieve->useconds = 0;
  pRetrieve->precision = precision;
  pRetrieve->compressed = 0;
  pRetrieve->completed = 1;
  pRetrieve->numOfRows = htobe64((int64_t)pBlock->info.rows);

  int32_t actualLen = blockEncode(pBlock, pRetrieve->data, numOfCols);
  actualLen += sizeof(SRetrieveTableRsp);
  taosArrayPush(pRsp->blockDataLen, &actualLen);
  taosArrayPush(pRsp->blockData, &buf);

  return TSDB_CODE_SUCCESS;
}

static int32_t tqAddBlockSchemaToRsp(const STqExecHandle* pExec, STaosxRsp* pRsp) {
  SSchemaWrapper* pSW = tCloneSSchemaWrapper(pExec->pExecReader->pSchemaWrapper);
  if (pSW == NULL) {
    return -1;
  }
  taosArrayPush(pRsp->blockSchema, &pSW);
  return 0;
}

static int32_t tqAddTbNameToRsp(const STQ* pTq, int64_t uid, STaosxRsp* pRsp, int32_t n) {
  SMetaReader mr = {0};
  metaReaderInit(&mr, pTq->pVnode->pMeta, 0);

  // TODO add reference to gurantee success
  if (metaGetTableEntryByUidCache(&mr, uid) < 0) {
    metaReaderClear(&mr);
    return -1;
  }

  for (int32_t i = 0; i < n; i++) {
    char* tbName = taosStrdup(mr.me.name);
    taosArrayPush(pRsp->blockTbName, &tbName);
  }
  metaReaderClear(&mr);
  return 0;
}

int32_t tqScanData(STQ* pTq, const STqHandle* pHandle, SMqDataRsp* pRsp, STqOffsetVal* pOffset) {
  const int32_t MAX_ROWS_TO_RETURN = 4096;
  int32_t       vgId = TD_VID(pTq->pVnode);
  int32_t       code = 0;
  int32_t       totalRows = 0;

  const STqExecHandle* pExec = &pHandle->execHandle;
  qTaskInfo_t          task = pExec->task;

  if (qStreamPrepareScan(task, pOffset, pHandle->execHandle.subType) < 0) {
    tqDebug("prepare scan failed, return, consumer:0x%"PRIx64, pHandle->consumerId);
    if (pOffset->type == TMQ_OFFSET__LOG) {
      pRsp->rspOffset = *pOffset;
      return code;
    } else {
      tqOffsetResetToLog(pOffset, pHandle->snapshotVer);
      if (qStreamPrepareScan(task, pOffset, pHandle->execHandle.subType) < 0) {
        tqDebug("prepare scan failed, return, consumer:0x%"PRIx64, pHandle->consumerId);
        pRsp->rspOffset = *pOffset;
        return code;
      }
    }
  }

  while (1) {
    SSDataBlock* pDataBlock = NULL;
    uint64_t     ts = 0;

    tqDebug("vgId:%d, tmq task start to execute, consumer:0x%" PRIx64, vgId, pHandle->consumerId);

    code = qExecTask(task, &pDataBlock, &ts);
    if (code != TSDB_CODE_SUCCESS) {
      tqError("vgId:%d, task exec error since %s, consumer:0x%" PRIx64, vgId, terrstr(),
              pHandle->consumerId);
      return code;
    }

    // current scan should be stopped ASAP, since the re-balance occurs.
    if (pDataBlock == NULL) {
      break;
    }

    code = tqAddBlockDataToRsp(pDataBlock, pRsp, pExec->numOfCols, pTq->pVnode->config.tsdbCfg.precision);
    if (code != TSDB_CODE_SUCCESS) {
      tqError("vgId:%d, failed to add block to rsp msg", vgId);
      return code;
    }

    pRsp->blockNum++;

    tqDebug("vgId:%d, consumer:0x%" PRIx64 " tmq task executed, rows:%d, total blocks:%d", vgId, pHandle->consumerId,
            pDataBlock->info.rows, pRsp->blockNum);

    if (pOffset->type == TMQ_OFFSET__SNAPSHOT_DATA) {
      totalRows += pDataBlock->info.rows;
      if (totalRows >= MAX_ROWS_TO_RETURN) {
        break;
      }
    }
  }

  qStreamExtractOffset(task, &pRsp->rspOffset);

  if (pRsp->rspOffset.type == 0) {
    code = TSDB_CODE_INVALID_PARA;
    tqError("vgId:%d, expected rsp offset: type %d %" PRId64 " %" PRId64 " %" PRId64, vgId, pRsp->rspOffset.type,
            pRsp->rspOffset.ts, pRsp->rspOffset.uid, pRsp->rspOffset.version);
    return code;
  }

  if (pRsp->withTbName || pRsp->withSchema) {
    code = TSDB_CODE_INVALID_PARA;
    tqError("vgId:%d, get column should not with meta:%d,%d", vgId, pRsp->withTbName, pRsp->withSchema);
    return code;
  }

  tqDebug("vgId:%d, consumer:0x%" PRIx64 " tmq task executed, rows:%d, total blocks:%d, rows:%d", vgId, pHandle->consumerId,
          pRsp->blockNum, totalRows);

  return code;
}

int32_t tqScanTaosx(STQ* pTq, const STqHandle* pHandle, STaosxRsp* pRsp, SMqMetaRsp* pMetaRsp, STqOffsetVal* pOffset) {
  const STqExecHandle* pExec = &pHandle->execHandle;
  qTaskInfo_t          task = pExec->task;

  if (qStreamPrepareScan(task, pOffset, pHandle->execHandle.subType) < 0) {
    tqDebug("prepare scan failed, return");
    if (pOffset->type == TMQ_OFFSET__LOG) {
      pRsp->rspOffset = *pOffset;
      return 0;
    } else {
      tqOffsetResetToLog(pOffset, pHandle->snapshotVer);
      if (qStreamPrepareScan(task, pOffset, pHandle->execHandle.subType) < 0) {
        tqDebug("prepare scan failed, return");
        pRsp->rspOffset = *pOffset;
        return 0;
      }
    }
  }

  int32_t rowCnt = 0;
  while (1) {
    SSDataBlock* pDataBlock = NULL;
    uint64_t     ts = 0;
    tqDebug("tmqsnap task start to execute");
    if (qExecTask(task, &pDataBlock, &ts) < 0) {
      tqError("vgId:%d, task exec error since %s", pTq->pVnode->config.vgId, terrstr());
      return -1;
    }
    tqDebug("tmqsnap task execute end, get %p", pDataBlock);

    if (pDataBlock != NULL && pDataBlock->info.rows > 0) {
      if (pRsp->withTbName) {
        if (pOffset->type == TMQ_OFFSET__LOG) {
          int64_t uid = pExec->pExecReader->lastBlkUid;
          if (tqAddTbNameToRsp(pTq, uid, pRsp, 1) < 0) {
            continue;
          }
        } else {
          char* tbName = taosStrdup(qExtractTbnameFromTask(task));
          taosArrayPush(pRsp->blockTbName, &tbName);
        }
      }
      if (pRsp->withSchema) {
        if (pOffset->type == TMQ_OFFSET__LOG) {
          tqAddBlockSchemaToRsp(pExec, pRsp);
        } else {
          SSchemaWrapper* pSW = tCloneSSchemaWrapper(qExtractSchemaFromTask(task));
          taosArrayPush(pRsp->blockSchema, &pSW);
        }
      }

      tqAddBlockDataToRsp(pDataBlock, (SMqDataRsp*)pRsp, taosArrayGetSize(pDataBlock->pDataBlock),
                          pTq->pVnode->config.tsdbCfg.precision);
      pRsp->blockNum++;
      if (pOffset->type == TMQ_OFFSET__LOG) {
        continue;
      } else {
        rowCnt += pDataBlock->info.rows;
        if (rowCnt <= 4096) continue;
      }
    }

    if (pDataBlock == NULL && pOffset->type == TMQ_OFFSET__SNAPSHOT_DATA) {
      if (qStreamExtractPrepareUid(task) != 0) {
        continue;
      }
      tqDebug("tmqsnap vgId: %d, tsdb consume over, switch to wal, ver %" PRId64, TD_VID(pTq->pVnode),
              pHandle->snapshotVer + 1);
      break;
    }

    if (pRsp->blockNum > 0) {
      tqDebug("tmqsnap task exec exited, get data");
      break;
    }

    SMqMetaRsp* tmp = qStreamExtractMetaMsg(task);
    if (tmp->rspOffset.type == TMQ_OFFSET__SNAPSHOT_DATA) {
      tqOffsetResetToData(pOffset, tmp->rspOffset.uid, tmp->rspOffset.ts);
      qStreamPrepareScan(task, pOffset, pHandle->execHandle.subType);
      tmp->rspOffset.type = TMQ_OFFSET__SNAPSHOT_META;
      tqDebug("tmqsnap task exec change to get data");
      continue;
    }

    *pMetaRsp = *tmp;
    tqDebug("tmqsnap task exec exited, get meta");

    tqDebug("task exec exited");
    break;
  }

  qStreamExtractOffset(task, &pRsp->rspOffset);

  if (pRsp->rspOffset.type == 0) {
    tqError("expected rsp offset: type %d %" PRId64 " %" PRId64 " %" PRId64, pRsp->rspOffset.type, pRsp->rspOffset.ts,
            pRsp->rspOffset.uid, pRsp->rspOffset.version);
    return -1;
  }

  return 0;
}

int32_t tqTaosxScanLog(STQ* pTq, STqHandle* pHandle, SPackedData submit, STaosxRsp* pRsp, int32_t* totalRows) {
  STqExecHandle* pExec = &pHandle->execHandle;
  SArray* pBlocks = taosArrayInit(0, sizeof(SSDataBlock));
  SArray* pSchemas = taosArrayInit(0, sizeof(void*));

  if (pExec->subType == TOPIC_SUB_TYPE__TABLE) {
    STqReader* pReader = pExec->pExecReader;
    tqReaderSetSubmitReq2(pReader, submit.msgStr, submit.msgLen, submit.ver);
    while (tqNextDataBlock2(pReader)) {
      taosArrayClear(pBlocks);
      taosArrayClear(pSchemas);
      SSubmitTbData* pSubmitTbDataRet = NULL;
      if (tqRetrieveTaosxBlock2(pReader, pBlocks, pSchemas, &pSubmitTbDataRet) < 0) {
        if (terrno == TSDB_CODE_TQ_TABLE_SCHEMA_NOT_FOUND) continue;
      }
      if (pRsp->withTbName) {
        int64_t uid = pExec->pExecReader->lastBlkUid;
        if (tqAddTbNameToRsp(pTq, uid, pRsp, taosArrayGetSize(pBlocks)) < 0) {
          taosArrayDestroyEx(pBlocks, (FDelete)blockDataFreeRes);
          taosArrayDestroyP(pSchemas, (FDelete)tDeleteSSchemaWrapper);
          pBlocks = taosArrayInit(0, sizeof(SSDataBlock));
          pSchemas = taosArrayInit(0, sizeof(void*));
          continue;
        }
      }
      if (pHandle->fetchMeta && pSubmitTbDataRet->pCreateTbReq != NULL) {
        if (pRsp->createTableNum == 0) {
          pRsp->createTableLen = taosArrayInit(0, sizeof(int32_t));
          pRsp->createTableReq = taosArrayInit(0, sizeof(void*));
        }

        int32_t  code = TSDB_CODE_SUCCESS;
        uint32_t len = 0;
        tEncodeSize(tEncodeSVCreateTbReq, pSubmitTbDataRet->pCreateTbReq, len, code);
        if (TSDB_CODE_SUCCESS != code) {
          continue;
        }
        void* createReq = taosMemoryCalloc(1, len);
        SEncoder encoder = {0};
        tEncoderInit(&encoder, createReq, len);
        code = tEncodeSVCreateTbReq(&encoder, pSubmitTbDataRet->pCreateTbReq);
        if (code < 0) {
          tEncoderClear(&encoder);
          taosMemoryFree(createReq);
          continue;
        }

        taosArrayPush(pRsp->createTableLen, &len);
        taosArrayPush(pRsp->createTableReq, &createReq);
        pRsp->createTableNum++;

        tEncoderClear(&encoder);
      }
      for (int32_t i = 0; i < taosArrayGetSize(pBlocks); i++) {
        SSDataBlock* pBlock = taosArrayGet(pBlocks, i);
        tqAddBlockDataToRsp(pBlock, (SMqDataRsp*)pRsp, taosArrayGetSize(pBlock->pDataBlock),
                            pTq->pVnode->config.tsdbCfg.precision);
        totalRows += pBlock->info.rows;
        blockDataFreeRes(pBlock);
        SSchemaWrapper* pSW = taosArrayGetP(pSchemas, i);
        taosArrayPush(pRsp->blockSchema, &pSW);
        pRsp->blockNum++;
      }
    }
  } else if (pExec->subType == TOPIC_SUB_TYPE__DB) {
    STqReader* pReader = pExec->pExecReader;
    tqReaderSetSubmitReq2(pReader, submit.msgStr, submit.msgLen, submit.ver);
    while (tqNextDataBlockFilterOut2(pReader, pExec->execDb.pFilterOutTbUid)) {
      taosArrayClear(pBlocks);
      taosArrayClear(pSchemas);
      SSubmitTbData* pSubmitTbDataRet = NULL;
      if (tqRetrieveTaosxBlock2(pReader, pBlocks, pSchemas, &pSubmitTbDataRet) < 0) {
        if (terrno == TSDB_CODE_TQ_TABLE_SCHEMA_NOT_FOUND) continue;
      }
      if (pRsp->withTbName) {
        int64_t uid = pExec->pExecReader->lastBlkUid;
        if (tqAddTbNameToRsp(pTq, uid, pRsp, taosArrayGetSize(pBlocks)) < 0) {
          taosArrayDestroyEx(pBlocks, (FDelete)blockDataFreeRes);
          taosArrayDestroyP(pSchemas, (FDelete)tDeleteSSchemaWrapper);
          pBlocks = taosArrayInit(0, sizeof(SSDataBlock));
          pSchemas = taosArrayInit(0, sizeof(void*));
          continue;
        }
      }
      if (pHandle->fetchMeta && pSubmitTbDataRet->pCreateTbReq != NULL) {
        if (pRsp->createTableNum == 0) {
          pRsp->createTableLen = taosArrayInit(0, sizeof(int32_t));
          pRsp->createTableReq = taosArrayInit(0, sizeof(void*));
        }

        int32_t  code = TSDB_CODE_SUCCESS;
        uint32_t len = 0;
        tEncodeSize(tEncodeSVCreateTbReq, pSubmitTbDataRet->pCreateTbReq, len, code);
        if (TSDB_CODE_SUCCESS != code) {
          continue;
        }
        void* createReq = taosMemoryCalloc(1, len);
        SEncoder encoder = {0};
        tEncoderInit(&encoder, createReq, len);
        code = tEncodeSVCreateTbReq(&encoder, pSubmitTbDataRet->pCreateTbReq);
        if (code < 0) {
          tEncoderClear(&encoder);
          taosMemoryFree(createReq);
          continue;
        }

        taosArrayPush(pRsp->createTableLen, &len);
        taosArrayPush(pRsp->createTableReq, &createReq);
        pRsp->createTableNum++;

        tEncoderClear(&encoder);
      }
      for (int32_t i = 0; i < taosArrayGetSize(pBlocks); i++) {
        SSDataBlock* pBlock = taosArrayGet(pBlocks, i);
        tqAddBlockDataToRsp(pBlock, (SMqDataRsp*)pRsp, taosArrayGetSize(pBlock->pDataBlock),
                            pTq->pVnode->config.tsdbCfg.precision);
        *totalRows += pBlock->info.rows;
        blockDataFreeRes(pBlock);
        SSchemaWrapper* pSW = taosArrayGetP(pSchemas, i);
        taosArrayPush(pRsp->blockSchema, &pSW);
        pRsp->blockNum++;
      }
    }
  }
  taosArrayDestroy(pBlocks);
  taosArrayDestroy(pSchemas);
  return 0;
}

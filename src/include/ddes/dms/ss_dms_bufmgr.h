/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 * 
 * ss_dms_bufmgr.h
 * 
 * IDENTIFICATION
 *        src/include/ddes/dms/ss_dms_bufmgr.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef SS_DMS_BUFMGR_H
#define SS_DMS_BUFMGR_H

#include "ddes/dms/ss_common_attr.h"
#include "ddes/dms/ss_dms.h"
#include "storage/buf/buf_internals.h"

#define GetDmsBufCtrl(id) (&t_thrd.storage_cxt.dmsBufCtl[(id)])

#define DmsInitLatch(drid, _type, _oid, _idx, _parent_part, _part, _uid) \
    do {                                                      \
        (drid)->type = _type;                                 \
        (drid)->uid = _uid;                                   \
        (drid)->oid = _oid;                                   \
        (drid)->index = _idx;                                 \
        (drid)->parent_part = _parent_part;                   \
        (drid)->part = _part;                                 \
    } while (0)

typedef struct SSBroadcastDDLLock {
    SSBroadcastOp type; // must be first
    LOCKTAG locktag;
    LOCKMODE lockmode;
    bool sessionlock;
    bool dontWait;
} SSBroadcastDDLLock;

void InitDmsBufCtrl(void);
void InitDmsContext(dms_context_t* dmsContext);

void MarkReadHint(int buf_id, char persistence, bool extend, const XLogPhyBlock *pblk);
bool LockModeCompatible(dms_buf_ctrl_t *buf_ctrl, LWLockMode mode);
bool StartReadPage(BufferDesc *buf_desc, LWLockMode mode);
void ClearReadHint(int buf_id, bool buf_deleted = false);
Buffer TerminateReadPage(BufferDesc* buf_desc, ReadBufferMode read_mode, const XLogPhyBlock *pblk);
Buffer TerminateReadSegPage(BufferDesc *buf_desc, ReadBufferMode read_mode, SegSpace *spc = NULL);
Buffer DmsReadPage(Buffer buffer, LWLockMode mode, ReadBufferMode read_mode);
Buffer DmsReadSegPage(Buffer buffer, LWLockMode mode, ReadBufferMode read_mode);
bool DmsReleaseOwner(BufferTag buf_tag, int buf_id);
int32 CheckBuf4Rebuild(BufferDesc* buf_desc);
int SSLockAcquire(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock, bool dontWait,
    dms_opengauss_lock_req_type_t reqType = LOCK_NORMAL_MODE);
int SSLockRelease(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock);
void SSLockReleaseAll();
void SSLockAcquireAll();
void MarkReadPblk(int buf_id, const XLogPhyBlock *pblk);
void SSCheckBufferIfNeedMarkDirty(Buffer buf);
void SSRecheckBufferPool();
void TransformLockTagToDmsLatch(dms_drlatch_t* dlatch, const LOCKTAG locktag);
void CheckPageNeedSkipInRecovery(Buffer buf);

#endif

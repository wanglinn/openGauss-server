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
 * ss_dms_bufmgr.cpp
 *        Provide common interface for read page within DMS process.
 *
 * IDENTIFICATION
 *        src/gausskernel/ddes/adapter/ss_dms_bufmgr.cpp
 *
 * ---------------------------------------------------------------------------------------
 */

#include "postgres.h"
#include "storage/proc.h"
#include "storage/buf/bufmgr.h"
#include "storage/smgr/segment.h"
#include "utils/resowner.h"
#include "ddes/dms/ss_dms_bufmgr.h"
#include "securec_check.h"
#include "miscadmin.h"

void InitDmsBufCtrl(void)
{
    bool found_dms_buf = false;
    t_thrd.storage_cxt.dmsBufCtl = (dms_buf_ctrl_t *)CACHELINEALIGN(ShmemInitStruct(
        "dms buffer ctrl", TOTAL_BUFFER_NUM * sizeof(dms_buf_ctrl_t) + PG_CACHE_LINE_SIZE, &found_dms_buf));

    if (!found_dms_buf) {
        for (int i = 0; i < TOTAL_BUFFER_NUM; i++) {
            dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(i);
            buf_ctrl->buf_id = i;
            buf_ctrl->state = 0;
            buf_ctrl->is_remote_dirty = 0;
            buf_ctrl->lock_mode = (uint8)DMS_LOCK_NULL;
            buf_ctrl->is_edp = 0;
            buf_ctrl->force_request = 0;
            buf_ctrl->edp_scn = 0;
            buf_ctrl->edp_map = 0;
            buf_ctrl->pblk_relno = InvalidOid;
            buf_ctrl->pblk_blkno = InvalidBlockNumber;
            buf_ctrl->pblk_lsn = InvalidXLogRecPtr;
        }
    }
}

void InitDmsContext(dms_context_t *dmsContext)
{
    /* Proc threads id range: [0, TotalProcs - 1]. Non-proc threads id range: [TotalProcs + 1, TotalProcs + 4] */
    uint32 TotalProcs = (uint32)(GLOBAL_ALL_PROCS);
    dmsContext->inst_id = (unsigned int)SS_MY_INST_ID;
    dmsContext->sess_id = (unsigned int)(t_thrd.proc ? t_thrd.proc->logictid : t_thrd.myLogicTid + TotalProcs);
    dmsContext->db_handle = t_thrd.proc;
    if (AmDmsReformProcProcess()) {
        dmsContext->sess_rcy = DMS_SESSION_IN_REFORM;
    } else if (AmPageRedoProcess() || AmStartupProcess()) {
        dmsContext->sess_rcy = DMS_SESSION_IN_RECOVERY;
    } else {
        dmsContext->sess_rcy = DMS_SESSION_NORMAL;
    }
    dmsContext->is_try = 0;
}

void InitDmsBufContext(dms_context_t* dmsBufCxt, BufferTag buftag)
{
    InitDmsContext(dmsBufCxt);
    dmsBufCxt->len   = DMS_PAGEID_SIZE;
    dmsBufCxt->type = (unsigned char)DRC_RES_PAGE_TYPE;
    errno_t err = memcpy_s(dmsBufCxt->resid, DMS_PAGEID_SIZE, &buftag, sizeof(BufferTag));
    securec_check_c(err, "\0", "\0");
}

void TransformLockTagToDmsLatch(dms_drlatch_t* dlatch, const LOCKTAG locktag)
{
    DmsInitLatch(&dlatch->drid, locktag.locktag_type, locktag.locktag_field1, locktag.locktag_field2,
        locktag.locktag_field3, locktag.locktag_field4, locktag.locktag_field5);
}

static void CalcSegDmsPhysicalLoc(BufferDesc* buf_desc, Buffer buffer)
{
    if (IsSegmentFileNode(buf_desc->tag.rnode)) {
        SegmentCheck(!IsSegmentPhysicalRelNode(buf_desc->tag.rnode));
        SegPageLocation loc = seg_get_physical_location(buf_desc->tag.rnode, buf_desc->tag.forkNum,
            buf_desc->tag.blockNum);
        SegmentCheck(loc.blocknum != InvalidBlockNumber);

        ereport(DEBUG1, (errmsg("buffer:%d is segdata page, bufdesc seginfo is empty, calc segfileno:%d, segblkno:%u",
            buffer, (int32)loc.extent_size, loc.blocknum)));

        buf_desc->seg_fileno = (uint8)EXTENT_SIZE_TO_TYPE((int)loc.extent_size);
        buf_desc->seg_blockno = loc.blocknum;
    }
}

bool LockModeCompatible(dms_buf_ctrl_t *buf_ctrl, LWLockMode mode)
{
    bool compatible = false;

    if (mode == LW_SHARED) {
        switch (buf_ctrl->lock_mode) {
            case DMS_LOCK_SHARE:
            case DMS_LOCK_EXCLUSIVE:
                compatible = true;
                break;
            default:
                break;
        }
    } else if (mode == LW_EXCLUSIVE) {
        if (buf_ctrl->lock_mode == (uint8)DMS_LOCK_EXCLUSIVE) {
            compatible = true;
        }
    } else {
        AssertEreport(0, MOD_DMS, "lock mode value is wrong");
    }

    return compatible;
}

void MarkReadPblk(int buf_id, const XLogPhyBlock *pblk)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_id);
    if (pblk) {
        buf_ctrl->pblk_relno = pblk->relNode;
        buf_ctrl->pblk_blkno = pblk->block;
        buf_ctrl->pblk_lsn = pblk->lsn;
    } else {
        buf_ctrl->pblk_relno = InvalidOid;
        buf_ctrl->pblk_blkno = InvalidBlockNumber;
        buf_ctrl->pblk_lsn = InvalidXLogRecPtr;
    }
}

void MarkReadHint(int buf_id, char persistence, bool extend, const XLogPhyBlock *pblk)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_id);
    if (persistence == 'p') {
        buf_ctrl->state |= BUF_IS_RELPERSISTENT;
    } else if (persistence == 't') {
        buf_ctrl->state |= BUF_IS_RELPERSISTENT_TEMP;
    }

    if (extend) {
        buf_ctrl->state |= BUF_IS_EXTEND;
    }

    MarkReadPblk(buf_id, pblk);
}

void ClearReadHint(int buf_id, bool buf_deleted)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_id);
    buf_ctrl->state &=
        ~(BUF_NEED_LOAD | BUF_IS_LOADED | BUF_LOAD_FAILED | BUF_NEED_TRANSFER | BUF_IS_EXTEND | BUF_DIRTY_NEED_FLUSH);
    if (buf_deleted) {
        buf_ctrl->state = 0;
    }
}

/*
 * true: the page is transferred successfully by dms,
 * false: the page request is rejected or error, if hold the content_lock,
 * should release the content_lock and io_in_process lock and retry.
 */
bool StartReadPage(BufferDesc *buf_desc, LWLockMode mode)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);
    dms_lock_mode_t req_mode = (mode == LW_SHARED) ? DMS_LOCK_SHARE : DMS_LOCK_EXCLUSIVE;

    dms_context_t dms_ctx;
    InitDmsBufContext(&dms_ctx, buf_desc->tag);

    int ret = dms_request_page(&dms_ctx, buf_ctrl, req_mode);
    return (ret == DMS_SUCCESS);
}

#ifdef USE_ASSERT_CHECKING
static void SmgrNetPageCheckDiskLSN(BufferDesc *buf_desc, ReadBufferMode read_mode, const XLogPhyBlock *pblk)
{
    /*
     * prerequisite is that the page that initialized to zero in memory should be flush to disk
     */
    if (ENABLE_VERIFY_PAGE_VERSION && (buf_desc->seg_fileno != EXTENT_INVALID ||
        IsSegmentBufferID(buf_desc->buf_id)) && (read_mode == RBM_NORMAL)) {
        char *origin_buf = (char *)palloc(BLCKSZ + ALIGNOF_BUFFER);
        char *temp_buf = (char *)BUFFERALIGN(origin_buf);
        ReadBuffer_common_for_check(read_mode, buf_desc, pblk, temp_buf);
        XLogRecPtr lsn_on_disk = PageGetLSN(temp_buf);
        XLogRecPtr lsn_on_mem = PageGetLSN(BufHdrGetBlock(buf_desc));
        /* maybe some pages are not protected by WAL-Logged */
        if ((lsn_on_mem != InvalidXLogRecPtr) && (lsn_on_disk > lsn_on_mem)) {
            RelFileNode rnode = buf_desc->tag.rnode;
            ereport(PANIC, (errmsg("[%d/%d/%d/%d/%d %d-%d] memory lsn(0x%llx) is less than disk lsn(0x%llx)",
                rnode.spcNode, rnode.dbNode, rnode.relNode, rnode.bucketNode, rnode.opt,
                buf_desc->tag.forkNum, buf_desc->tag.blockNum,
                (unsigned long long)lsn_on_mem, (unsigned long long)lsn_on_disk)));
        }
        pfree(origin_buf);
    }
}
#endif

Buffer TerminateReadPage(BufferDesc* buf_desc, ReadBufferMode read_mode, const XLogPhyBlock *pblk)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);
    Buffer buffer;
    if (buf_ctrl->state & BUF_NEED_LOAD) {
        if (g_instance.dms_cxt.SSRecoveryInfo.in_flushcopy && AmDmsReformProcProcess()) {
            ereport(PANIC, (errmsg("SS In flush copy, can't read from disk!")));
        }
        buffer = ReadBuffer_common_for_dms(read_mode, buf_desc, pblk);
    } else {
#ifdef USE_ASSERT_CHECKING
        if (buf_ctrl->state & BUF_IS_EXTEND) {
            ereport(PANIC, (errmsg("extend page should not be tranferred from DMS, "
                "and needs to be loaded from disk!")));
        }
#endif

        Block bufBlock = BufHdrGetBlock(buf_desc);
        Page page = (Page)(bufBlock);
        PageSetChecksumInplace(page, buf_desc->tag.blockNum);

#ifdef USE_ASSERT_CHECKING
        SmgrNetPageCheckDiskLSN(buf_desc, read_mode, pblk);
#endif

        TerminateBufferIO(buf_desc, false, BM_VALID);
        buffer = BufferDescriptorGetBuffer(buf_desc);
        if (!RecoveryInProgress()) {
            CalcSegDmsPhysicalLoc(buf_desc, buffer);
        }
    }

    if ((read_mode == RBM_ZERO_AND_LOCK || read_mode == RBM_ZERO_AND_CLEANUP_LOCK) &&
        !LWLockHeldByMe(buf_desc->content_lock)) {
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    }

    ClearReadHint(buf_desc->buf_id);
    return buffer;
}

static bool DmsStartBufferIO(BufferDesc *buf_desc, LWLockMode mode)
{
    uint32 buf_state;
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);

    if (IsSegmentBufferID(buf_desc->buf_id)) {
        Assert(!HasInProgressBuf());
    } else {
        Assert(!t_thrd.storage_cxt.InProgressBuf || t_thrd.storage_cxt.InProgressBuf == buf_desc);
    }

    if (LWLockHeldByMe(buf_desc->io_in_progress_lock)) {
        return false;
    }

    if (LockModeCompatible(buf_ctrl, mode)) {
        if (!(pg_atomic_read_u32(&buf_desc->state) & BM_IO_IN_PROGRESS)) {
            return false;
        }
    }

    for (;;) {
        (void)LWLockAcquire(buf_desc->io_in_progress_lock, LW_EXCLUSIVE);

        buf_state = LockBufHdr(buf_desc);
        if (!(buf_state & BM_IO_IN_PROGRESS)) {
            break;
        }

        UnlockBufHdr(buf_desc, buf_state);
        LWLockRelease(buf_desc->io_in_progress_lock);
        WaitIO(buf_desc);
    }

    if (LockModeCompatible(buf_ctrl, mode)) {
        UnlockBufHdr(buf_desc, buf_state);
        LWLockRelease(buf_desc->io_in_progress_lock);
        return false;
    }

    buf_state |= BM_IO_IN_PROGRESS;
    UnlockBufHdr(buf_desc, buf_state);

    if (IsSegmentBufferID(buf_desc->buf_id)) {
        SetInProgressFlags(buf_desc, true);
    } else {
        t_thrd.storage_cxt.InProgressBuf = buf_desc;
        t_thrd.storage_cxt.IsForInput = true;
    }
    return true;
}

#ifdef USE_ASSERT_CHECKING
static void SegNetPageCheckDiskLSN(BufferDesc *buf_desc, ReadBufferMode read_mode, SegSpace *spc)
{
    /*
     * prequisite is that the page that initialized to zero in memory should be flushed to disk,
     * references to seg_extend
     */
    if (ENABLE_VERIFY_PAGE_VERSION && (read_mode == RBM_NORMAL)) {
        char *origin_buf = (char *)palloc(BLCKSZ + ALIGNOF_BUFFER);
        char *temp_buf = (char *)BUFFERALIGN(origin_buf);
        ReadSegBufferForCheck(buf_desc, read_mode, spc, temp_buf);
        XLogRecPtr lsn_on_disk = PageGetLSN(temp_buf);
        XLogRecPtr lsn_on_mem = PageGetLSN(BufHdrGetBlock(buf_desc));
        /* maybe some pages are not protected by WAL-Logged */
        if ((lsn_on_mem != InvalidXLogRecPtr) && (lsn_on_disk > lsn_on_mem)) {
            RelFileNode rnode = buf_desc->tag.rnode;
            ereport(PANIC, (errmsg("[%d/%d/%d/%d/%d %d-%d] memory lsn(0x%llx) is less than disk lsn(0x%llx)",
                rnode.spcNode, rnode.dbNode, rnode.relNode, rnode.bucketNode, rnode.opt,
                buf_desc->tag.forkNum, buf_desc->tag.blockNum,
                (unsigned long long)lsn_on_mem, (unsigned long long)lsn_on_disk)));
        }
        pfree(origin_buf);
    }
}
#endif

Buffer TerminateReadSegPage(BufferDesc *buf_desc, ReadBufferMode read_mode, SegSpace *spc)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);
    Buffer buffer;
    if (buf_ctrl->state & BUF_NEED_LOAD) {
        buffer = ReadSegBufferForDMS(buf_desc, read_mode, spc);
    } else {
        Page page = (Page)BufHdrGetBlock(buf_desc);
        PageSetChecksumInplace(page, buf_desc->tag.blockNum);

#ifdef USE_ASSERT_CHECKING
        SegNetPageCheckDiskLSN(buf_desc, read_mode, spc);
#endif

        SegTerminateBufferIO(buf_desc, false, BM_VALID);
        buffer = BufferDescriptorGetBuffer(buf_desc);
    }

    if ((read_mode == RBM_ZERO_AND_LOCK || read_mode == RBM_ZERO_AND_CLEANUP_LOCK) &&
        !LWLockHeldByMe(buf_desc->content_lock)) {
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
    }

    ClearReadHint(buf_desc->buf_id);
    return buffer;
}

Buffer DmsReadSegPage(Buffer buffer, LWLockMode mode, ReadBufferMode read_mode)
{
    BufferDesc *buf_desc = GetBufferDescriptor(buffer - 1);
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);

    if (buf_ctrl->state & BUF_IS_RELPERSISTENT_TEMP) {
        return buffer;
    }

    if (!DmsStartBufferIO(buf_desc, mode)) {
        return buffer;
    }

    if (!StartReadPage(buf_desc, mode)) {
        return 0;
    }
    return TerminateReadSegPage(buf_desc, read_mode);
}

Buffer DmsReadPage(Buffer buffer, LWLockMode mode, ReadBufferMode read_mode)
{
    BufferDesc *buf_desc = GetBufferDescriptor(buffer - 1);
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);

    if (buf_ctrl->state & BUF_IS_RELPERSISTENT_TEMP) {
        return buffer;
    }

    XLogPhyBlock pblk = {0, 0, 0};
    if (OidIsValid(buf_ctrl->pblk_relno)) {
        Assert(ExtentTypeIsValid(buf_ctrl->pblk_relno));
        Assert(buf_ctrl->pblk_blkno != InvalidBlockNumber);
        pblk.relNode = buf_ctrl->pblk_relno;
        pblk.block = buf_ctrl->pblk_blkno;
        pblk.lsn = buf_ctrl->pblk_lsn;
    }

    if (!DmsStartBufferIO(buf_desc, mode)) {
        return buffer;
    }

    if (!StartReadPage(buf_desc, mode)) {
        return 0;
    }
    return TerminateReadPage(buf_desc, read_mode, OidIsValid(buf_ctrl->pblk_relno) ? &pblk : NULL);
}

bool DmsReleaseOwner(BufferTag buf_tag, int buf_id)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_id);
    if (buf_ctrl->state & BUF_IS_RELPERSISTENT_TEMP) {
        return true;
    }
    unsigned char released = 0;
    dms_context_t dms_ctx;
    InitDmsBufContext(&dms_ctx, buf_tag);

    return ((dms_release_owner(&dms_ctx, buf_ctrl, &released) == DMS_SUCCESS) && (released != 0));
}

int32 CheckBuf4Rebuild(BufferDesc *buf_desc)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf_desc->buf_id);
    Assert(buf_ctrl != NULL);
    Assert(buf_ctrl->is_edp != 1);
    Assert(XLogRecPtrIsValid(g_instance.dms_cxt.ckptRedo));

    if (buf_ctrl->lock_mode == (unsigned char)DMS_LOCK_NULL) {
        return DMS_SUCCESS;
    }

    dms_context_t dms_ctx;
    InitDmsBufContext(&dms_ctx, buf_desc->tag);
    bool is_dirty = (buf_desc->state & (BM_DIRTY | BM_JUST_DIRTIED)) > 0 ? true : false;
    int ret = dms_buf_res_rebuild_drc(&dms_ctx, buf_ctrl, (unsigned long long)BufferGetLSN(buf_desc), is_dirty);
    if (ret != DMS_SUCCESS) {
        ereport(LOG, (errmsg("Failed to rebuild page, rel:%u/%u/%u/%d, forknum:%d, blocknum:%u.",
            buf_desc->tag.rnode.spcNode, buf_desc->tag.rnode.dbNode, buf_desc->tag.rnode.relNode,
            buf_desc->tag.rnode.bucketNode, buf_desc->tag.forkNum, buf_desc->tag.blockNum)));
        return ret;
    }
    return DMS_SUCCESS;
}

int SSLockAcquire(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock, bool dontWait,
    dms_opengauss_lock_req_type_t reqType)
{
    dms_context_t dms_ctx;
    InitDmsContext(&dms_ctx);
    SSBroadcastDDLLock ssmsg;
    ssmsg.type = BCAST_DDLLOCK;
    errno_t rc = memcpy_s(&(ssmsg.locktag), sizeof(LOCKTAG), locktag, sizeof(LOCKTAG));
    securec_check(rc, "\0", "\0");
    ssmsg.lockmode = lockmode;
    ssmsg.sessionlock = sessionLock;
    ssmsg.dontWait = dontWait;
    unsigned int count = SS_BROADCAST_FAILED_RETRYCOUNTS;
    int ret = DMS_ERROR;

    int output_backup = t_thrd.postgres_cxt.whereToSendOutput;
    t_thrd.postgres_cxt.whereToSendOutput = DestNone;
    /* retry 3 times to get the lock (22seconds) */
    while (ret != DMS_SUCCESS && !dontWait && count) {
        ret = dms_broadcast_opengauss_ddllock(&dms_ctx, (char *)&ssmsg, sizeof(SSBroadcastDDLLock),
            (unsigned char)false, dontWait ? SS_BROADCAST_WAIT_FIVE_MICROSECONDS : SS_BROADCAST_WAIT_FIVE_SECONDS,
            (unsigned char)reqType);
        if (ret == DMS_SUCCESS) {
            break;
        }
        pg_usleep(5000L);
        count--;
    }

    t_thrd.postgres_cxt.whereToSendOutput = output_backup;
    return ret;
}

int SSLockRelease(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock)
{
    dms_context_t dms_ctx;
    InitDmsContext(&dms_ctx);
    SSBroadcastDDLLock ssmsg;
    ssmsg.type = BCAST_DDLLOCKRELEASE;
    errno_t rc = memcpy_s(&(ssmsg.locktag), sizeof(LOCKTAG), locktag, sizeof(LOCKTAG));
    securec_check(rc, "\0", "\0");
    ssmsg.lockmode = lockmode;
    ssmsg.sessionlock = sessionLock;
    ssmsg.dontWait = false;

    int output_backup = t_thrd.postgres_cxt.whereToSendOutput;
    t_thrd.postgres_cxt.whereToSendOutput = DestNone;
    int ret = dms_broadcast_opengauss_ddllock(&dms_ctx, (char *)&ssmsg, sizeof(SSBroadcastDDLLock),
        (unsigned char)false, SS_BROADCAST_WAIT_FIVE_SECONDS, (unsigned char)LOCK_NORMAL_MODE);
    if (ret != DMS_SUCCESS) {
        ereport(WARNING, (errmsg("SS broadcast DDLLockRelease request failed!")));
    }

    t_thrd.postgres_cxt.whereToSendOutput = output_backup;
    return ret;
}

void SSLockReleaseAll()
{
    dms_context_t dms_ctx;
    InitDmsContext(&dms_ctx);
    SSBroadcastCmdOnly ssmsg;
    ssmsg.type = BCAST_DDLLOCKRELEASE_ALL;

    int output_backup = t_thrd.postgres_cxt.whereToSendOutput;
    t_thrd.postgres_cxt.whereToSendOutput = DestNone;
    int ret = dms_broadcast_opengauss_ddllock(&dms_ctx, (char *)&ssmsg, sizeof(SSBroadcastCmdOnly),
        (unsigned char)false, SS_BROADCAST_WAIT_FIVE_SECONDS, (unsigned char)LOCK_RELEASE_SELF);
    if (ret != DMS_SUCCESS) {
        ereport(DEBUG1, (errmsg("SS broadcast DDLLockReleaseAll request failed!")));
    }

    t_thrd.postgres_cxt.whereToSendOutput = output_backup;
}

void SSLockAcquireAll()
{
    PROCLOCK *proclock = NULL;
    HASH_SEQ_STATUS seqstat;
    int i;
    for (i = 0; i < NUM_LOCK_PARTITIONS; i++) {
        (void)LWLockAcquire(GetMainLWLockByIndex(FirstLockMgrLock + i), LW_SHARED);
    }

    hash_seq_init(&seqstat, t_thrd.storage_cxt.LockMethodProcLockHash);
    while ((proclock = (PROCLOCK *)hash_seq_search(&seqstat))) {
        if ((proclock->tag.myLock->tag.locktag_type < (uint8)LOCKTAG_PAGE ||
            proclock->tag.myLock->tag.locktag_type == (uint8)LOCKTAG_OBJECT) &&
            (proclock->holdMask & LOCKBIT_ON(AccessExclusiveLock))) {
            LOCK *lock = proclock->tag.myLock;
            int ret = SSLockAcquire(&(lock->tag), AccessExclusiveLock, false, false, LOCK_REACQUIRE);
            if (ret) {
                ereport(WARNING, (errmodule(MOD_DMS), errmsg("SS Broadcast LockAcquire when reform finished failed")));
            }
        }
    }

    for (i = NUM_LOCK_PARTITIONS; --i >= 0;) {
        LWLockRelease(GetMainLWLockByIndex(FirstLockMgrLock + i));
    }
}

void SSCheckBufferIfNeedMarkDirty(Buffer buf)
{
    dms_buf_ctrl_t* buf_ctrl = GetDmsBufCtrl(buf - 1);
    if (buf_ctrl->state & BUF_DIRTY_NEED_FLUSH) {
        MarkBufferDirty(buf);
    }
}

void SSRecheckBufferPool()
{
    uint32 buf_state;
    for (int i = 0; i < TOTAL_BUFFER_NUM; i++) {
        /*
         * BUF_DIRTY_NEED_FLUSH was removed during mark buffer dirty and lsn_on_disk was set during sync buffer
         * As BUF_DIRTY_NEED_FLUSH was set only if page lsn is bigger than ckpt redo, it should be removed at this time
         * Unfortunately if it is not, mark it dirty again. For lsn_on_disk, if it is still invalid, this means it is
         * not flushed. So if it is not dirty, invalidate it again.
         */
        BufferDesc *buf_desc = GetBufferDescriptor(i);
        pg_memory_barrier();
        buf_state = pg_atomic_read_u32(&buf_desc->state);
        if (!(buf_state & BM_VALID || buf_state & BM_TAG_VALID)) {
            continue;
        }

        dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(i);
        if (buf_ctrl->state & BUF_DIRTY_NEED_FLUSH) {
            XLogRecPtr pagelsn = BufferGetLSN(buf_desc);
            int mode = WARNING;
#ifdef USE_ASSERT_CHECKING
            mode = PANIC;
#endif
            ereport(mode,
                (errmsg("Buffer was not flushed or replayed, spc/db/rel/bucket fork-block: %u/%u/%u/%d %d-%u, page lsn (0x%llx)",
                buf_desc->tag.rnode.spcNode, buf_desc->tag.rnode.dbNode, buf_desc->tag.rnode.relNode,
                buf_desc->tag.rnode.bucketNode, buf_desc->tag.forkNum, buf_desc->tag.blockNum, (unsigned long long)pagelsn)));
        }
    }
}

void CheckPageNeedSkipInRecovery(Buffer buf)
{
    dms_buf_ctrl_t *buf_ctrl = GetDmsBufCtrl(buf - 1);
    if (buf_ctrl->lock_mode == DMS_LOCK_EXCLUSIVE) {
        return;
    }

    BufferDesc* buf_desc = GetBufferDescriptor(buf - 1);
    char pageid[DMS_PAGEID_SIZE];
    errno_t err = memcpy_s(pageid, DMS_PAGEID_SIZE, &(buf_desc->tag), sizeof(BufferTag));
    securec_check(err, "\0", "\0");
    bool skip = false;
    int ret = dms_recovery_page_need_skip(pageid, (unsigned char *)&skip);
    if (ret != DMS_SUCCESS) {
        ereport(PANIC, (errmsg("DMS Internal error happened during recovery, errno %d", ret)));
    }
    Assert(!skip);
}

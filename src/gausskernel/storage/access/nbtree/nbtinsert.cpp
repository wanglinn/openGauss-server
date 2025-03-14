/* -------------------------------------------------------------------------
 *
 * nbtinsert.cpp
 *    Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/access/nbtree/nbtinsert.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/cstore_delta.h"
#include "access/cstore_insert.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/genam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"

#define MarkItemDeadAndDirtyBuffer(itemid, opaque, buf, nbuf) \
    do {                                                      \
        ItemIdMarkDead((itemid));                             \
        (opaque)->btpo_flags |= BTP_HAS_GARBAGE;              \
        if ((nbuf) != InvalidBuffer) {                        \
            MarkBufferDirtyHint((nbuf), true);                \
        } else {                                              \
            MarkBufferDirtyHint((buf), true);                 \
        }                                                     \
    } while (0)

/* Minimum tree height for application of fastpath optimization */
#define BTREE_FASTPATH_MIN_LEVEL  2

typedef struct {
    /* context data for _bt_checksplitloc */
    Size newitemsz;          /* size of new item to be inserted */
    int fillfactor;          /* needed when splitting rightmost page */
    bool is_leaf;            /* T if splitting a leaf page */
    bool is_rightmost;       /* T if splitting a rightmost page */
    OffsetNumber newitemoff; /* where the new item is to be inserted */
    int leftspace;           /* space available for items on left page */
    int rightspace;          /* space available for items on right page */
    int olddataitemstotal;   /* space taken by old items */

    bool have_split; /* found a valid split? */

    /* these fields valid only if have_split is true */
    bool newitemonleft;      /* new item on left or right of best split */
    OffsetNumber firstright; /* best split point */
    int best_delta;          /* best size delta so far */
} FindSplitData;

static Buffer _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);
static void _bt_findinsertloc(Relation rel, Buffer *bufptr, OffsetNumber *offsetptr, int keysz, ScanKey scankey,
                              IndexTuple newtup, BTStack stack, Relation heapRel);
static void _bt_insertonpg(Relation rel, Buffer buf, Buffer cbuf, BTStack stack, IndexTuple itup,
                           OffsetNumber newitemoff, bool split_only_page, bool useFastPath);
static Buffer _bt_split(Relation rel, Buffer buf, Buffer cbuf, OffsetNumber firstright, OffsetNumber newitemoff,
                        Size newitemsz, IndexTuple newitem, bool newitemonleft);
static OffsetNumber _bt_findsplitloc(Relation rel, Page page, OffsetNumber newitemoff, Size newitemsz,
                                     bool *newitemonleft);
static void _bt_checksplitloc(FindSplitData *state, OffsetNumber firstoldonright, bool newitemonleft,
                              int dataitemstoleft, Size firstoldonrightsz);
static bool _bt_pgaddtup(Page page, Size itemsize, IndexTuple itup, OffsetNumber itup_off);
static bool _bt_isequal(Relation idxrel, Page page, OffsetNumber offnum, int keysz, ScanKey scankey);
static void _bt_vacuum_one_page(Relation rel, Buffer buffer, Relation heapRel);
static bool CheckItemIsAlive(ItemPointer tid, Relation relation, Snapshot snapshot, bool* all_dead,
                             CUDescScan* cudescScan);

/*
 *	_bt_doinsert() -- Handle insertion of a single index tuple in the tree.
 *
 *		This routine is called by the public interface routines, btbuild
 *		and btinsert.  By here, itup is filled in, including the TID.
 *
 *		If checkUnique is UNIQUE_CHECK_NO or UNIQUE_CHECK_PARTIAL, this
 *		will allow duplicates.	Otherwise (UNIQUE_CHECK_YES or
 *		UNIQUE_CHECK_EXISTING) it will throw error for a duplicate.
 *		For UNIQUE_CHECK_EXISTING we merely run the duplicate check, and
 *		don't actually insert.
 *
 *		The result value is only significant for UNIQUE_CHECK_PARTIAL:
 *		it must be TRUE if the entry is known unique, else FALSE.
 *		(In the current implementation we'll also return TRUE after a
 *		successful UNIQUE_CHECK_YES or UNIQUE_CHECK_EXISTING call, but
 *		that's just a coding artifact.)
 */
bool _bt_doinsert(Relation rel, IndexTuple itup, IndexUniqueCheck checkUnique, Relation heapRel)
{
    bool is_unique = false;
    int indnkeyatts;
    ScanKey itup_scankey;
    BTStack stack = NULL;
    Buffer buf;
    OffsetNumber offset;
    Oid indexHeapRelOid = InvalidOid;
    Relation indexHeapRel = NULL;
    Partition part = NULL;
    Relation partRel = NULL;

    GPIScanDesc gpiScan = NULL;
    CBIScanDesc cbiScan = NULL;
    CUDescScan* cudescScan = NULL;

    if (RelationIsGlobalIndex(rel)) {
        GPIScanInit(&gpiScan);
        indexHeapRelOid = IndexGetRelation(rel->rd_id, false);
        if (indexHeapRelOid == heapRel->grandparentId) { // For subpartitiontable
            indexHeapRel = relation_open(indexHeapRelOid, AccessShareLock);
            Assert(RelationIsSubPartitioned(indexHeapRel));
            part = partitionOpen(indexHeapRel, heapRel->parentId, AccessShareLock);
            partRel = partitionGetRelation(indexHeapRel, part);
            gpiScan->parentRelation = partRel;
            partitionClose(indexHeapRel, part, AccessShareLock);
        } else {
            gpiScan->parentRelation = relation_open(heapRel->parentId, AccessShareLock);
        }
    }
    if (RelationIsCrossBucketIndex(rel)) {
        cbi_scan_init(&cbiScan);
        cbiScan->parentRelation = heapRel->parent;
    }

    if (RelationIsCUFormat(heapRel)) {
        cudescScan = (CUDescScan*)New(CurrentMemoryContext) CUDescScan(heapRel);
    }

    BTCheckElement element;
    is_unique = SearchBufferAndCheckUnique(rel, itup, checkUnique, heapRel,
        gpiScan, cbiScan, cudescScan, &element);

    itup_scankey = element.itupScanKey;
    stack = element.btStack;
    buf = element.buffer;
    offset = element.offset;
    indnkeyatts = element.indnkeyatts;

    if (checkUnique != UNIQUE_CHECK_EXISTING) {
        /*
         * The only conflict predicate locking cares about for indexes is when
         * an index tuple insert conflicts with an existing lock.  Since the
         * actual location of the insert is hard to predict because of the
         * random search used to prevent O(N^2) performance when there are
         * many duplicate entries, we can just use the "first valid" page.
         * This reasoning also applies to INCLUDE indexes, whose extra
         * attributes are not considered part of the key space.
         */
        CheckForSerializableConflictIn(rel, NULL, buf);
        /* do the insertion */
        _bt_findinsertloc(rel, &buf, &offset, indnkeyatts, itup_scankey, itup, stack, heapRel);
        _bt_insertonpg(rel, buf, InvalidBuffer, stack, itup, offset, false, element.useFastPath);
    } else {
        /* just release the buffer */
        _bt_relbuf(rel, buf);
    }
    /* be tidy */
    if (stack) {
        _bt_freestack(stack);
    }
    _bt_freeskey(itup_scankey);

    if (gpiScan != NULL) { // means rel switch happened
        if (indexHeapRelOid == heapRel->grandparentId) { // For subpartitiontable
            releaseDummyRelation(&partRel);
            relation_close(indexHeapRel, AccessShareLock);
        } else {
            relation_close(gpiScan->parentRelation, AccessShareLock);
        }
        GPIScanEnd(gpiScan);
    }

    if (cbiScan != NULL) {
        cbi_scan_end(cbiScan);
    }

    if (cudescScan != NULL) {
        cudescScan->Destroy();
        delete cudescScan;
        cudescScan = NULL;
    }

    return is_unique;
}

bool SearchBufferAndCheckUnique(Relation rel, IndexTuple itup, IndexUniqueCheck checkUnique, Relation heapRel,
    GPIScanDesc gpiScan, CBIScanDesc cbiScan, CUDescScan* cudescScan, BTCheckElement* element)
{
    bool is_unique = false;
    int indnkeyatts;
    ScanKey itup_scankey;
    BTStack stack = NULL;
    Buffer buf;
    OffsetNumber offset;
    bool fastpath = false;

    indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
    Assert(indnkeyatts != 0);
    /* we need an insertion scan key to do our search, so build one */
    itup_scankey = _bt_mkscankey(rel, itup);

    /*
     * It's very common to have an index on an auto-incremented or
     * monotonically increasing value. In such cases, every insertion happens
     * towards the end of the index. We try to optimise that case by caching
     * the right-most leaf of the index. If our cached block is still the
     * rightmost leaf, has enough free space to accommodate a new entry and
     * the insertion key is strictly greater than the first key in this page,
     * then we can safely conclude that the new key will be inserted in the
     * cached block. So we simply search within the cached block and insert the
     * key at the appropriate location. We call it a fastpath.
     *
     * Testing has revealed, though, that the fastpath can result in increased
     * contention on the exclusive-lock on the rightmost leaf page. So we
     * conditionally check if the lock is available. If it's not available then
     * we simply abandon the fastpath and take the regular path. This makes
     * sense because unavailability of the lock also signals that some other
     * backend might be concurrently inserting into the page, thus reducing our
     * chances to finding an insertion place in this page.
     */
top:
    fastpath = false;

    offset = InvalidOffsetNumber;
    if (RelationGetTargetBlock(rel) != InvalidBlockNumber) {
        Size            itemsz;
        Page            page;
        BTPageOpaqueInternal    lpageop;

        /*
         * Conditionally acquire exclusive lock on the buffer before doing any
         * checks. If we don't get the lock, we simply follow slowpath. If we
         * do get the lock, this ensures that the index state cannot change, as
         * far as the rightmost part of the index is concerned.
         */
        buf = ReadBuffer(rel, RelationGetTargetBlock(rel));

        if (ConditionalLockBuffer(buf)) {
            _bt_checkpage(rel, buf);

            page = BufferGetPage(buf);

            lpageop = (BTPageOpaqueInternal) PageGetSpecialPointer(page);
            itemsz = IndexTupleSize(itup);
            itemsz = MAXALIGN(itemsz);  /* be safe, PageAddItem will do this
                                         * but we need to be consistent */

            /*
             * Check if the page is still the rightmost leaf page, has enough
             * free space to accommodate the new tuple, no split is in progress
             * and the scankey is greater than or equal to the first key on the
             * page.
             */
            if (P_ISLEAF(lpageop) && P_RIGHTMOST(lpageop) &&
                    !P_IGNORE(lpageop) &&
                    (PageGetFreeSpace(page) > itemsz) &&
                    PageGetMaxOffsetNumber(page) >= P_FIRSTDATAKEY(lpageop) &&
                    _bt_compare(rel, indnkeyatts, itup_scankey, page,
                        P_FIRSTDATAKEY(lpageop)) > 0) {
                /*
                 * The right-most block should never have incomplete split. But
                 * be paranoid and check for it anyway.
                 */
                Assert(!P_INCOMPLETE_SPLIT(lpageop));
                fastpath = true;
            } else {
                _bt_relbuf(rel, buf);

                /*
                 * Something did not workout. Just forget about the cached
                 * block and follow the normal path. It might be set again if
                 * the conditions are favourble.
                 */
                RelationSetTargetBlock(rel, InvalidBlockNumber);
            }
        } else {
            ReleaseBuffer(buf);

            /*
             * If someone's holding a lock, it's likely to change anyway,
             * so don't try again until we get an updated rightmost leaf.
             */
            RelationSetTargetBlock(rel, InvalidBlockNumber);
        }
    }
    if (!fastpath) {
        /*
         * Find the first page containing this key.  Buffer returned by
         * _bt_search() is locked in exclusive mode.
         */
        stack = _bt_search(rel, indnkeyatts, itup_scankey, false, &buf, BT_WRITE);

        /* trade in our read lock for a write lock */
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        LockBuffer(buf, BT_WRITE);

        /*
         * If the page was split between the time that we surrendered our read
         * lock and acquired our write lock, then this page may no longer be the
         * right place for the key we want to insert.  In this case, we need to
         * move right in the tree.	See Lehman and Yao for an excruciatingly
         * precise description.
        */
        buf = _bt_moveright(rel, buf, indnkeyatts, itup_scankey, false, true, stack, BT_WRITE);
    }
    /*
     * If we're not allowing duplicates, make sure the key isn't already in
     * the index.
     *
     * NOTE: obviously, _bt_check_unique can only detect keys that are already
     * in the index; so it cannot defend against concurrent insertions of the
     * same key.  We protect against that by means of holding a write lock on
     * the target page.  Any other would-be inserter of the same key must
     * acquire a write lock on the same target page, so only one would-be
     * inserter can be making the check at one time.  Furthermore, once we are
     * past the check we hold write locks continuously until we have performed
     * our insertion, so no later inserter can fail to see our insertion.
     * (This requires some care in _bt_insertonpg.)
     *
     * If we must wait for another xact, we release the lock while waiting,
     * and then must start over completely.
     *
     * For a partial uniqueness check, we don't wait for the other xact. Just
     * let the tuple in and return false for possibly non-unique, or true for
     * definitely unique.
     */
    if (checkUnique != UNIQUE_CHECK_NO) {
        TransactionId xwait;

        offset = _bt_binsrch(rel, buf, indnkeyatts, itup_scankey, false);
        xwait = _bt_check_unique(rel, itup, heapRel, buf, offset, itup_scankey,
            checkUnique, &is_unique, gpiScan, cbiScan, cudescScan);

        if (TransactionIdIsValid(xwait)) {
            /* Have to wait for the other guy ... */
            _bt_relbuf(rel, buf);
            XactLockTableWait(xwait);
            /* start over... */
            if (stack) {
                _bt_freestack(stack);
            }
            goto top;
        }
    }

    element->btStack = stack;
    element->itupScanKey = itup_scankey;
    element->buffer = buf;
    element->offset = offset;
    element->indnkeyatts = indnkeyatts;
    element->useFastPath = fastpath;
    element->targetBlock = RelationGetTargetBlock(rel);

    return is_unique;
}

/*
 *	_bt_check_unique() -- Check for violation of unique index constraint
 *
 * offset points to the first possible item that could conflict. It can
 * also point to end-of-page, which means that the first tuple to check
 * is the first tuple on the next page.
 *
 * Returns InvalidTransactionId if there is no conflict, else an xact ID
 * we must wait for to see if it commits a conflicting tuple.	If an actual
 * conflict is detected, no return --- just ereport().
 *
 * However, if checkUnique == UNIQUE_CHECK_PARTIAL, we always return
 * InvalidTransactionId because we don't want to wait.  In this case we
 * set *is_unique to false if there is a potential conflict, and the
 * core code must redo the uniqueness check later.
 */
TransactionId _bt_check_unique(Relation rel, IndexTuple itup, Relation heapRel, Buffer buf, OffsetNumber offset,
    ScanKey itup_scankey, IndexUniqueCheck checkUnique, bool* is_unique, GPIScanDesc gpiScan, CBIScanDesc cbiScan,
    CUDescScan* cudescScan)
{
    int indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
    SnapshotData SnapshotDirty;
    OffsetNumber maxoff;
    Page page;
    BTPageOpaqueInternal opaque;
    Buffer nbuf = InvalidBuffer;
    bool found = false;
    Relation tarRel = heapRel;
    Relation hbktParentHeapRel = NULL;

    /* Assume unique until we find a duplicate */
    *is_unique = true;

    InitDirtySnapshot(SnapshotDirty);

    if (cudescScan != NULL) {
        cudescScan->ResetSnapshot(&SnapshotDirty);
    }

    page = BufferGetPage(buf);
    opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
    maxoff = PageGetMaxOffsetNumber(page);

    /*
     * Scan over all equal tuples, looking for live conflicts.
     */
    for (;;) {
        ItemId curitemid;
        IndexTuple curitup;
        BlockNumber nblkno;

        /*
         * make sure the offset points to an actual item before trying to
         * examine it...
         */
        if (offset <= maxoff) {
            curitemid = PageGetItemId(page, offset);
            /*
             * We can skip items that are marked killed.
             *
             * Formerly, we applied _bt_isequal() before checking the kill
             * flag, so as to fall out of the item loop as soon as possible.
             * However, in the presence of heavy update activity an index may
             * contain many killed items with the same key; running
             * _bt_isequal() on each killed item gets expensive. Furthermore
             * it is likely that the non-killed version of each key appears
             * first, so that we didn't actually get to exit any sooner
             * anyway. So now we just advance over killed items as quickly as
             * we can. We only apply _bt_isequal() when we get to a non-killed
             * item or the end of the page.
             */
            if (!ItemIdIsDead(curitemid)) {
                ItemPointerData htid;
                bool all_dead = false;

                /*
                 * _bt_compare returns 0 for (1,NULL) and (1,NULL) - this's
                 * how we handling NULLs - and so we must not use _bt_compare
                 * in real comparison, but only for ordering/finding items on
                 * pages. - vadim 03/24/97
                 */
                if (!_bt_isequal(rel, page, offset, indnkeyatts, itup_scankey))
                    break; /* we're past all the equal tuples */

                /* okay, we gotta fetch the heap tuple ... */
                curitup = (IndexTuple)PageGetItem(page, curitemid);
                htid = curitup->t_tid;
                Oid curPartOid = InvalidOid;
                int2 bucketid = InvalidBktId;
                if (RelationIsGlobalIndex(rel)) {
                    /* global partitioned index without crossbucket */
                    curPartOid = index_getattr_tableoid(rel, curitup);
                    if (curPartOid != gpiScan->currPartOid) {
                        GPISetCurrPartOid(gpiScan, curPartOid);
                        if (!GPIGetNextPartRelation(gpiScan, CurrentMemoryContext, AccessShareLock)) {
                            if (CheckPartitionIsInvisible(gpiScan)) {
                                MarkItemDeadAndDirtyBuffer(curitemid, opaque, buf, nbuf);
                            }
                            goto next;
                        } else {
                            hbktParentHeapRel = tarRel = gpiScan->fakePartRelation;
                        }
                    }
                }
                if (RelationIsCrossBucketIndex(rel) && RELATION_OWN_BUCKET(rel)) {
                    bucketid = index_getattr_bucketid(rel, curitup);
                    if (cbiScan->bucketid != bucketid) {
                        cbi_set_bucketid(cbiScan, bucketid);
                    }
                    if (cbiScan->parentRelation != hbktParentHeapRel && RelationIsValid(hbktParentHeapRel)) {
                        cbiScan->parentRelation = hbktParentHeapRel;
                    }
                    if (!cbi_get_bucket_relation(cbiScan, CurrentMemoryContext)) {
                        MarkItemDeadAndDirtyBuffer(curitemid, opaque, buf, nbuf);
                        goto next;
                    } else {
                        tarRel = cbiScan->fakeBucketRelation;
                    }
                }

                /*
                 * If we are doing a recheck, we expect to find the tuple we
                 * are rechecking.	It's not a duplicate, but we have to keep
                 * scanning. For global partition index, part oid in index tuple
                 * is supposed to be same as heapRel oid, add check in case
                 * abnormal condition.
                 */
                if (checkUnique == UNIQUE_CHECK_EXISTING && ItemPointerCompare(&htid, &itup->t_tid) == 0) {
                    if (RelationIsGlobalIndex(rel) && curPartOid != heapRel->rd_id) {
                        ereport(ERROR,
                            (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("failed to re-find tuple within GPI \"%s\"", RelationGetRelationName(rel))));
                    }
                    found = true;
                } else if (CheckItemIsAlive(&htid, tarRel, &SnapshotDirty, &all_dead, cudescScan)) {
                    /*
                     * We check the whole HOT-chain to see if there is any tuple
                     * that satisfies SnapshotDirty.  This is necessary because we
                     * have just a single index entry for the entire chain.
                     */
                    TransactionId xwait;

                    /*
                     * It is a duplicate. If we are only doing a partial
                     * check, then don't bother checking if the tuple is being
                     * updated in another transaction. Just return the fact
                     * that it is a potential conflict and leave the full
                     * check till later.
                     */
                    if (checkUnique == UNIQUE_CHECK_PARTIAL) {
                        if (nbuf != InvalidBuffer)
                            _bt_relbuf(rel, nbuf);
                        *is_unique = false;
                        return InvalidTransactionId;
                    }

                    /*
                     * If this tuple is being updated by other transaction
                     * then we have to wait for its commit/abort.
                     */
                    xwait = (TransactionIdIsValid(SnapshotDirty.xmin)) ? SnapshotDirty.xmin : SnapshotDirty.xmax;
                    if (TransactionIdIsValid(xwait)) {
                        if (nbuf != InvalidBuffer)
                            _bt_relbuf(rel, nbuf);
                        /* Tell _bt_doinsert to wait... */
                        return xwait;
                    }

                    /*
                     * Otherwise we have a definite conflict.  But before
                     * complaining, look to see if the tuple we want to insert
                     * is itself now committed dead --- if so, don't complain.
                     * This is a waste of time in normal scenarios but we must
                     * do it to support CREATE INDEX CONCURRENTLY.
                     *
                     * We must follow HOT-chains here because during
                     * concurrent index build, we insert the root TID though
                     * the actual tuple may be somewhere in the HOT-chain.
                     * While following the chain we might not stop at the
                     * exact tuple which triggered the insert, but that's OK
                     * because if we find a live tuple anywhere in this chain,
                     * we have a unique key conflict.  The other live tuple is
                     * not part of this chain because it had a different index
                     * entry.
                     *
                     * If the itup doesn't point to the heapRel, we skip this check.
                     */
                    if (ItemPointerIsValid(&(itup->t_tid))) {
                        htid = itup->t_tid;
                        if (cudescScan != NULL) {
                            cudescScan->ResetSnapshot(SnapshotSelf);
                        }
                        if (CheckItemIsAlive(&htid, heapRel, SnapshotSelf, NULL, cudescScan)) {
                            /* Normal case --- it's still live */
                        } else {
                            /*
                            * It's been deleted, so no error, and no need to
                            * continue searching
                            */
                            break;
                        }
                    }

                    /*
                     * This is a definite conflict.  Break the tuple down into
                     * datums and report the error.  But first, make sure we
                     * release the buffer locks we're holding ---
                     * BuildIndexValueDescription could make catalog accesses,
                     * which in the worst case might touch this same index and
                     * cause deadlocks.
                     */
                    if (nbuf != InvalidBuffer)
                        _bt_relbuf(rel, nbuf);
                    _bt_relbuf(rel, buf);

                    {
                        Datum values[INDEX_MAX_KEYS];
                        bool isnull[INDEX_MAX_KEYS];
                        char *key_desc = NULL;

                        index_deform_tuple(itup, RelationGetDescr(rel), values, isnull);

                        key_desc = BuildIndexValueDescription(rel, values, isnull);

                        /* save the primary scene once index relation of CU Descriptor
                         * violates unique constraint. this means wrong max cu id happens.
                         */
                        Assert(0 != memcmp(NameStr(rel->rd_rel->relname), "pg_cudesc_", strlen("pg_cudesc_")));

                        const char* relationName = NULL;
                        if (memcmp(RelationGetRelationName(rel), "pg_delta_", strlen("pg_delta_")) == 0) {
                            /* index name is like pg_delta_index_xxxx or pg_delta_part_index_xxx. */
                            relationName = GetCUIdxNameFromDeltaIdx(rel);
                        } else {
                            relationName = RelationGetRelationName(rel);
                        }

                        ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
                                        errmsg("duplicate key value violates unique constraint \"%s\"",
                                               relationName),
                                        key_desc ? errdetail("Key %s already exists.", key_desc) : 0));
                    }
                } else if (all_dead) {
                    /*
                     * The conflicting tuple (or whole HOT chain) is dead to
                     * everyone, so we may as well mark the index entry
                     * killed.
                     */
                    ItemIdMarkDead(curitemid);
                    opaque->btpo_flags |= BTP_HAS_GARBAGE;

                    /*
                     * Mark buffer with a dirty hint, since state is not
                     * crucial. Be sure to mark the proper buffer dirty.
                     */
                    if (nbuf != InvalidBuffer)
                        MarkBufferDirtyHint(nbuf, true);
                    else
                        MarkBufferDirtyHint(buf, true);
                }
            }
        }

next:
        /*
         * Advance to next tuple to continue checking.
         */
        if (offset < maxoff)
            offset = OffsetNumberNext(offset);
        else {
            /* If scankey == hikey we gotta check the next page too */
            if (P_RIGHTMOST(opaque))
                break;
            if (!_bt_isequal(rel, page, P_HIKEY, indnkeyatts, itup_scankey))
                break;
            /* Advance to next non-dead page --- there must be one */
            for (;;) {
                nblkno = opaque->btpo_next;
                nbuf = _bt_relandgetbuf(rel, nbuf, nblkno, BT_READ);
                page = BufferGetPage(nbuf);
                opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
                if (!P_IGNORE(opaque))
                    break;
                if (P_RIGHTMOST(opaque))
                    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                    errmsg("fell off the end of index \"%s\"", RelationGetRelationName(rel))));
            }
            maxoff = PageGetMaxOffsetNumber(page);
            offset = P_FIRSTDATAKEY(opaque);
        }
    }

    /*
     * If we are doing a recheck then we should have found the tuple we are
     * checking.  Otherwise there's something very wrong --- probably, the
     * index is on a non-immutable expression.
     */
    if (checkUnique == UNIQUE_CHECK_EXISTING && !found)
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("failed to re-find tuple within index \"%s\"", RelationGetRelationName(rel)),
                        errhint("This may be because of a non-immutable index expression.")));

    if (nbuf != InvalidBuffer)
        _bt_relbuf(rel, nbuf);

    return InvalidTransactionId;
}

/*
 *	_bt_findinsertloc() -- Finds an insert location for a tuple
 *
 *		If the new key is equal to one or more existing keys, we can
 *		legitimately place it anywhere in the series of equal keys --- in fact,
 *		if the new key is equal to the page's "high key" we can place it on
 *		the next page.	If it is equal to the high key, and there's not room
 *		to insert the new tuple on the current page without splitting, then
 *		we can move right hoping to find more free space and avoid a split.
 *		(We should not move right indefinitely, however, since that leads to
 *		O(N^2) insertion behavior in the presence of many equal keys.)
 *		Once we have chosen the page to put the key on, we'll insert it before
 *		any existing equal keys because of the way _bt_binsrch() works.
 *
 *		If there's not enough room in the space, we try to make room by
 *		removing any LP_DEAD tuples.
 *
 *		On entry, *buf and *offsetptr point to the first legal position
 *		where the new tuple could be inserted.	The caller should hold an
 *		exclusive lock on *buf.  *offsetptr can also be set to
 *		InvalidOffsetNumber, in which case the function will search for the
 *		right location within the page if needed.  On exit, they point to the
 *		chosen insert location.  If _bt_findinsertloc decides to move right,
 *		the lock and pin on the original page will be released and the new
 *		page returned to the caller is exclusively locked instead.
 *
 *		newtup is the new tuple we're inserting, and scankey is an insertion
 *		type scan key for it.
 */
static void _bt_findinsertloc(Relation rel, Buffer *bufptr, OffsetNumber *offsetptr, int keysz, ScanKey scankey,
                              IndexTuple newtup, BTStack stack, Relation heapRel)
{
    Buffer buf = *bufptr;
    Page page = BufferGetPage(buf);
    Size itemsz;
    BTPageOpaqueInternal lpageop;
    bool movedright = false;
    bool vacuumed = false;
    OffsetNumber newitemoff;
    OffsetNumber firstlegaloff = *offsetptr;

    lpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(page);

    itemsz = IndexTupleDSize(*newtup);
    itemsz = MAXALIGN(itemsz); /* be safe, PageAddItem will do this but we
                                * need to be consistent */
    /*
     * Check whether the item can fit on a btree page at all. (Eventually, we
     * ought to try to apply TOAST methods if not.) We actually need to be
     * able to fit three items on every page, so restrict any one item to 1/3
     * the per-page available space. Note that at this point, itemsz doesn't
     * include the ItemId.
     */
    if (itemsz > (Size)BTMaxItemSize(page))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("index row size %lu exceeds maximum %lu for index \"%s\"", (unsigned long)itemsz,
                               (unsigned long)BTMaxItemSize(page), RelationGetRelationName(rel)),
                        errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
                                "Consider a function index of an MD5 hash of the value, "
                                "or use full text indexing.")));

    /*
     * If we will need to split the page to put the item on this page,
     * check whether we can put the tuple somewhere to the right,
     * instead.  Keep scanning right until we
     * (a) find a page with enough free space,
     * (b) reach the last page where the tuple can legally go, or
     * (c) get tired of searching.
     * (c) is not flippant; it is important because if there are many
     * pages' worth of equal keys, it's better to split one of the early
     * pages than to scan all the way to the end of the run of equal keys
     * on every insert.  We implement "get tired" as a random choice,
     * since stopping after scanning a fixed number of pages wouldn't work
     * well (we'd never reach the right-hand side of previously split
     * pages). Currently the probability of moving right is set at 0.99,
     * which may seem too high to change the behavior much, but it does an
     * excellent job of preventing O(N^2) behavior with many equal keys.
     */
    movedright = false;
    vacuumed = false;
    while (PageGetFreeSpace(page) < itemsz) {
        Buffer rbuf;
        BlockNumber rblkno;

        /*
         * before considering moving right, see if we can obtain enough space
         * by erasing LP_DEAD items
         */
        if (P_ISLEAF(lpageop) && P_HAS_GARBAGE(lpageop)) {
            _bt_vacuum_one_page(rel, buf, heapRel);
            /*
             * remember that we vacuumed this page, because that makes the
             * hint supplied by the caller invalid
             */
            vacuumed = true;

            if (PageGetFreeSpace(page) >= itemsz)
                break; /* OK, now we have enough space */
        }

        /*
         * nope, so check conditions (b) and (c) enumerated above
         */
        if (P_RIGHTMOST(lpageop) || _bt_compare(rel, keysz, scankey, page, P_HIKEY) != 0 ||
            random() <= (MAX_RANDOM_VALUE / 100))
            break;

        /*
         * step right to next non-dead page
         *
         * must write-lock that page before releasing write lock on current
         * page; else someone else's _bt_check_unique scan could fail to see
         * our insertion.  write locks on intermediate dead pages won't do
         * because we don't know when they will get de-linked from the tree.
         */
        rbuf = InvalidBuffer;
        rblkno = lpageop->btpo_next;

        for (;;) {
            rbuf = _bt_relandgetbuf(rel, rbuf, rblkno, BT_WRITE);
            page = BufferGetPage(rbuf);
            lpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(page);

            /*
             * If this page was incompletely split, finish the split now.
             * We do this while holding a lock on the left sibling, which
             * is not good because finishing the split could be a fairly
             * lengthy operation.  But this should happen very seldom.
             */
            if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
                if (P_INCOMPLETE_SPLIT(lpageop)) {
                    _bt_finish_split(rel, rbuf, stack);
                    rbuf = InvalidBuffer;
                    continue;
                }
            }
            if (!P_IGNORE(lpageop))
                break;
            if (P_RIGHTMOST(lpageop))
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("fell off the end of index \"%s\"", RelationGetRelationName(rel))));

            rblkno = lpageop->btpo_next;
        }
        _bt_relbuf(rel, buf);
        buf = rbuf;
        movedright = true;
        vacuumed = false;
    }

    /*
     * Now we are on the right page, so find the insert position. If we moved
     * right at all, we know we should insert at the start of the page. If we
     * didn't move right, we can use the firstlegaloff hint if the caller
     * supplied one, unless we vacuumed the page which might have moved tuples
     * around making the hint invalid. If we didn't move right or can't use
     * the hint, find the position by searching.
     */
    if (movedright)
        newitemoff = P_FIRSTDATAKEY(lpageop);
    else if (firstlegaloff != InvalidOffsetNumber && !vacuumed)
        newitemoff = firstlegaloff;
    else
        newitemoff = _bt_binsrch(rel, buf, keysz, scankey, false);

    *bufptr = buf;
    *offsetptr = newitemoff;
}

/*
 * Insert a tuple on a particular page in the index.
 *
 * This recursive procedure does the following things:
 *
 * On entry, we must have the correct buffer in which to do the
 * insertion, and the buffer must be pinned and write-locked.	On return,
 * we will have dropped both the pin and the lock on the buffer.
 *
 * When inserting to a non-leaf page, 'cbuf' is the left-sibling of the
 * page we're inserting the downlink for.  This function will clear the
 * INCOMPLETE_SPLIT flag on it, and release the buffer.
 *
 * The locking interactions in this code are critical.  You should
 * grok Lehman and Yao's paper before making any changes.  In addition,
 * you need to understand how we disambiguate duplicate keys in this
 * implementation, in order to be able to find our location using
 * L&Y "move right" operations.  Since we may insert duplicate user
 * keys, and since these dups may propagate up the tree, we use the
 * 'afteritem' parameter to position ourselves correctly for the
 * insertion on internal pages.
 */
static void _bt_insertonpg(Relation rel, Buffer buf, Buffer cbuf, BTStack stack, IndexTuple itup,
                           OffsetNumber newitemoff, bool split_only_page, bool useFastPath)
{
    Page page;
    BTPageOpaqueInternal lpageop;
    OffsetNumber firstright = InvalidOffsetNumber;
    Size itemsz;

    page = BufferGetPage(buf);
    lpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(page);

    if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
        /* child buffer must be given iff inserting on an internal page */
        Assert(P_ISLEAF(lpageop) == !BufferIsValid(cbuf));

        /* The caller should've finished any incomplete splits already. */
        if (P_INCOMPLETE_SPLIT(lpageop)) {
            elog(ERROR, "cannot insert to incompletely split page %u", BufferGetBlockNumber(buf));
        }
    }

    itemsz = IndexTupleDSize(*itup);
    itemsz = MAXALIGN(itemsz);
    /*
     * Do we need to split the page to fit the item on it?
     *
     * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its result,
     * so this comparison is correct even though we appear to be accounting
     * only for the item and not for its line pointer.
     */
    if (PageGetFreeSpace(page) < itemsz) {
        bool is_root = P_ISROOT(lpageop) > 0 ? true : false;
        bool is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(lpageop);
        bool newitemonleft = false;
        Buffer rbuf;

        /*
         * If we're here then a pagesplit is needed. We should never reach here
         * if we're using the fastpath since we should have checked for all the
         * required conditions, including the fact that this page has enough
         * freespace. Note that this routine can in theory deal with the
         * situation where a NULL stack pointer is passed (that's what would
         * happen if the fastpath is taken), like it does during crash
         * recovery. But that path is much slower, defeating the very purpose
         * of the optimization.  The following assertion should protect us from
         * any future code changes that invalidate those assumptions.
         *
         * Note that whenever we fail to take the fastpath, we clear the
         * cached block. Checking for a valid cached block at this point is
         * enough to decide whether we're in a fastpath or not.
         */
        Assert(!useFastPath);

        /* Choose the split point */
        firstright = _bt_findsplitloc(rel, page, newitemoff, itemsz, &newitemonleft);

        /* split the buffer into left and right halves */
        rbuf = _bt_split(rel, buf, cbuf, firstright, newitemoff, itemsz, itup, newitemonleft);
        PredicateLockPageSplit(rel, BufferGetBlockNumber(buf), BufferGetBlockNumber(rbuf));

        /* ----------
         * By here,
         *
         *		+  our target page has been split;
         *		+  the original tuple has been inserted;
         *		+  we have write locks on both the old (left half)
         *		   and new (right half) buffers, after the split; and
         *		+  we know the key we want to insert into the parent
         *		   (it's the "high key" on the left child page).
         *
         * We're ready to do the parent insertion.  We need to hold onto the
         * locks for the child pages until we locate the parent, but we can
         * release them before doing the actual insertion (see Lehman and Yao
         * for the reasoning).
         * ----------
         */
        _bt_insert_parent(rel, buf, rbuf, stack, is_root, is_only);
    } else {
        Buffer metabuf = InvalidBuffer;
        Page metapg = NULL;
        BTMetaPageData *metad = NULL;
        OffsetNumber itup_off;
        BlockNumber itup_blkno;
        BlockNumber cachedBlock = InvalidBlockNumber;

        itup_off = newitemoff;
        itup_blkno = BufferGetBlockNumber(buf);

        /*
         * If we are doing this insert because we split a page that was the
         * only one on its tree level, but was not the root, it may have been
         * the "fast root".  We need to ensure that the fast root link points
         * at or above the current page.  We can safely acquire a lock on the
         * metapage here --- see comments for _bt_newroot().
         */
        if (split_only_page) {
            Assert(!P_ISLEAF(lpageop));

            metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
            metapg = BufferGetPage(metabuf);
            metad = BTPageGetMeta(metapg);
            if (metad->btm_fastlevel >= lpageop->btpo.level) {
                /* no update wanted */
                _bt_relbuf(rel, metabuf);
                metabuf = InvalidBuffer;
            }
        }

        /* Do the update.  No ereport(ERROR) until changes are logged */
        START_CRIT_SECTION();

        if (!_bt_pgaddtup(page, itemsz, itup, newitemoff))
            ereport(PANIC,
                    (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("failed to add new item to block %u in index \"%s\"",
                                                              itup_blkno, RelationGetRelationName(rel))));

        MarkBufferDirty(buf);

        if (BufferIsValid(metabuf)) {
            metad->btm_fastroot = itup_blkno;
            metad->btm_fastlevel = lpageop->btpo.level;
            MarkBufferDirty(metabuf);
        }

        if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            /* clear INCOMPLETE_SPLIT flag on child if inserting a downlink */
            if (BufferIsValid(cbuf)) {
                Page cpage = BufferGetPage(cbuf);
                BTPageOpaqueInternal cpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(cpage);
                Assert(P_INCOMPLETE_SPLIT(cpageop));
                cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
                MarkBufferDirty(cbuf);
            }
        }

        /*
         * Cache the block information if we just inserted into the rightmost
         * leaf page of the index and it's not the root page.  For very small
         * index where root is also the leaf, there is no point trying for any
         * optimization.
         */
        if (P_RIGHTMOST(lpageop) && P_ISLEAF(lpageop) && !P_ISROOT(lpageop))
            cachedBlock = BufferGetBlockNumber(buf);

        /* XLOG stuff */
        if (RelationNeedsWAL(rel)) {
            xl_btree_insert xlrec;
            BlockNumber xldownlink;
            xl_btree_metadata xlmeta;
            uint8 xlinfo;
            XLogRecPtr recptr;
            IndexTupleData trunctuple;

            xlrec.offnum = itup_off;

            XLogBeginInsert();
            XLogRegisterData((char *)&xlrec, SizeOfBtreeInsert);

            if (P_ISLEAF(lpageop))
                xlinfo = XLOG_BTREE_INSERT_LEAF;
            else {
                if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
                    xldownlink = ItemPointerGetBlockNumber(&(itup->t_tid));
                    XLogRegisterData((char*)&xldownlink, sizeof(BlockNumber));
                } else {
                    /*
                     * Register the left child whose INCOMPLETE_SPLIT flag was
                     * cleared.
                     */
                    XLogRegisterBuffer(1, cbuf, REGBUF_STANDARD);
                }
                xlinfo = XLOG_BTREE_INSERT_UPPER;
            }

            if (BufferIsValid(metabuf)) {
                xlmeta.root = metad->btm_root;
                xlmeta.level = metad->btm_level;
                xlmeta.fastroot = metad->btm_fastroot;
                xlmeta.fastlevel = metad->btm_fastlevel;

                if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
                    XLogRegisterBuffer(1, metabuf, REGBUF_WILL_INIT);
                    XLogRegisterBufData(1, (char *)&xlmeta, sizeof(xl_btree_metadata));
                } else {
                    XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
                    XLogRegisterBufData(2, (char *)&xlmeta, sizeof(xl_btree_metadata));
                }
                xlinfo = XLOG_BTREE_INSERT_META;
            }

            /* Read comments in _bt_pgaddtup */
            XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
            if (!P_ISLEAF(lpageop) && newitemoff == P_FIRSTDATAKEY(lpageop)) {
                trunctuple = *itup;
                trunctuple.t_info = sizeof(IndexTupleData);
                XLogRegisterBufData(0, (char *)&trunctuple, sizeof(IndexTupleData));
            } else
                XLogRegisterBufData(0, (char *)itup, IndexTupleDSize(*itup));

            if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
                recptr = XLogInsert(RM_BTREE_ID, xlinfo);
            } else {
                recptr = XLogInsert(RM_BTREE_ID, xlinfo | BTREE_SPLIT_UPGRADE_FLAG);
            }

            if (BufferIsValid(metabuf)) {
                PageSetLSN(metapg, recptr);
            }
            if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
                if (BufferIsValid(cbuf)) {
                    PageSetLSN(BufferGetPage(cbuf), recptr);
                }
            }
            PageSetLSN(page, recptr);
        }

        END_CRIT_SECTION();

        /* release buffers */
        if (BufferIsValid(metabuf)) {
            _bt_relbuf(rel, metabuf);
        }
        if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            if (BufferIsValid(cbuf)) {
                _bt_relbuf(rel, cbuf);
            }
        }
        _bt_relbuf(rel, buf);

        /*
         * If we decided to cache the insertion target block, then set it now.
         * But before that, check for the height of the tree and don't go for
         * the optimization for small indexes. We defer that check to this
         * point to ensure that we don't call _bt_getrootheight while holding
         * lock on any other block.
         *
         * We do this after dropping locks on all buffers. So the information
         * about whether the insertion block is still the rightmost block or
         * not may have changed in between. But we will deal with that during
         * next insert operation. No special care is required while setting it.
         */
        if (BlockNumberIsValid(cachedBlock) &&
            _bt_getrootheight(rel) >= BTREE_FASTPATH_MIN_LEVEL)
            RelationSetTargetBlock(rel, cachedBlock);
    }
}

/*
 *	_bt_split() -- split a page in the btree.
 *
 *		On entry, buf is the page to split, and is pinned and write-locked.
 *		firstright is the item index of the first item to be moved to the
 *		new right page.  newitemoff etc. tell us about the new item that
 *		must be inserted along with the data from the old page.
 *
 *		When splitting a non-leaf page, 'cbuf' is the left-sibling of the
 *		page we're inserting the downlink for.  This function will clear the
 *		INCOMPLETE_SPLIT flag on it, and release the buffer.
 *
 *		Returns the new right sibling of buf, pinned and write-locked.
 *		The pin and lock on buf are maintained.
 */
static Buffer _bt_split(Relation rel, Buffer buf, Buffer cbuf, OffsetNumber firstright, OffsetNumber newitemoff,
                        Size newitemsz, IndexTuple newitem, bool newitemonleft)
{
    Buffer rbuf;
    Page origpage;
    Page leftpage, rightpage;
    BlockNumber origpagenumber, rightpagenumber;
    BTPageOpaqueInternal ropaque, lopaque, oopaque;
    Buffer sbuf = InvalidBuffer;
    Page spage = NULL;
    BTPageOpaqueInternal sopaque = NULL;
    Size itemsz;
    ItemId itemid;
    IndexTuple item;
    OffsetNumber leftoff, rightoff;
    OffsetNumber maxoff;
    OffsetNumber i;
    bool isroot = false;
    bool isleaf = false;
    errno_t rc;
    IndexTuple lefthikey;
    int indnatts = IndexRelationGetNumberOfAttributes(rel);
    int indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

    /* Acquire a new page to split into */
    rbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);

    /*
     * origpage is the original page to be split.  leftpage is a temporary
     * buffer that receives the left-sibling data, which will be copied back
     * into origpage on success.  rightpage is the new page that receives the
     * right-sibling data.	If we fail before reaching the critical section,
     * origpage hasn't been modified and leftpage is only workspace. In
     * principle we shouldn't need to worry about rightpage either, because it
     * hasn't been linked into the btree page structure; but to avoid leaving
     * possibly-confusing junk behind, we are careful to rewrite rightpage as
     * zeroes before throwing any error.
     */
    origpage = BufferGetPage(buf);
    leftpage = PageGetTempPage(origpage);
    rightpage = BufferGetPage(rbuf);

    origpagenumber = BufferGetBlockNumber(buf);
    rightpagenumber = BufferGetBlockNumber(rbuf);

    _bt_pageinit(leftpage, BufferGetPageSize(buf));
    /* rightpage was already initialized by _bt_getbuf
     * Copy the original page's LSN into leftpage, which will become the
     * updated version of the page.  We need this because XLogInsert will
     * examine the LSN and possibly dump it in a page image.
     */
    PageSetLSN(leftpage, PageGetLSN(origpage));

    /* init btree private data */
    oopaque = (BTPageOpaqueInternal)PageGetSpecialPointer(origpage);
    lopaque = (BTPageOpaqueInternal)PageGetSpecialPointer(leftpage);
    ropaque = (BTPageOpaqueInternal)PageGetSpecialPointer(rightpage);

    isroot = P_ISROOT(oopaque) > 0 ? true : false;
    isleaf = P_ISLEAF(oopaque) > 0 ? true : false;

    /* if we're splitting this page, it won't be the root when we're done */
    /* also, clear the SPLIT_END and HAS_GARBAGE flags in both pages */
    lopaque->btpo_flags = oopaque->btpo_flags;
    lopaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
    ropaque->btpo_flags = lopaque->btpo_flags;
    /* set flag in left page indicating that the right page has no downlink */
    if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
        lopaque->btpo_flags |= BTP_INCOMPLETE_SPLIT;
    }
    lopaque->btpo_prev = oopaque->btpo_prev;
    lopaque->btpo_next = rightpagenumber;
    ropaque->btpo_prev = origpagenumber;
    ropaque->btpo_next = oopaque->btpo_next;
    lopaque->btpo.level = ropaque->btpo.level = oopaque->btpo.level;
    /* Since we already have write-lock on both pages, ok to read cycleid */
    lopaque->btpo_cycleid = _bt_vacuum_cycleid(rel);
    ropaque->btpo_cycleid = lopaque->btpo_cycleid;

    /*
     * If the page we're splitting is not the rightmost page at its level in
     * the tree, then the first entry on the page is the high key for the
     * page.  We need to copy that to the right half.  Otherwise (meaning the
     * rightmost page case), all the items on the right half will be user
     * data.
     */
    rightoff = P_HIKEY;

    if (!P_RIGHTMOST(oopaque)) {
        itemid = PageGetItemId(origpage, P_HIKEY);
        itemsz = ItemIdGetLength(itemid);
        item = (IndexTuple)PageGetItem(origpage, itemid);
        Assert(BTreeTupleGetNAtts(item, rel) == indnkeyatts);
        if (PageAddItem(rightpage, (Item)item, itemsz, rightoff, false, false) == InvalidOffsetNumber) {
            rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
            securec_check(rc, "", "");
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("failed to add hikey to the right sibling while splitting block %u of index \"%s\"",
                                   origpagenumber, RelationGetRelationName(rel))));
        }
        rightoff = OffsetNumberNext(rightoff);
    }

    /*
     * The "high key" for the new left page will be the first key that's going
     * to go into the new right page.  This might be either the existing data
     * item at position firstright, or the incoming tuple.
     */
    leftoff = P_HIKEY;
    if (!newitemonleft && newitemoff == firstright) {
        /* incoming tuple will become first on right page */
        itemsz = newitemsz;
        item = newitem;
    } else {
        /* existing item at firstright will become first on right page */
        itemid = PageGetItemId(origpage, firstright);
        itemsz = ItemIdGetLength(itemid);
        item = (IndexTuple)PageGetItem(origpage, itemid);
    }

    /*
     * We must truncate included attributes of the "high key" item, before
     * insert it onto the leaf page.  It's the only point in insertion
     * process, where we perform truncation.  All other functions work with
     * this high key and do not change it.
     */
    if (indnatts != indnkeyatts && isleaf) {
        lefthikey = _bt_nonkey_truncate(rel, item);
        itemsz = IndexTupleSize(lefthikey);
        itemsz = MAXALIGN(itemsz);
    } else {
        lefthikey = item;
    }

    if (PageAddItem(leftpage, (Item)lefthikey, itemsz, leftoff, false, false) == InvalidOffsetNumber) {
        rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
        securec_check(rc, "", "");
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("failed to add hikey to the left sibling while splitting block %u of index \"%s\"",
                               origpagenumber, RelationGetRelationName(rel))));
    }
    leftoff = OffsetNumberNext(leftoff);

    /* be tidy */
    if (lefthikey != item) {
        pfree(lefthikey);
    }

    /*
     * Now transfer all the data items to the appropriate page.
     *
     * Note: we *must* insert at least the right page's items in item-number
     * order, for the benefit of _bt_restore_page().
     */
    maxoff = PageGetMaxOffsetNumber(origpage);

    for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i)) {
        itemid = PageGetItemId(origpage, i);
        itemsz = ItemIdGetLength(itemid);
        item = (IndexTuple)PageGetItem(origpage, itemid);

        /* does new item belong before this one? */
        if (i == newitemoff) {
            if (newitemonleft) {
                if (!_bt_pgaddtup(leftpage, newitemsz, newitem, leftoff)) {
                    rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
                    securec_check(rc, "", "");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("failed to add new item to the left sibling while splitting block %u of index \"%s\"",
                                origpagenumber, RelationGetRelationName(rel))));
                }
                leftoff = OffsetNumberNext(leftoff);
            } else {
                if (!_bt_pgaddtup(rightpage, newitemsz, newitem, rightoff)) {
                    rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
                    securec_check(rc, "", "");
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("failed to add new item to the right sibling while splitting block %u of index \"%s\"",
                                origpagenumber, RelationGetRelationName(rel))));
                }
                rightoff = OffsetNumberNext(rightoff);
            }
        }

        /* decide which page to put it on */
        if (i < firstright) {
            if (!_bt_pgaddtup(leftpage, itemsz, item, leftoff)) {
                rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
                securec_check(rc, "", "");
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("failed to add old item to the left sibling while splitting block %u of index \"%s\"",
                                origpagenumber, RelationGetRelationName(rel))));
            }
            leftoff = OffsetNumberNext(leftoff);
        } else {
            if (!_bt_pgaddtup(rightpage, itemsz, item, rightoff)) {
                rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
                securec_check(rc, "", "");
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("failed to add old item to the right sibling while splitting block %u of index \"%s\"",
                                origpagenumber, RelationGetRelationName(rel))));
            }
            rightoff = OffsetNumberNext(rightoff);
        }
    }

    /* cope with possibility that newitem goes at the end */
    if (i <= newitemoff) {
        /*
         * Can't have newitemonleft here; that would imply we were told to put
         * *everything* on the left page, which cannot fit (if it could, we'd
         * not be splitting the page).
         */
        Assert(!newitemonleft);
        if (!_bt_pgaddtup(rightpage, newitemsz, newitem, rightoff)) {
            rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
            securec_check(rc, "", "");
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("failed to add new item to the right sibling while splitting block %u of index \"%s\"",
                            origpagenumber, RelationGetRelationName(rel))));
        }
        rightoff = OffsetNumberNext(rightoff);
    }

    /*
     * We have to grab the right sibling (if any) and fix the prev pointer
     * there. We are guaranteed that this is deadlock-free since no other
     * writer will be holding a lock on that page and trying to move left, and
     * all readers release locks on a page before trying to fetch its
     * neighbors.
     */
    if (!P_RIGHTMOST(oopaque)) {
        sbuf = _bt_getbuf(rel, oopaque->btpo_next, BT_WRITE);
        spage = BufferGetPage(sbuf);
        sopaque = (BTPageOpaqueInternal)PageGetSpecialPointer(spage);
        if (sopaque->btpo_prev != origpagenumber) {
            rc = memset_s(rightpage, BLCKSZ, 0, BufferGetPageSize(rbuf));
            securec_check(rc, "", "");

            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("right sibling's left-link doesn't match: block %u links to %u instead of expected %u in "
                            "index \"%s\"",
                            oopaque->btpo_next, sopaque->btpo_prev, origpagenumber, RelationGetRelationName(rel))));
        }

        /*
         * Check to see if we can set the SPLIT_END flag in the right-hand
         * split page; this can save some I/O for vacuum since it need not
         * proceed to the right sibling.  We can set the flag if the right
         * sibling has a different cycleid: that means it could not be part of
         * a group of pages that were all split off from the same ancestor
         * page.  If you're confused, imagine that page A splits to A B and
         * then again, yielding A C B, while vacuum is in progress.  Tuples
         * originally in A could now be in either B or C, hence vacuum must
         * examine both pages.	But if D, our right sibling, has a different
         * cycleid then it could not contain any tuples that were in A when
         * the vacuum started.
         */
        if (sopaque->btpo_cycleid != ropaque->btpo_cycleid)
            ropaque->btpo_flags |= BTP_SPLIT_END;
    }

    /*
     * Right sibling is locked, new siblings are prepared, but original page
     * is not updated yet.
     *
     * NO EREPORT(ERROR) till right sibling is updated.  We can get away with
     * not starting the critical section till here because we haven't been
     * scribbling on the original page yet; see comments above.
     */
    START_CRIT_SECTION();

    /*
     * By here, the original data page has been split into two new halves, and
     * these are correct.  The algorithm requires that the left page never
     * move during a split, so we copy the new left page back on top of the
     * original.  Note that this is not a waste of time, since we also require
     * (in the page management code) that the center of a page always be
     * clean, and the most efficient way to guarantee this is just to compact
     * the data by reinserting it into a new left page.  (XXX the latter
     * comment is probably obsolete; but in any case it's good to not scribble
     * on the original page until we enter the critical section.)
     *
     * We need to do this before writing the WAL record, so that XLogInsert
     * can WAL log an image of the page if necessary.
     */
    PageRestoreTempPage(leftpage, origpage);
    /* leftpage, lopaque must not be used below here */
    MarkBufferDirty(buf);
    MarkBufferDirty(rbuf);

    if (!P_RIGHTMOST(ropaque)) {
        sopaque->btpo_prev = rightpagenumber;
        MarkBufferDirty(sbuf);
    }

    /*
     * Clear INCOMPLETE_SPLIT flag on child if inserting the new item finishes
     * a split.
     */
    if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
        if (!isleaf) {
            Page cpage = BufferGetPage(cbuf);
            BTPageOpaqueInternal cpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(cpage);

            cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
            MarkBufferDirty(cbuf);
        }
    }

    /* XLOG stuff */
    if (RelationNeedsWAL(rel)) {
        xl_btree_split xlrec;
        uint8 xlinfo;
        XLogRecPtr recptr;

        xlrec.level = ropaque->btpo.level;
        xlrec.firstright = firstright;
        xlrec.newitemoff = newitemoff;

        XLogBeginInsert();
        XLogRegisterData((char *)&xlrec, SizeOfBtreeSplit);
        XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
        XLogRegisterBuffer(1, rbuf, REGBUF_WILL_INIT);
        /* Log the right sibling, because we've changed its' prev-pointer. */
        if (!P_RIGHTMOST(ropaque)) {
            XLogRegisterBuffer(2, sbuf, REGBUF_STANDARD);
        }

        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            if (ropaque->btpo.level > 0) {
                /* Log downlink on non-leaf pages */
                XLogRegisterData((char *)&newitem->t_tid.ip_blkid, sizeof(BlockIdData));

                /*
                 * We must also log the left page's high key, because the right
                 * page's leftmost key is suppressed on non-leaf levels.  Show it
                 * as belonging to the left page buffer, so that it is not stored
                 * if XLogInsert decides it needs a full-page image of the left
                 * page.
                 */
                itemid = PageGetItemId(origpage, P_HIKEY);
                item = (IndexTuple)PageGetItem(origpage, itemid);
                XLogRegisterBufData(0, (char *)item, MAXALIGN(IndexTupleSize(item)));
            }
        } else {
            if (BufferIsValid(cbuf)) {
                XLogRegisterBuffer(3, cbuf, REGBUF_STANDARD);
            }
        }

        /*
         * Log the new item, if it was inserted on the left page. (If it was
         * put on the right page, we don't need to explicitly WAL log it
         * because it's included with all the other items on the right page.)
         * Show the new item as belonging to the left page buffer, so that it
         * is not stored if XLogInsert decides it needs a full-page image of
         * the left page.  We store the offset anyway, though, to support
         * archive compression of these records.
         */
        if (newitemonleft)
            XLogRegisterBufData(0, (char *)newitem, MAXALIGN(newitemsz));

        if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            /* Log the left page's new high key */
            itemid = PageGetItemId(origpage, P_HIKEY);
            item = (IndexTuple)PageGetItem(origpage, itemid);
            XLogRegisterBufData(0, (char *)item, MAXALIGN(IndexTupleSize(item)));
        }

        /*
         * Log the contents of the right page in the format understood by
         * _bt_restore_page(). We set lastrdata->buffer to InvalidBuffer,
         * because we're going to recreate the whole page anyway, so it should
         * never be stored by XLogInsert.
         *
         * Direct access to page is not good but faster - we should implement
         * some new func in page API.  Note we only store the tuples
         * themselves, knowing that they were inserted in item-number order
         * and so the item pointers can be reconstructed.
         * See comments for _bt_restore_page().
         */
        XLogRegisterBufData(1, (char *)rightpage + ((PageHeader)rightpage)->pd_upper,
                            ((PageHeader)rightpage)->pd_special - ((PageHeader)rightpage)->pd_upper);

        if (isroot) {
            xlinfo = newitemonleft ? XLOG_BTREE_SPLIT_L_ROOT : XLOG_BTREE_SPLIT_R_ROOT;
        } else {
            xlinfo = newitemonleft ? XLOG_BTREE_SPLIT_L : XLOG_BTREE_SPLIT_R;
        }
        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            recptr = XLogInsert(RM_BTREE_ID, xlinfo);
        } else {
            recptr = XLogInsert(RM_BTREE_ID, xlinfo | BTREE_SPLIT_UPGRADE_FLAG);
        }

        PageSetLSN(origpage, recptr);
        PageSetLSN(rightpage, recptr);
        if (!P_RIGHTMOST(ropaque)) {
            PageSetLSN(spage, recptr);
        }
        if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            if (!isleaf) {
                PageSetLSN(BufferGetPage(cbuf), recptr);
            }
        }
    }

    END_CRIT_SECTION();

    /* release the old right sibling */
    if (!P_RIGHTMOST(ropaque)) {
        _bt_relbuf(rel, sbuf);
    }
    if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
        /* release the child */
        if (!isleaf) {
            _bt_relbuf(rel, cbuf);
        }
    }

    /* split's done */
    return rbuf;
}

/*
 * Find an appropriate place to split a page.
 *
 * The idea here is to equalize the free space that will be on each split
 * page, *after accounting for the inserted tuple*.  (If we fail to account
 * for it, we might find ourselves with too little room on the page that
 * it needs to go into!)
 *
 * If the page is the rightmost page on its level, we instead try to arrange
 * to leave the left split page fillfactor% full.  In this way, when we are
 * inserting successively increasing keys (consider sequences, timestamps,
 * etc) we will end up with a tree whose pages are about fillfactor% full,
 * instead of the 50% full result that we'd get without this special case.
 * This is the same as nbtsort.c produces for a newly-created tree.  Note
 * that leaf and nonleaf pages use different fillfactors.
 *
 * We are passed the intended insert position of the new tuple, expressed as
 * the offsetnumber of the tuple it must go in front of.  (This could be
 * maxoff+1 if the tuple is to go at the end.)
 *
 * We return the index of the first existing tuple that should go on the
 * righthand page, plus a boolean indicating whether the new tuple goes on
 * the left or right page. The bool is necessary to disambiguate the case
 * where firstright == newitemoff.
 */
static OffsetNumber _bt_findsplitloc(Relation rel, Page page, OffsetNumber newitemoff, Size newitemsz,
                                     bool *newitemonleft)
{
    BTPageOpaqueInternal opaque;
    OffsetNumber offnum;
    OffsetNumber maxoff;
    ItemId itemid;
    FindSplitData state;
    int leftspace, rightspace, goodenough, olddataitemstotal, olddataitemstoleft;
    bool goodenoughfound = false;

    opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);

    /* Passed-in newitemsz is MAXALIGNED but does not include line pointer */
    newitemsz += sizeof(ItemIdData);

    /* Total free space available on a btree page, after fixed overhead */
    leftspace = rightspace = PageGetPageSize(page) - SizeOfPageHeaderData - MAXALIGN(sizeof(BTPageOpaqueData));

    /* The right page will have the same high key as the old page */
    if (!P_RIGHTMOST(opaque)) {
        itemid = PageGetItemId(page, P_HIKEY);
        rightspace -= (int)(MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData));
    }

    /* Count up total space in data items without actually scanning 'em */
    olddataitemstotal = rightspace - (int)PageGetExactFreeSpace(page);

    state.newitemsz = newitemsz;
    state.is_leaf = P_ISLEAF(opaque) > 0 ? true : false;
    state.is_rightmost = P_RIGHTMOST(opaque);
    state.have_split = false;
    if (state.is_leaf) {
        state.fillfactor = RelationGetFillFactor(rel, BTREE_DEFAULT_FILLFACTOR);
    } else {
        state.fillfactor = BTREE_NONLEAF_FILLFACTOR;
    }
    state.newitemonleft = false; /* these just to keep compiler quiet */
    state.firstright = 0;
    state.best_delta = 0;
    state.leftspace = leftspace;
    state.rightspace = rightspace;
    state.olddataitemstotal = olddataitemstotal;
    state.newitemoff = newitemoff;

    /*
     * Finding the best possible split would require checking all the possible
     * split points, because of the high-key and left-key special cases.
     * That's probably more work than it's worth; instead, stop as soon as we
     * find a "good-enough" split, where good-enough is defined as an
     * imbalance in free space of no more than pagesize/16 (arbitrary...) This
     * should let us stop near the middle on most pages, instead of plowing to
     * the end.
     */
    goodenough = leftspace / 16;

    /*
     * Scan through the data items and calculate space usage for a split at
     * each possible position.
     */
    olddataitemstoleft = 0;
    goodenoughfound = false;
    maxoff = PageGetMaxOffsetNumber(page);

    for (offnum = P_FIRSTDATAKEY(opaque); offnum <= maxoff; offnum = OffsetNumberNext(offnum)) {
        Size itemsz;

        itemid = PageGetItemId(page, offnum);
        itemsz = MAXALIGN(ItemIdGetLength(itemid)) + sizeof(ItemIdData);

        /*
         * Will the new item go to left or right of split?
         */
        if (offnum > newitemoff) {
            _bt_checksplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
        } else if (offnum < newitemoff) {
            _bt_checksplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
        } else {
            /* need to try it both ways! */
            _bt_checksplitloc(&state, offnum, true, olddataitemstoleft, itemsz);
            _bt_checksplitloc(&state, offnum, false, olddataitemstoleft, itemsz);
        }

        /* Abort scan once we find a good-enough choice */
        if (state.have_split && state.best_delta <= goodenough) {
            goodenoughfound = true;
            break;
        }

        olddataitemstoleft += itemsz;
    }

    /*
     * If the new item goes as the last item, check for splitting so that all
     * the old items go to the left page and the new item goes to the right
     * page.
     */
    if (newitemoff > maxoff && !goodenoughfound) {
        _bt_checksplitloc(&state, newitemoff, false, olddataitemstotal, 0);
    }

    /*
     * I believe it is not possible to fail to find a feasible split, but just
     * in case ...
     */
    if (!state.have_split) {
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("could not find a feasible split point for index \"%s\"", RelationGetRelationName(rel))));
    }

    *newitemonleft = state.newitemonleft;
    return state.firstright;
}

/*
 * Subroutine to analyze a particular possible split choice (ie, firstright
 * and newitemonleft settings), and record the best split so far in *state.
 *
 * firstoldonright is the offset of the first item on the original page
 * that goes to the right page, and firstoldonrightsz is the size of that
 * tuple. firstoldonright can be > max offset, which means that all the old
 * items go to the left page and only the new item goes to the right page.
 * In that case, firstoldonrightsz is not used.
 *
 * olddataitemstoleft is the total size of all old items to the left of
 * firstoldonright.
 */
static void _bt_checksplitloc(FindSplitData *state, OffsetNumber firstoldonright, bool newitemonleft,
                              int olddataitemstoleft, Size firstoldonrightsz)
{
    int leftfree, rightfree;
    Size firstrightitemsz;
    bool newitemisfirstonright = false;

    /* Is the new item going to be the first item on the right page? */
    newitemisfirstonright = (firstoldonright == state->newitemoff && !newitemonleft);
    if (newitemisfirstonright) {
        firstrightitemsz = state->newitemsz;
    } else {
        firstrightitemsz = firstoldonrightsz;
    }

    /* Account for all the old tuples */
    leftfree = state->leftspace - olddataitemstoleft;
    rightfree = state->rightspace - (state->olddataitemstotal - olddataitemstoleft);

    /*
     * The first item on the right page becomes the high key of the left page;
     * therefore it counts against left space as well as right space. When
     * index has included attribues, then those attributes of left page high
     * key will be truncate leaving that page with slightly more free space.
     * However, that shouldn't affect our ability to find valid split
     * location, because anyway split location should exists even without high
     * key truncation.
     */
    leftfree -= firstrightitemsz;

    /* account for the new item */
    if (newitemonleft) {
        leftfree -= (int)state->newitemsz;
    } else {
        rightfree -= (int)state->newitemsz;
    }

    /*
     * If we are not on the leaf level, we will be able to discard the key
     * data from the first item that winds up on the right page.
     */
    if (!state->is_leaf) {
        rightfree += (int)firstrightitemsz - (int)(MAXALIGN(sizeof(IndexTupleData)) + sizeof(ItemIdData));
    }

    /*
     * If feasible split point, remember best delta.
     */
    if (leftfree >= 0 && rightfree >= 0) {
        int delta;

        if (state->is_rightmost) {
            /*
             * If splitting a rightmost page, try to put (100-fillfactor)% of
             * free space on left page. See comments for _bt_findsplitloc.
             */
            delta = (state->fillfactor * leftfree) - ((100 - state->fillfactor) * rightfree);
        } else {
            /* Otherwise, aim for equal free space on both sides */
            delta = leftfree - rightfree;
        }

        if (delta < 0) {
            delta = -delta;
        }
        if (!state->have_split || delta < state->best_delta) {
            state->have_split = true;
            state->newitemonleft = newitemonleft;
            state->firstright = firstoldonright;
            state->best_delta = delta;
        }
    }
}

/*
 * _bt_insert_parent() -- Insert downlink into parent after a page split.
 *
 * On entry, buf and rbuf are the left and right split pages, which we
 * still hold write locks on per the L&Y algorithm.  We release the
 * write locks once we have write lock on the parent page. (Any sooner,
 * and it'd be possible for some other process to try to split or delete
 * one of these pages, and get confused because it cannot find the downlink.)
 *
 * stack - stack showing how we got here.  May be NULL in cases that don't
 *         have to be efficient (concurrent ROOT split, WAL recovery)
 * is_root - we split the true root
 * is_only - we split a page alone on its level (might have been fast root)
 */
void _bt_insert_parent(Relation rel, Buffer buf, Buffer rbuf, BTStack stack, bool is_root, bool is_only)
{
    /*
     * Here we have to do something Lehman and Yao don't talk about: deal with
     * a root split and construction of a new root.  If our stack is empty
     * then we have just split a node on what had been the root level when we
     * descended the tree. If it was still the root then we perform a
     * new-root construction.  If it *wasn't* the root anymore, search to find
     * the next higher level that someone constructed meanwhile, and find the
     * right place to insert as for the normal case.
     *
     * If we have to search for the parent level, we do so by re-descending
     * from the root.  This is not super-efficient, but it's rare enough not
     * to matter.  (This path is also taken when called from WAL recovery ---
     * we have no stack in that case.)
     */
    if (is_root) {
        Buffer rootbuf;

        Assert(stack == NULL);
        Assert(is_only);
        /* create a new root node and update the metapage */
        rootbuf = _bt_newroot(rel, buf, rbuf);
        /* release the split buffers */
        _bt_relbuf(rel, rootbuf);
        _bt_relbuf(rel, rbuf);
        _bt_relbuf(rel, buf);
    } else {
        BlockNumber bknum = BufferGetBlockNumber(buf);
        BlockNumber rbknum = BufferGetBlockNumber(rbuf);
        Page page = BufferGetPage(buf);
        IndexTuple new_item;
        BTStackData fakestack;
        IndexTuple ritem;
        Buffer pbuf;

        if (stack == NULL) {
            BTPageOpaqueInternal lpageop;

            if (!t_thrd.xlog_cxt.InRecovery) {
                ereport(DEBUG2, (errmsg("concurrent ROOT page split")));
            }
            lpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
            /* Find the leftmost page at the next level up */
            pbuf = _bt_get_endpoint(rel, lpageop->btpo.level + 1, false);
            /* Set up a phony stack entry pointing there */
            stack = &fakestack;
            stack->bts_blkno = BufferGetBlockNumber(pbuf);
            stack->bts_offset = InvalidOffsetNumber;
            stack->bts_btentry = InvalidBlockNumber;
            stack->bts_parent = NULL;
            _bt_relbuf(rel, pbuf);
        }

        /* get high key from left page == lower bound for new right page */
        ritem = (IndexTuple)PageGetItem(page, PageGetItemId(page, P_HIKEY));

        /* form an index tuple that points at the new right page
         * assure that memory is properly allocated, prevent from missing log of insert parent */
        START_CRIT_SECTION();
        new_item = CopyIndexTuple(ritem);
        if (t_thrd.proc->workingVersionNum < SUPPORT_GPI_VERSION_NUM) {
            ItemPointerSet(&(new_item->t_tid), rbknum, P_HIKEY);
        } else {
            BTreeInnerTupleSetDownLink(new_item, rbknum);
        }
        END_CRIT_SECTION();

        /*
         * Find the parent buffer and get the parent page.
         *
         * Oops - if we were moved right then we need to change stack item! We
         * want to find parent pointing to where we are, right ? - vadim
         * 05/27/97
         */
        stack->bts_btentry = bknum;
        pbuf = _bt_getstackbuf(rel, stack);

        /* Now we can unlock the right child. The left child will be unlocked
         * by _bt_insertonpg()
         */
        _bt_relbuf(rel, rbuf);
        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            _bt_relbuf(rel, buf);
        }

        /* Check for error only after writing children */
        if (pbuf == InvalidBuffer) {
            XLogWaitFlush(t_thrd.xlog_cxt.LogwrtResult->Write);
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("failed to re-find parent key in index \"%s\" for split pages %u/%u",
                                   RelationGetRelationName(rel), bknum, rbknum)));
        }
        /* Recursively update the parent */
        _bt_insertonpg(rel, pbuf, buf, stack->bts_parent, new_item, stack->bts_offset + 1, is_only, false);

        /* be tidy */
        pfree(new_item);
    }
}

/*
 * _bt_finish_split() -- Finish an incomplete split
 *
 * A crash or other failure can leave a split incomplete.  The insertion
 * routines won't allow to insert on a page that is incompletely split.
 * Before inserting on such a page, call _bt_finish_split().
 *
 * On entry, 'lbuf' must be locked in write-mode. On exit, it is unlocked
 * and unpinned.
 */
void _bt_finish_split(Relation rel, Buffer lbuf, BTStack stack)
{
    Page lpage = BufferGetPage(lbuf);
    BTPageOpaqueInternal lpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(lpage);
    Buffer rbuf;
    Page rpage;
    BTPageOpaqueInternal rpageop;
    bool was_root = false;

    Assert(P_INCOMPLETE_SPLIT(lpageop));

    /* Lock right sibling, the one missing the downlink */
    rbuf = _bt_getbuf(rel, lpageop->btpo_next, BT_WRITE);
    rpage = BufferGetPage(rbuf);
    rpageop = (BTPageOpaqueInternal)PageGetSpecialPointer(rpage);

    /* Could this be a root split? */
    if (!stack) {
        Buffer metabuf;
        Page metapg;
        BTMetaPageData *metad;

        /* acquire lock on the metapage */
        metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
        metapg = BufferGetPage(metabuf);
        metad = BTPageGetMeta(metapg);

        was_root = (metad->btm_root == BufferGetBlockNumber(lbuf));

        _bt_relbuf(rel, metabuf);
    } else {
        was_root = false;
    }

    /* Was this the only page on the level before split? */
    bool was_only = (P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop));

    elog(DEBUG1, "finishing incomplete split of %u/%u", BufferGetBlockNumber(lbuf), BufferGetBlockNumber(rbuf));

    _bt_insert_parent(rel, lbuf, rbuf, stack, was_root, was_only);
}

/*
 *	_bt_getstackbuf() -- Walk back up the tree one step, and find the item
 *						 we last looked at in the parent.
 *
 *		This is possible because we save the downlink from the parent item,
 *		which is enough to uniquely identify it.  Insertions into the parent
 *		level could cause the item to move right; deletions could cause it
 *		to move left, but not left of the page we previously found it in.
 *
 *		Adjusts bts_blkno & bts_offset if changed.
 *
 *		Returns InvalidBuffer if item not found (should not happen).
 */
Buffer _bt_getstackbuf(Relation rel, BTStack stack)
{
    BlockNumber blkno;
    OffsetNumber start;

    blkno = stack->bts_blkno;
    start = stack->bts_offset;
    t_thrd.storage_cxt.is_btree_split = true;
    for (;;) {
        Buffer buf;
        Page page;
        BTPageOpaqueInternal opaque;

        buf = _bt_getbuf(rel, blkno, BT_WRITE);
        page = BufferGetPage(buf);
        opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
        if (P_INCOMPLETE_SPLIT(opaque)) {
            _bt_finish_split(rel, buf, stack->bts_parent);
            continue;
        }

        if (!P_IGNORE(opaque)) {
            OffsetNumber offnum, minoff, maxoff;
            ItemId itemid;
            IndexTuple item;

            minoff = P_FIRSTDATAKEY(opaque);
            maxoff = PageGetMaxOffsetNumber(page);

            /*
             * start = InvalidOffsetNumber means "search the whole page". We
             * need this test anyway due to possibility that page has a high
             * key now when it didn't before.
             */
            if (start < minoff)
                start = minoff;

            /*
             * Need this check too, to guard against possibility that page
             * split since we visited it originally.
             */
            if (start > maxoff)
                start = OffsetNumberNext(maxoff);

            /*
             * These loops will check every item on the page --- but in an
             * order that's attuned to the probability of where it actually
             * is.	Scan to the right first, then to the left.
             */
            for (offnum = start; offnum <= maxoff; offnum = OffsetNumberNext(offnum)) {
                itemid = PageGetItemId(page, offnum);
                item = (IndexTuple)PageGetItem(page, itemid);
                bool getRightItem = false;
                getRightItem = BTreeInnerTupleGetDownLink(item) == stack->bts_btentry;
                if (getRightItem) {
                    /* Return accurate pointer to where link is now */
                    stack->bts_blkno = blkno;
                    stack->bts_offset = offnum;
                    t_thrd.storage_cxt.is_btree_split = false;
                    return buf;
                }
            }

            for (offnum = OffsetNumberPrev(start); offnum >= minoff; offnum = OffsetNumberPrev(offnum)) {
                itemid = PageGetItemId(page, offnum);
                item = (IndexTuple)PageGetItem(page, itemid);
                bool getRightItem = false;
                getRightItem = BTreeInnerTupleGetDownLink(item) == stack->bts_btentry;
                if (getRightItem) {
                    /* Return accurate pointer to where link is now */
                    stack->bts_blkno = blkno;
                    stack->bts_offset = offnum;
                    t_thrd.storage_cxt.is_btree_split = false;
                    return buf;
                }
            }
        }

        /*
         * The item we're looking for moved right at least one page.
         */
        if (P_RIGHTMOST(opaque)) {
            _bt_relbuf(rel, buf);
            t_thrd.storage_cxt.is_btree_split = false;
            return InvalidBuffer;
        }
        blkno = opaque->btpo_next;
        start = InvalidOffsetNumber;
        _bt_relbuf(rel, buf);
    }
}

/*
 *	_bt_newroot() -- Create a new root page for the index.
 *
 *		We've just split the old root page and need to create a new one.
 *		In order to do this, we add a new root page to the file, then lock
 *		the metadata page and update it.  This is guaranteed to be deadlock-
 *		free, because all readers release their locks on the metadata page
 *		before trying to lock the root, and all writers lock the root before
 *		trying to lock the metadata page.  We have a write lock on the old
 *		root page, so we have not introduced any cycles into the waits-for
 *		graph.
 *
 *		On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *		locked. On exit, a new root page exists with entries for the
 *		two new children, metapage is updated and unlocked/unpinned.
 *		The new root buffer is returned to caller which has to unlock/unpin
 *		lbuf, rbuf & rootbuf.
 */
static Buffer _bt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
    Buffer rootbuf;
    Page lpage, rootpage;
    BlockNumber lbkno, rbkno;
    BlockNumber rootblknum;
    BTPageOpaqueInternal rootopaque;
    BTPageOpaqueInternal lopaque;
    ItemId itemid;
    IndexTuple item;
    IndexTuple left_item;
    Size left_item_sz;
    IndexTuple right_item;
    Size right_item_sz;
    Buffer metabuf;
    Page metapg;
    BTMetaPageData *metad = NULL;

    lbkno = BufferGetBlockNumber(lbuf);
    rbkno = BufferGetBlockNumber(rbuf);
    lpage = BufferGetPage(lbuf);
    lopaque = (BTPageOpaqueInternal)PageGetSpecialPointer(lpage);

    /*
     *	We have split the old root page and need to create a new one.
     *	Here we can not have any ERROR before the new root is created,
     *	and the metapage is update.
     *
     *	If we meet the ERROR here, the old root has been split into two
     *	pages, and the new root is failed to create. In this situation, the B+
     *	tree is without the root page, and the metadata is wrong.
     *
     *	1. root 1 splits to 1 and 2, block 1's root flag has changed.
     *	2. when we get in this function and haven't updated metapage's
     *	 rootpointer. ERROR here and we release our LWlock on 1 and 2.
     *	3. we continue insert into 1 and block 1 splits again. we use outdated
     *	 metapage to find 'trueroot' . deaklock.
     *
     *	Don't worry about incomplete btree pages as a result of PANIC here,
     *	because we can complete this incomplete action in btree_xlog_cleanup
     *	after redo our xlog.
     */
    START_CRIT_SECTION();

    /* get a new root page */
    rootbuf = _bt_getbuf(rel, P_NEW, BT_WRITE);
    rootpage = BufferGetPage(rootbuf);
    rootblknum = BufferGetBlockNumber(rootbuf);

    /* acquire lock on the metapage */
    metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
    metapg = BufferGetPage(metabuf);
    metad = BTPageGetMeta(metapg);

    /*
     * Create downlink item for left page (old root).  Since this will be the
     * first item in a non-leaf page, it implicitly has minus-infinity key
     * value, so we need not store any actual key in it.
     */
    left_item_sz = sizeof(IndexTupleData);
    left_item = (IndexTuple)palloc(left_item_sz);
    left_item->t_info = (unsigned short)left_item_sz;
    
    if (t_thrd.proc->workingVersionNum < SUPPORT_GPI_VERSION_NUM) {
        ItemPointerSet(&(left_item->t_tid), lbkno, P_HIKEY);
    } else {
        BTreeInnerTupleSetDownLink(left_item, lbkno);
        BTreeTupleSetNAtts(left_item, 0);
    }

    /*
     * Create downlink item for right page.  The key for it is obtained from
     * the "high key" position in the left page.
     */
    itemid = PageGetItemId(lpage, P_HIKEY);
    right_item_sz = ItemIdGetLength(itemid);
    item = (IndexTuple)PageGetItem(lpage, itemid);
    right_item = CopyIndexTuple(item);
    
    if (t_thrd.proc->workingVersionNum < SUPPORT_GPI_VERSION_NUM) {
        ItemPointerSet(&(right_item->t_tid), rbkno, P_HIKEY);
    } else {
        BTreeInnerTupleSetDownLink(right_item, rbkno);
    }
    /* set btree special data */
    rootopaque = (BTPageOpaqueInternal)PageGetSpecialPointer(rootpage);
    rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
    rootopaque->btpo_flags = BTP_ROOT;
    rootopaque->btpo.level = ((BTPageOpaqueInternal)PageGetSpecialPointer(lpage))->btpo.level + 1;
    rootopaque->btpo_cycleid = 0;

    /* update metapage data */
    metad->btm_root = rootblknum;
    metad->btm_level = rootopaque->btpo.level;
    metad->btm_fastroot = rootblknum;
    metad->btm_fastlevel = rootopaque->btpo.level;

    /*
     * Insert the left page pointer into the new root page.  The root page is
     * the rightmost page on its level so there is no "high key" in it; the
     * two items will go into positions P_HIKEY and P_FIRSTKEY.
     *
     * Note: we *must* insert the two items in item-number order, for the
     * benefit of _bt_restore_page().
     */
    if (PageAddItem(rootpage, (Item)left_item, left_item_sz, P_HIKEY, false, false) == InvalidOffsetNumber)
        ereport(PANIC, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("failed to add leftkey to new root page while splitting block %u of index \"%s\"",
                               BufferGetBlockNumber(lbuf), RelationGetRelationName(rel))));

    /*
     * insert the right page pointer into the new root page.
     */
    if (PageAddItem(rootpage, (Item)right_item, right_item_sz, P_FIRSTKEY, false, false) == InvalidOffsetNumber)
        ereport(PANIC, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("failed to add rightkey to new root page while splitting block %u of index \"%s\"",
                               BufferGetBlockNumber(lbuf), RelationGetRelationName(rel))));

    if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
        /* Clear the incomplete-split flag in the left child */
        Assert(P_INCOMPLETE_SPLIT(lopaque));
        lopaque->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
        MarkBufferDirty(lbuf);
    }
    MarkBufferDirty(rootbuf);
    MarkBufferDirty(metabuf);

    /* XLOG stuff */
    if (RelationNeedsWAL(rel)) {
        xl_btree_newroot xlrec;
        XLogRecPtr recptr;
        xl_btree_metadata md;

        xlrec.rootblk = rootblknum;
        xlrec.level = metad->btm_level;

        XLogBeginInsert();
        XLogRegisterData((char *)&xlrec, SizeOfBtreeNewroot);

        XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            XLogRegisterBuffer(1, metabuf, REGBUF_WILL_INIT);
        } else {
            XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
            XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
        }

        md.root = rootblknum;
        md.level = metad->btm_level;
        md.fastroot = rootblknum;
        md.fastlevel = metad->btm_level;

        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            XLogRegisterBufData(1, (char *)&md, sizeof(xl_btree_metadata));
        } else {
            XLogRegisterBufData(2, (char *)&md, sizeof(xl_btree_metadata));
        }

        /*
         * Direct access to page is not good but faster - we should implement
         * some new func in page API.
         */
        XLogRegisterBufData(0, (char *)rootpage + ((PageHeader)rootpage)->pd_upper,
                            ((PageHeader)rootpage)->pd_special - ((PageHeader)rootpage)->pd_upper);

        if (t_thrd.proc->workingVersionNum < BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT);
        } else {
            recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT | BTREE_SPLIT_UPGRADE_FLAG);
        }

        if (t_thrd.proc->workingVersionNum >= BTREE_SPLIT_DELETE_UPGRADE_VERSION) {
            PageSetLSN(lpage, recptr);
        }
        PageSetLSN(rootpage, recptr);
        PageSetLSN(metapg, recptr);
    }

    END_CRIT_SECTION();

    /* done with metapage */
    _bt_relbuf(rel, metabuf);

    pfree(left_item);
    pfree(right_item);

    return rootbuf;
}

/*
 *	_bt_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested.  It does
 *		not affect pin/lock status, but you'd better have a write lock
 *		and pin on the target buffer!  Don't forget to write and release
 *		the buffer afterwards, either.
 *
 *		The main difference between this routine and a bare PageAddItem call
 *		is that this code knows that the leftmost index tuple on a non-leaf
 *		btree page doesn't need to have a key.  Therefore, it strips such
 *		tuples down to just the tuple header.  CAUTION: this works ONLY if
 *		we insert the tuples in order, so that the given itup_off does
 *		represent the final position of the tuple!
 */
static bool _bt_pgaddtup(Page page, Size itemsize, IndexTuple itup, OffsetNumber itup_off)
{
    BTPageOpaqueInternal opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);
    IndexTupleData trunctuple;

    if (!P_ISLEAF(opaque) && itup_off == P_FIRSTDATAKEY(opaque)) {
        trunctuple = *itup;
        trunctuple.t_info = sizeof(IndexTupleData);
        if (t_thrd.proc->workingVersionNum >= SUPPORT_GPI_VERSION_NUM) {
            BTreeTupleSetNAtts(&trunctuple, 0);
        }
        itup = &trunctuple;
        itemsize = sizeof(IndexTupleData);
    }

    if (PageAddItem(page, (Item)itup, itemsize, itup_off, false, false) == InvalidOffsetNumber)
        return false;

    return true;
}

/*
 * _bt_isequal - used in _bt_doinsert in check for duplicates.
 *
 * This is very similar to _bt_compare, except for NULL handling.
 * Rule is simple: NOT_NULL not equal NULL, NULL not equal NULL too.
 */
static bool _bt_isequal(Relation idxrel, Page page, OffsetNumber offnum, int keysz, ScanKey scankey)
{
    TupleDesc itupdesc = RelationGetDescr(idxrel);
    IndexTuple itup;
    int i;

    /* Better be comparing to a leaf item */
    Assert(P_ISLEAF((BTPageOpaqueInternal)PageGetSpecialPointer(page)));

    itup = (IndexTuple)PageGetItem(page, PageGetItemId(page, offnum));
    /*
     * Index tuple shouldn't be truncated.	Despite we technically could
     * compare truncated tuple as well, this function should be only called
     * for regular non-truncated leaf tuples and P_HIKEY tuple on
     * rightmost leaf page.
     */
    for (i = 1; i <= keysz; i++) {
        AttrNumber attno;
        Datum datum;
        bool isNull = false;
        int32 result;

        attno = scankey->sk_attno;
        Assert(attno == i);
        datum = index_getattr(itup, attno, itupdesc, &isNull);

        /* NULLs are never equal to anything */
        if (isNull || (scankey->sk_flags & SK_ISNULL))
            return false;

        result =
            DatumGetInt32(FunctionCall2Coll(&scankey->sk_func, scankey->sk_collation, datum, scankey->sk_argument));
        if (result != 0)
            return false;
        scankey++;
    }

    /* if we get here, the keys are equal */
    return true;
}

/*
 * _bt_vacuum_one_page - vacuum just one index page.
 *
 * Try to remove LP_DEAD items from the given page.  The passed buffer
 * must be exclusive-locked, but unlike a real VACUUM, we don't need a
 * super-exclusive "cleanup" lock (see nbtree/README).
 */
static void _bt_vacuum_one_page(Relation rel, Buffer buffer, Relation heapRel)
{
    OffsetNumber deletable[MaxOffsetNumber];
    int ndeletable = 0;
    OffsetNumber offnum, minoff, maxoff;
    Page page = BufferGetPage(buffer);
    BTPageOpaqueInternal opaque = (BTPageOpaqueInternal)PageGetSpecialPointer(page);

    /*
     * Scan over all items to see which ones need to be deleted according to
     * LP_DEAD flags.
     */
    minoff = P_FIRSTDATAKEY(opaque);
    maxoff = PageGetMaxOffsetNumber(page);
    for (offnum = minoff; offnum <= maxoff; offnum = OffsetNumberNext(offnum)) {
        ItemId itemId = PageGetItemId(page, offnum);
        if (ItemIdIsDead(itemId))
            deletable[ndeletable++] = offnum;
    }
    if (ndeletable > 0)
        _bt_delitems_delete(rel, buffer, deletable, ndeletable, heapRel);
    /*
     * Note: if we didn't find any LP_DEAD items, then the page's
     * BTP_HAS_GARBAGE hint bit is falsely set.  We do not bother expending a
     * separate write to clear it, however.  We will clear it when we split
     * the page.
     */
}

static bool CheckItemIsAlive(ItemPointer tid, Relation relation, Snapshot snapshot,
                             bool* all_dead, CUDescScan* cudescScan)
{
    if (!RelationIsCUFormat(relation)) {
        return heap_hot_search(tid, relation, snapshot, all_dead);
    } else {
        return cudescScan->CheckItemIsAlive(tid);
    }
}

bool CheckPartitionIsInvisible(GPIScanDesc gpiScan)
{
    if (OidRBTreeMemberOid(gpiScan->invisiblePartTreeForVacuum, gpiScan->currPartOid)) {
        return true;
    }

    PartStatus currStatus = PartitionGetMetadataStatus(gpiScan->currPartOid, true);

    if (currStatus == PART_METADATA_INVISIBLE) {
        (void)OidRBTreeInsertOid(gpiScan->invisiblePartTreeForVacuum, gpiScan->currPartOid);
        return true;
    }

    return false;
}
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
 * ss_dms_recovery.h
 * 
 * IDENTIFICATION
 *        src/include/ddes/dms/ss_dms_recovery.h
 *
 * ---------------------------------------------------------------------------------------
 */   
#ifndef SS_DMS_RECOVERY_H
#define SS_DMS_RECOVERY_H

#include "port.h"
#include "ddes/dms/ss_common_attr.h"

#define REFORM_CTRL_PAGE  DMS_MAX_INSTANCE

#define RECOVERY_WAIT_TIME 10000
#define SSFAILOVER_TRIGGER (ENABLE_DMS && g_instance.dms_cxt.SSRecoveryInfo.failover_triggered == true && \
                            g_instance.dms_cxt.SSClusterState == NODESTATE_STANDBY_FAILOVER_PROMOTING)
#define SSSKIP_REDO_REPLAY (ENABLE_DMS && g_instance.dms_cxt.SSRecoveryInfo.skip_redo_replay == true)
#define SS_BEFORE_RECOVERY (ENABLE_DMS && g_instance.dms_cxt.SSReformInfo.in_reform == true \
                            && g_instance.dms_cxt.SSRecoveryInfo.recovery_pause_flag == true)
#define SS_IN_FAILOVER (ENABLE_DMS && g_instance.dms_cxt.SSRecoveryInfo.in_failover == true)

typedef struct st_reformer_ctrl {
    uint64 list_stable; // stable instances list
    int primaryInstId;
    pg_crc32c crc;
} ss_reformer_ctrl_t;

typedef struct st_reform_info {
    bool in_reform;
    dms_role_t dms_role;
} ss_reform_info_t;

typedef struct ss_recovery_info {
    bool recovery_pause_flag;
    volatile bool failover_triggered;
    char recovery_xlogDir[MAXPGPATH];
    bool skip_redo_replay;
    LWLock* update_seg_lock;
    bool new_primary_reset_walbuf_flag;
    bool reclsn_updated;
    bool ready_to_startup;              // when DB start (except failover), the flag will set true
    bool startup_reform;                // used to judge DB first start, when first reform finshed set false
    bool restart_failover_flag;         // used to indicate do failover when DB start
    bool reform_ready;
    bool in_failover;      // used to judge this is failover, this tag will combine with failover_triggered later
    // in failover Scenario,before failover_triggered become true, this node knows itself will become new primary
    bool in_flushcopy;
} ss_recovery_info_t;

extern bool SSRecoveryNodes();
extern int SSGetPrimaryInstId();
extern void SSSavePrimaryInstId(int id);
extern void SSReadControlFile(int id, bool updateDmsCtx = false);
extern void SSWriteReformerControlPages(void);
extern bool SSRecoveryApplyDelay(const XLogReaderState *record);
extern void SShandle_promote_signal();
extern void SSTriggerFailover();
extern void ss_failover_dw_init();


#endif
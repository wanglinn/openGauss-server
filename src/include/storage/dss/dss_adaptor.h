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
 * dss_adaptor.h
 * 
 * IDENTIFICATION
 *        src/include/storage/dss/dss_adaptor.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSS_ADAPTOR_H
#define DSS_ADAPTOR_H

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include "dss_api_def.h"

#define SS_LIBDSS_NAME "libdssapi.so"

int dss_device_init(const char *conn_path, bool enable_dss);

// callback for register dssapi
typedef int (*dss_open_device)(const char *name, int flags, int *handle);
typedef int (*dss_read_device)(int handle, void *buf, int size, int *read_size);
typedef int (*dss_write_device)(int handle, const void *buf, int size);
typedef int (*dss_pread_device)(int handle, void *buf, int size, long offset, int *read_size);
typedef int (*dss_pwrite_device)(int handle, const void *buf, int size, long offset);
typedef long (*dss_seek_device)(int handle, long offset, int origin);
typedef int (*dss_trucate_device)(int handle, long keep_size);
typedef int (*dss_create_device)(const char *name, int flags);
typedef int (*dss_remove_device)(const char *name);
typedef int (*dss_close_device)(int handle);
typedef int (*dss_exist_device)(const char *name, bool *result);
typedef int (*dss_create_device_dir)(const char *name);
typedef int (*dss_exist_device_dir)(const char *name, bool *result);
typedef dss_dir_handle (*dss_open_device_dir)(const char *name);
typedef int (*dss_read_device_dir)(dss_dir_handle dir, dss_dirent_t *item, dss_dir_item_t *result);
typedef int (*dss_close_device_dir)(dss_dir_handle dir);
typedef int (*dss_remove_device_dir)(const char *name);
typedef int (*dss_rename_device)(const char *src, const char *dst);
typedef int (*dss_check_device_size)(int size);
typedef int (*dss_align_device_size)(int size);
typedef int (*dss_link_device)(const char *oldpath, const char *newpath);
typedef int (*dss_unlink_device)(const char *path);
typedef int (*dss_exist_device_link)(const char *path, bool *result);
typedef int (*dss_device_name)(int handle, char *fname, size_t fname_size);
typedef int (*dss_read_device_link)(const char *path, char *buf, int bufsize);
typedef int (*dss_stat_device)(const char *path, dss_stat_info_t item);
typedef int (*dss_lstat_device)(const char *path, dss_stat_info_t item);
typedef int (*dss_fstat_device)(int handle, dss_stat_info_t item);
typedef int (*dss_set_status)(dss_server_status_t status);
typedef void (*dss_device_size)(const char *fname, long *fsize);
typedef void (*dss_error_info)(int *errorcode, const char **errormsg);
typedef void (*dss_svr_path)(const char *conn_path);
typedef void (*dss_log_callback)(dss_log_output cb_log_output);
typedef int (*dss_version)(void);
typedef int (*dss_aio_prep_pwrite_device)(void *iocb, int handle, void *buf, size_t count, long long offset);
typedef int (*dss_aio_prep_pread_device)(void *iocb, int handle, void *buf, size_t count, long long offset);
typedef struct st_dss_device_op_t {
    void *handle;
    dss_create_device dss_create;
    dss_remove_device dss_remove;
    dss_open_device dss_open;
    dss_read_device dss_read;
    dss_pread_device dss_pread;
    dss_write_device dss_write;
    dss_pwrite_device dss_pwrite;
    dss_seek_device dss_seek;
    dss_trucate_device dss_truncate;
    dss_close_device dss_close;
    dss_exist_device dss_exist;
    dss_create_device_dir dss_create_dir;
    dss_exist_device_dir dss_exist_dir;
    dss_rename_device dss_rename;
    dss_check_device_size dss_check_size;
    dss_align_device_size dss_align_size;
    dss_device_size dss_fsize;
    dss_device_name dss_fname;
    dss_error_info dss_get_error;
    dss_open_device_dir dss_open_dir; 
    dss_read_device_dir dss_read_dir;
    dss_close_device_dir dss_close_dir;
    dss_remove_device_dir dss_remove_dir;
    dss_link_device dss_link;
    dss_unlink_device dss_unlink;
    dss_exist_device_link dss_exist_link;
    dss_read_device_link dss_read_link;
    dss_stat_device dss_stat;
    dss_lstat_device dss_lstat;
    dss_fstat_device dss_fstat;
    dss_set_status dss_set_server_status;
    dss_svr_path dss_set_svr_path;
    dss_log_callback dss_register_log_callback;
    dss_version dss_get_version;
    dss_aio_prep_pwrite_device dss_aio_pwrite;
    dss_aio_prep_pread_device dss_aio_pread;
} dss_device_op_t;

void dss_register_log_callback(dss_log_output cb_log_output);

#endif // DSS_ADAPTOR_H
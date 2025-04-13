
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_PROCESS_CYCLE_H_INCLUDED_
#define _NGX_PROCESS_CYCLE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_CMD_OPEN_CHANNEL   1
#define NGX_CMD_CLOSE_CHANNEL  2
#define NGX_CMD_QUIT           3
#define NGX_CMD_TERMINATE      4
#define NGX_CMD_REOPEN         5

// nginx的模式 单进程
#define NGX_PROCESS_SINGLE     0
// 多进程模式 master-worker进程模式
#define NGX_PROCESS_MASTER     1
#define NGX_PROCESS_SIGNALLER  2
// 多进程模式的worker进程
#define NGX_PROCESS_WORKER     3
#define NGX_PROCESS_HELPER     4


typedef struct {
    ngx_event_handler_pt       handler;
    char                      *name;
    ngx_msec_t                 delay;
} ngx_cache_manager_ctx_t;


void ngx_master_process_cycle(ngx_cycle_t *cycle);
void ngx_single_process_cycle(ngx_cycle_t *cycle);

// 标识nginx的进程模式 单进程模式或者master-worker多进程模式
extern ngx_uint_t      ngx_process;
/**
 * 子进程编号 master进程会fork出worker进程 每个子进程都会用copy-on-write写时复制将master进程的内存空间复制到自己进程 所以每个子进程赋值这个变量时都是写了个副本在自己进程内 进程间互相不可见
 * 用途
 * <ul>
 *   <li></li>
 * </ul>
 * 在cycle的listening
 */
extern ngx_uint_t      ngx_worker;
extern ngx_pid_t       ngx_pid;
extern ngx_pid_t       ngx_new_binary;
extern ngx_uint_t      ngx_inherited;
extern ngx_uint_t      ngx_daemonized;
extern ngx_uint_t      ngx_exiting;

extern sig_atomic_t    ngx_reap;
extern sig_atomic_t    ngx_sigio;
extern sig_atomic_t    ngx_sigalrm;
extern sig_atomic_t    ngx_quit;
extern sig_atomic_t    ngx_debug_quit;
extern sig_atomic_t    ngx_terminate;
extern sig_atomic_t    ngx_noaccept;
extern sig_atomic_t    ngx_reconfigure;
extern sig_atomic_t    ngx_reopen;
extern sig_atomic_t    ngx_change_binary;


#endif /* _NGX_PROCESS_CYCLE_H_INCLUDED_ */

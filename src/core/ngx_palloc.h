
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_PALLOC_H_INCLUDED_
#define _NGX_PALLOC_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * NGX_MAX_ALLOC_FROM_POOL should be (ngx_pagesize - 1), i.e. 4095 on x86.
 * On Windows NT it decreases a number of locked pages in a kernel.
 */
/**
 * 内存池每次可以分配内存大小限制 跟系统平台有关
 */
#define NGX_MAX_ALLOC_FROM_POOL  (ngx_pagesize - 1)

#define NGX_DEFAULT_POOL_SIZE    (16 * 1024)

#define NGX_POOL_ALIGNMENT       16
#define NGX_MIN_POOL_SIZE                                                     \
    ngx_align((sizeof(ngx_pool_t) + 2 * sizeof(ngx_pool_large_t)),            \
              NGX_POOL_ALIGNMENT)


typedef void (*ngx_pool_cleanup_pt)(void *data);

typedef struct ngx_pool_cleanup_s  ngx_pool_cleanup_t;

struct ngx_pool_cleanup_s {
    ngx_pool_cleanup_pt   handler;
    void                 *data;
    ngx_pool_cleanup_t   *next;
};


typedef struct ngx_pool_large_s  ngx_pool_large_t;

// 内存池中大块内存链表形式
struct ngx_pool_large_s {
    // 链表的next域
    ngx_pool_large_t     *next;
    // 实际用来存储数据的内存
    void                 *alloc;
};

/**
 * 内存池中数据块链表节点 管理当前数据块的在内存池中划分的内存区域
 * 一个内存中有分为很多内存块 用链表形式组织
 * <ul>
 *   <li>内存池内 小内存块</li>
 *   <li>内存池外 大内存块</li>
 * </ul>
 * 大小=8+8+8+4=28byte->32byte
 */
typedef struct {
    /**
     * 内存块可分配内存区域的地址位置
     * 内存块中有连续内存 可能不是全部使用 而只是使用了其中一部分
     * last指向可分配的起始地址
     */
    u_char               *last;
    /**
     * 指向内存块可分配的最后地址
     * 也就是说[last...end)是可分配部分
     */
    u_char               *end;
    /**
     * 链表的next域 下一个内存块
     */
    ngx_pool_t           *next;
    /**
     * 内存块的分配失败次数
     * 什么叫分配失败
     * 就是内存池分配内存时要在内存块上找满足需求大小的内存块进行内存分配 当所有内存块都开辟不出来空间给需求时 就需要新建内存块了 这种情况就判定为一次内存分配失败
     * 找内存的时候是找了[current....)所有内存块 那么没个内存块分配失败计数都要+1
     * 这个计数的意义是设定一个阈值4 当current的分配次数超过4次 内存池的current就要易主
     */
    ngx_uint_t            failed;
} ngx_pool_data_t;


/**
 * 内存池的头部结构
 * 为啥要分小/大内存块
 * <ul>
 *   <li>小内存块要去看剩下的空间够不够使用需求的 减少内存碎片</li>
 *   <li>大内存块是直接分配的 虽然可能存在一些内存浪费 但是都使用大内存了 为了检索效率肯定用空间换时间</li>
 * </ul>
 * 大小=32+8+8+8+8+8+8=80byte
 */
struct ngx_pool_s {
    /**
     * 分配链
     * 内存池的内存块链表 小内存块
     * 内存布局的小设计 报ngx_pool_data_t放在最前面 p为内存池地址 以后就可以用p->d->next->d->next方式遍历内存块
     */
    ngx_pool_data_t       d;
    /**
     * 内存中内存块有两种规格 小内存块和大内存块
     * 这个值取决于内核页大小和实例化内存池指定的大小
     * 创建内存池指定的大小(约等于小内存块大小)=内存池头+小内存块头+内存块可分配大小
     * 内存池实例化的时候就决定好这个属性了 以此界定要申请的空间属性小内存还是大内存
     * <ul>
     *   <li>小内存就从分配链的小内存块上开辟 不足就新建内存块挂到分配链上</li>
     *   <li>大内存就新建大内存块挂到大内存块链表上</li>
     * </ul>
     */
    size_t                max;
    /**
     * 内存池正在使用的内存块 小内存块
     * 这个current是作用就是指向每次分配内存从哪个内存块开始遍历看看内存块有没有可用空间可以分配出去 current滑动的机制靠内存块分配失败计数
     * 为什么要设计这个 遍历的时候直接从链表头开始不就行了
     * <ul>
     *   <li>每次分配内存都从内存池的分配链开始遍历链表 这个方案当然可行 但是弊端也是显而易见 可能存在的情况是内存块分配满了 没有可用空间 但是每次都要被遍历 明显的性能损耗</li>
     *   <li>避免浪费轮询的机制就是不要每次都从链表头开始 而是从一个大概率可以分配成功的内存块开始遍历链表</li>
     * </ul>
     */
    ngx_pool_t           *current;
    ngx_chain_t          *chain;
    /*
     * 大内存块链表
     */
    ngx_pool_large_t     *large;
    ngx_pool_cleanup_t   *cleanup;
    ngx_log_t            *log;
};


typedef struct {
    ngx_fd_t              fd;
    u_char               *name;
    ngx_log_t            *log;
} ngx_pool_cleanup_file_t;


ngx_pool_t *ngx_create_pool(size_t size, ngx_log_t *log);
void ngx_destroy_pool(ngx_pool_t *pool);
void ngx_reset_pool(ngx_pool_t *pool);

void *ngx_palloc(ngx_pool_t *pool, size_t size);
void *ngx_pnalloc(ngx_pool_t *pool, size_t size);
void *ngx_pcalloc(ngx_pool_t *pool, size_t size);
void *ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment);
ngx_int_t ngx_pfree(ngx_pool_t *pool, void *p);


ngx_pool_cleanup_t *ngx_pool_cleanup_add(ngx_pool_t *p, size_t size);
void ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd);
void ngx_pool_cleanup_file(void *data);
void ngx_pool_delete_file(void *data);


#endif /* _NGX_PALLOC_H_INCLUDED_ */

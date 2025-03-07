
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_LIST_H_INCLUDED_
#define _NGX_LIST_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct ngx_list_part_s  ngx_list_part_t;

// 单链表节点
struct ngx_list_part_s {
    /*
     * 存放数据的内存起始位置
     * 元素占用空间总内存是size*nalloc
     * 那么一个链表节点数据空间是[elts...elts+size*nalloc)
     */
    void             *elts;
    // 链表节点上已经存放了多少个元素
    ngx_uint_t        nelts;
    ngx_list_part_t  *next;
};

/*
 * 单链表
 * 不同于双链表
 * <ul>
 *   <li>双链表维护了哨兵节点 哨兵节点占位用不存数据</li>
 *   <li>单链表不需要哨兵节点</li>
 * </ul>
 */
typedef struct {
    // 单链表的尾节点 为什么要维护尾节点呢 用头插就行了
    ngx_list_part_t  *last;
    // 单链表的头节点
    ngx_list_part_t   part;
    /*
     * <ul>
     *   <li>size 每个元素大小</li>
     *   <li>nalloc 链表上每个链表节点元素容量</li>
     * </ul>
     * size*nalloc就是一个节点上所有元素要占用的空间
     */
    size_t            size;
    ngx_uint_t        nalloc;
    ngx_pool_t       *pool;
} ngx_list_t;


ngx_list_t *ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size);

/*
 * 链表初始化
 * @param list 实例化好的链表
 * @param pool 内存池
 * @param n 一个链表节点的元素容量 一个链表节点存放多少个元素
 * @param size 一个元素大小
 * @return 操作状态码
 */
static ngx_inline ngx_int_t
ngx_list_init(ngx_list_t *list, ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    // 给链表节点分配内存存放数据
    list->part.elts = ngx_palloc(pool, n * size);
    if (list->part.elts == NULL) {
        return NGX_ERROR;
    }
    // 链表上已经存放的元素个数 还没放数据
    list->part.nelts = 0;
    list->part.next = NULL;
    list->last = &list->part;
    list->size = size;
    list->nalloc = n;
    list->pool = pool;

    return NGX_OK;
}


/*
 *
 *  the iteration through the list:
 *
 *  part = &list.part;
 *  data = part->elts;
 *
 *  for (i = 0 ;; i++) {
 *
 *      if (i >= part->nelts) {
 *          if (part->next == NULL) {
 *              break;
 *          }
 *
 *          part = part->next;
 *          data = part->elts;
 *          i = 0;
 *      }
 *
 *      ...  data[i] ...
 *
 *  }
 */


void *ngx_list_push(ngx_list_t *list);


#endif /* _NGX_LIST_H_INCLUDED_ */

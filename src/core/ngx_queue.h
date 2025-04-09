
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


#ifndef _NGX_QUEUE_H_INCLUDED_
#define _NGX_QUEUE_H_INCLUDED_


typedef struct ngx_queue_s  ngx_queue_t;

/*
 * 双链表实现的队列
 */
struct ngx_queue_s {
    // 前驱
    ngx_queue_t  *prev;
    // 后继
    ngx_queue_t  *next;
};

// 实例化好后进行初始化 头节点是哨兵节点 不存储数据
#define ngx_queue_init(q)                                                     \
    (q)->prev = q;                                                            \
    (q)->next = q


// 队列为空
#define ngx_queue_empty(h)                                                    \
    (h == (h)->prev)


/*
 * 头插入队
 * @param h 队列
 * @param x 节点入队
 */
#define ngx_queue_insert_head(h, x)                                           \
    (x)->next = (h)->next;                                                    \
    (x)->next->prev = x;                                                      \
    (x)->prev = h;                                                            \
    (h)->next = x


#define ngx_queue_insert_after   ngx_queue_insert_head


/*
 * 尾插入队 本质是把x插入到h的前驱
 * <ul>
 *   <li>当h是哨兵时 结果就是x被尾插到h链表的末数据节点</li>
 *   <li>是h是数据节点时 结果就是x被插入当作h的前驱节点</li>
 * </ul>
 * @param h 队列
 * @param x 节点入队
 */
#define ngx_queue_insert_tail(h, x)                                           \
    (x)->prev = (h)->prev;                                                    \
    (x)->prev->next = x;                                                      \
    (x)->next = h;                                                            \
    (h)->prev = x

// 第2个节点插入到第1个节点的前驱
#define ngx_queue_insert_before   ngx_queue_insert_tail


// 队列首元素
#define ngx_queue_head(h)                                                     \
    (h)->next

// 队列末元素
#define ngx_queue_last(h)                                                     \
    (h)->prev

// 队列的哨兵节点
#define ngx_queue_sentinel(h)                                                 \
    (h)

// 后继节点
#define ngx_queue_next(q)                                                     \
    (q)->next

// 前驱节点
#define ngx_queue_prev(q)                                                     \
    (q)->prev


#if (NGX_DEBUG)

#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next;                                              \
    (x)->prev = NULL;                                                         \
    (x)->next = NULL

#else

// 从双端链表中移除数据节点
#define ngx_queue_remove(x)                                                   \
    (x)->next->prev = (x)->prev;                                              \
    (x)->prev->next = (x)->next

#endif

/*
 * 链表拆分
 * h [...q) 前半部分以原来的哨兵为链表
 * n [q...] 后半部分以n为哨兵形成新的链表
 * @param h 原链表的哨兵节点
 * @param q 要拆的节点
 * @param n 新拆出来的链表的哨兵节点
 */
#define ngx_queue_split(h, q, n)                                              \
    (n)->prev = (h)->prev;                                                    \
    (n)->prev->next = n;                                                      \
    (n)->next = q;                                                            \
    (h)->prev = (q)->prev;                                                    \
    (h)->prev->next = h;                                                      \
    (q)->prev = n;


/*
 * 合并两个链表
 * <ul>
 *   <li>合并完新链表的哨兵节点为h</li>
 *   <li>只把n的数据节点部分挂到h上</li>
 * </ul>
 * @param h 以h为哨兵的双链表
 * @param n 以n为哨兵的双链表
 */
#define ngx_queue_add(h, n)                                                   \
    (h)->prev->next = (n)->next;                                              \
    (n)->next->prev = (h)->prev;                                              \
    (h)->prev = (n)->prev;                                                    \
    (h)->prev->next = h;


#define ngx_queue_data(q, type, link)                                         \
    (type *) ((u_char *) q - offsetof(type, link))


ngx_queue_t *ngx_queue_middle(ngx_queue_t *queue);
void ngx_queue_sort(ngx_queue_t *queue,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *));


#endif /* _NGX_QUEUE_H_INCLUDED_ */

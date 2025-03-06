
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static void ngx_queue_merge(ngx_queue_t *queue, ngx_queue_t *tail,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *));


/*
 * find the middle queue element if the queue has odd number of elements
 * or the first element of the queue's second part otherwise
 */
/*
 * 快慢指针方式找链表的中间元素
 */
ngx_queue_t *
ngx_queue_middle(ngx_queue_t *queue)
{
    ngx_queue_t  *middle, *next;
    // 链表中首元素
    middle = ngx_queue_head(queue);
    // 链表中末元素
    if (middle == ngx_queue_last(queue)) {
        // 链表中只有一个元素
        return middle;
    }
    // 首元素
    next = ngx_queue_head(queue);
    // 快慢指针 middle慢指针 next快指针
    for ( ;; ) {
        middle = ngx_queue_next(middle);

        next = ngx_queue_next(next);

        if (next == ngx_queue_last(queue)) {
            return middle;
        }

        next = ngx_queue_next(next);

        if (next == ngx_queue_last(queue)) {
            return middle;
        }
    }
}


/* the stable merge sort */

/**
 * 归并排序
 * @param queue 双链表的哨兵节点
 */
void
ngx_queue_sort(ngx_queue_t *queue,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *))
{
    ngx_queue_t  *q, tail;

    q = ngx_queue_head(queue);

    if (q == ngx_queue_last(queue)) {
        // 队列只有1个元素
        return;
    }
    // 队列中间元素
    q = ngx_queue_middle(queue);
    /*
     * 一分为二
     * queue是前半部分 [...q)
     * tail是后半部分 [q...]
     * 分别对前后两部分分别排序 然后合并
     */
    ngx_queue_split(queue, q, &tail);

    ngx_queue_sort(queue, cmp);
    ngx_queue_sort(&tail, cmp);

    ngx_queue_merge(queue, &tail, cmp);
}

/*
 * 合并升序链表 tail上节点都合并到queue上 保持升序
 * @param queue 链表
 * @param tail 链表
 * @param cmp 比较函数
 */
static void
ngx_queue_merge(ngx_queue_t *queue, ngx_queue_t *tail,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *))
{
    ngx_queue_t  *q1, *q2;
    /*
     * 遍历两条链表合并到queue上
     */
    q1 = ngx_queue_head(queue);
    q2 = ngx_queue_head(tail);
    for ( ;; ) {
        if (q1 == ngx_queue_sentinel(queue)) {
            // 第1条链表空了 第2条链表所有数据节点都挂到第1条链表上
            ngx_queue_add(queue, tail);
            break;
        }
        if (q2 == ngx_queue_sentinel(tail)) {
            // 第2条链表空了
            break;
        }
        if (cmp(q1, q2) <= 0) {
            // 继续遍历
            q1 = ngx_queue_next(q1);
            continue;
        }
        // 从第2条链表上移除节点 插入到第1条链表节点前驱
        ngx_queue_remove(q2);
        ngx_queue_insert_before(q1, q2);
        q2 = ngx_queue_head(tail);
    }
}

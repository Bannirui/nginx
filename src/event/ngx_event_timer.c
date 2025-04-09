
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


// 任务队列 存放普通定时任务
ngx_rbtree_t              ngx_event_timer_rbtree;
static ngx_rbtree_node_t  ngx_event_timer_sentinel;

/*
 * the event timer rbtree may contain the duplicate keys, however,
 * it should not be a problem, because we use the rbtree to find
 * a minimum timer value only
 */

ngx_int_t
ngx_event_timer_init(ngx_log_t *log)
{
    ngx_rbtree_init(&ngx_event_timer_rbtree, &ngx_event_timer_sentinel,
                    ngx_rbtree_insert_timer_value);

    return NGX_OK;
}

/*
 * 计算下一个任务的超时时间(ms)
 * @return 下一个定时器事件的超时值 即下一个需要处理的定时器的时间间隔 还要等多久就可以处理事件了
 * 会检查定时器队列中的所有事件 并计算出下一个最早的定时器超时时间
 */
ngx_msec_t
ngx_event_find_timer(void)
{
    ngx_msec_int_t      timer;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    if (ngx_event_timer_rbtree.root == &ngx_event_timer_sentinel) {
        return NGX_TIMER_INFINITE;
    }
    // 待处理事件用红黑树维护的队列
    root = ngx_event_timer_rbtree.root;
    // 红黑树的边界
    sentinel = ngx_event_timer_rbtree.sentinel;
    // 从红黑树中找到最小节点 就是待处理事件最早到期的 最早需要处理的事件
    node = ngx_rbtree_min(root, sentinel);
    // 任务定时器 再过timer(ms)需要执行任务
    timer = (ngx_msec_int_t) (node->key - ngx_current_msec);

    return (ngx_msec_t) (timer > 0 ? timer : 0);
}

/**
 * 处理超时事件
 * 事件循环线程每个处理周期都会在恰当时机被唤醒
 * 在这个唤醒时机 定时任务就获得到一次被执行的机会
 * <ul>
 *   <li>找到过期的定时器从红黑树中移除</li>
 *   <li>根据定时器找到对应的事件 回调事件</li>
 * </ul>
 */
void
ngx_event_expire_timers(void)
{
    // 事件 根据timer定时器的地址倒推出来事件的地址
    ngx_event_t        *ev;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    sentinel = ngx_event_timer_rbtree.sentinel;
    // 轮询检索红黑树把过期的定时器删除
    for ( ;; ) {
        // 红黑树的根
        root = ngx_event_timer_rbtree.root;
        // 到了树的边界 说明找遍了整棵树
        if (root == sentinel) {
            return;
        }
        // 事件队列中最早超时到期的事件
        node = ngx_rbtree_min(root, sentinel);

        /* node->key > ngx_current_msec */
        // 任务定时器队列中最早到期的都还没超时 说明所有的任务都还没超时
        if ((ngx_msec_int_t) (node->key - ngx_current_msec) > 0) {
            return;
        }
        // 已经找到有任务超时过期了 根据事件的定时器找到事件本身
        ev = ngx_rbtree_data(node, ngx_event_t, timer);

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "event timer del: %d: %M",
                       ngx_event_ident(ev->data), ev->timer.key);
        // 从红黑树中删除过期的定时器
        ngx_rbtree_delete(&ngx_event_timer_rbtree, &ev->timer);

#if (NGX_DEBUG)
        ev->timer.left = NULL;
        ev->timer.right = NULL;
        ev->timer.parent = NULL;
#endif

        ev->timer_set = 0;

        ev->timedout = 1;
        // 回调事件
        ev->handler(ev);
    }
}


ngx_int_t
ngx_event_no_timers_left(void)
{
    ngx_event_t        *ev;
    ngx_rbtree_node_t  *node, *root, *sentinel;

    sentinel = ngx_event_timer_rbtree.sentinel;
    root = ngx_event_timer_rbtree.root;

    if (root == sentinel) {
        return NGX_OK;
    }

    for (node = ngx_rbtree_min(root, sentinel);
         node;
         node = ngx_rbtree_next(&ngx_event_timer_rbtree, node))
    {
        ev = ngx_rbtree_data(node, ngx_event_t, timer);

        if (!ev->cancelable) {
            return NGX_AGAIN;
        }
    }

    /* only cancelable timers left */

    return NGX_OK;
}

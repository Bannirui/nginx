
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

// 存放网络accept事件 监听端口的新连接事件
ngx_queue_t  ngx_posted_accept_events;
/**
* 当某个事件触发后 Nginx可能不是马上处理它 而是判断当前负载 连接状态等条件
* <ul>
*   <li>如果当前不适合处理 就把它放入ngx_posted_next_events</li>
*   <li>等到下一轮事件循环再处理它</li>
* </ul>
 */
ngx_queue_t  ngx_posted_next_events;
// 存放网络IO的普通事件也就是是网络IO的读写事件
ngx_queue_t  ngx_posted_events;

/**
 * 尝试处理accept队列的事件任务
 * <ul>
 *   <li>ngx_posted_accept_events这个队列里面缓存的是事件循环前置步骤调用内核kevent拿到的就绪网络连接事件</li>
 *   <li>现在在这个环节把队列里面缓存待处理的连接任务都处理掉</li>
 * </ul>
 * 换言之 队列里面空的就没连接事件要处理 有多少就处理多少
 */
void
ngx_event_process_posted(ngx_cycle_t *cycle, ngx_queue_t *posted)
{
    ngx_queue_t  *q;
    ngx_event_t  *ev;
	// 轮询队列里面待处理的连接事件进行处理
    while (!ngx_queue_empty(posted)) {

        q = ngx_queue_head(posted);
        ev = ngx_queue_data(q, ngx_event_t, queue);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                      "posted event %p", ev);

        ngx_delete_posted_event(ev);

        ev->handler(ev);
    }
}


void
ngx_event_move_posted_next(ngx_cycle_t *cycle)
{
    ngx_queue_t  *q;
    ngx_event_t  *ev;

    for (q = ngx_queue_head(&ngx_posted_next_events);
         q != ngx_queue_sentinel(&ngx_posted_next_events);
         q = ngx_queue_next(q))
    {
        ev = ngx_queue_data(q, ngx_event_t, queue);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                      "posted next event %p", ev);

        ev->ready = 1;
        ev->available = -1;
    }

    ngx_queue_add(&ngx_posted_events, &ngx_posted_next_events);
    ngx_queue_init(&ngx_posted_next_events);
}

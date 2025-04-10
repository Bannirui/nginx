
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

// kq模块的配置
typedef struct {
    // 对应kq系统调用的changelist 决定changelist的长度 一次可以注册多少个事件变更
    ngx_uint_t  changes;
    // 对应kq系统调用的eventlist 决定eventlist的长度 一次最多可以等待多少个事件
    ngx_uint_t  events;
} ngx_kqueue_conf_t;


static ngx_int_t ngx_kqueue_init(ngx_cycle_t *cycle, ngx_msec_t timer);
#ifdef EVFILT_USER
static ngx_int_t ngx_kqueue_notify_init(ngx_log_t *log);
#endif
static void ngx_kqueue_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_kqueue_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_kqueue_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_kqueue_set_event(ngx_event_t *ev, ngx_int_t filter,
    ngx_uint_t flags);
#ifdef EVFILT_USER
static ngx_int_t ngx_kqueue_notify(ngx_event_handler_pt handler);
#endif
static ngx_int_t ngx_kqueue_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags);
static ngx_inline void ngx_kqueue_dump_event(ngx_log_t *log,
    struct kevent *kev);

static void *ngx_kqueue_create_conf(ngx_cycle_t *cycle);
static char *ngx_kqueue_init_conf(ngx_cycle_t *cycle, void *conf);

// kq的实例
int                    ngx_kqueue = -1;
// 要注册到kq的变更事件
static struct kevent  *change_list;
// kq系统调用返回的就绪的事件
static struct kevent  *event_list;
/*
 * nchanges 要注册的变更事件个数 change_list中事件数量
 * nevents event_list数组长度 不是event_list中就绪事件的个数 比如数组长度10 其中放了5个就绪事件
 */
static ngx_uint_t      max_changes, nchanges, nevents;

#ifdef EVFILT_USER
static ngx_event_t     notify_event;
static struct kevent   notify_kev;
#endif


static ngx_str_t      kqueue_name = ngx_string("kqueue");

static ngx_command_t  ngx_kqueue_commands[] = {

    { ngx_string("kqueue_changes"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_kqueue_conf_t, changes),
      NULL },

    { ngx_string("kqueue_events"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_kqueue_conf_t, events),
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_kqueue_module_ctx = {
    &kqueue_name,
    ngx_kqueue_create_conf,                /* create configuration */
    // 设置kq的changes和events默认值512
    ngx_kqueue_init_conf,                  /* init configuration */

    {
        ngx_kqueue_add_event,              /* add an event */
        ngx_kqueue_del_event,              /* delete an event */
        ngx_kqueue_add_event,              /* enable an event */
        ngx_kqueue_del_event,              /* disable an event */
        NULL,                              /* add an connection */
        NULL,                              /* delete an connection */
#ifdef EVFILT_USER
        ngx_kqueue_notify,                 /* trigger a notify */
#else
        NULL,                              /* trigger a notify */
#endif
        ngx_kqueue_process_events,         /* process the events */
        ngx_kqueue_init,                   /* init the events */
        ngx_kqueue_done                    /* done the events */
    }

};

ngx_module_t  ngx_kqueue_module = {
    NGX_MODULE_V1,
    &ngx_kqueue_module_ctx,                /* module context */
    ngx_kqueue_commands,                   /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/*
 * 初始化kq的时候根据模块需求 是不是需要高精度定时器 如果需要高精度定时器就借助kq的定时器事件实现
 * @param timer ms时间 向kq注册个定时器事件 间隔就是这个时间 也就是每隔timer时间就有就绪事件到达 selector被唤醒
 *              为什么设置这个参数 为了系统的效率 兼顾处理网络任务和普通任务
 *              <ul>
 *                <li>系统调用阻塞式 一直等到有事件就绪到达</li>
 *                <li>发起系统调用时指定超时时间 确保不会一直陷入阻塞 这种方式的定时时间依赖<ul>
 *                  <li>系统层面计算超时时间 准确度可能存在问题</li>
 *                  <li>在发起调用时指定超时时间 这种方式的定时精度可能不够</li>
 *                </ul></li>
 *                <li>向kq注册定时器事件 这种定时精度高 本质就是高精度定时器</li>
 *              </ul>
 */
static ngx_int_t
ngx_kqueue_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ngx_kqueue_conf_t  *kcf;
    struct timespec     ts;
#if (NGX_HAVE_TIMER_EVENT)
    struct kevent       kev;
#endif
    /*
     * 设置kq的changes和events默认值512
     * <ul>
     *   <li>一次最多可以注册512个事件变更</li>
     *   <li>一次最多可以接收512个就绪事件</li>
     * </ul>
     */
    kcf = ngx_event_get_conf(cycle->conf_ctx, ngx_kqueue_module);

    if (ngx_kqueue == -1) {
        // 系统调用 实例化kq
        ngx_kqueue = kqueue();

        if (ngx_kqueue == -1) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "kqueue() failed");
            return NGX_ERROR;
        }

#ifdef EVFILT_USER
        if (ngx_kqueue_notify_init(cycle->log) != NGX_OK) {
            return NGX_ERROR;
        }
#endif
    }

    if (max_changes < kcf->changes) {
        // nchanges是要注册的变更事件个数 就是change_list长度
        if (nchanges) {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
            // change_list中变更事件注册到kq
            if (kevent(ngx_kqueue, change_list, (int) nchanges, NULL, 0, &ts)
                == -1)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "kevent() failed");
                return NGX_ERROR;
            }
            nchanges = 0;
        }

        if (change_list) {
            ngx_free(change_list);
        }
        // 上面释放了change_list数组的内存 现在重新分配数组内存 用来将来存放要变更的事件
        change_list = ngx_alloc(kcf->changes * sizeof(struct kevent),
                                cycle->log);
        if (change_list == NULL) {
            return NGX_ERROR;
        }
    }

    max_changes = kcf->changes;

    if (nevents < kcf->events) {
        if (event_list) {
            ngx_free(event_list);
        }

        event_list = ngx_alloc(kcf->events * sizeof(struct kevent), cycle->log);
        if (event_list == NULL) {
            return NGX_ERROR;
        }
    }

    ngx_event_flags = NGX_USE_ONESHOT_EVENT
                      |NGX_USE_KQUEUE_EVENT
                      |NGX_USE_VNODE_EVENT;

#if (NGX_HAVE_TIMER_EVENT)
    // 通过向kq注册定时器事件方式实现高精度定时器
    if (timer) {
        kev.ident = 0;
        // 表明注册的事件类型是定时器事件 不是读写事件 kq会在设定的时间间隔触发这个事件
        kev.filter = EVFILT_TIMER;
        /*
         * 两个作用
         * <ul>
         *   <li>ADD表明向kq注册新事件 如果kq中已经存在这个事件就更新</li>
         *   <li>ENABLE表明启用这个事件 让它开始工作</li>
         * </ul>
         */
        kev.flags = EV_ADD|EV_ENABLE;
        // 定时器不需要子标志
        kev.fflags = 0;
        // 间隔时间 ms
        kev.data = timer;
        kev.udata = 0;

        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        // 向kq注册定时器
        if (kevent(ngx_kqueue, &kev, 1, NULL, 0, &ts) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "kevent(EVFILT_TIMER) failed");
            return NGX_ERROR;
        }

        ngx_event_flags |= NGX_USE_TIMER_EVENT;
    }

#endif

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags |= NGX_USE_CLEAR_EVENT;
#else
    ngx_event_flags |= NGX_USE_LEVEL_EVENT;
#endif

#if (NGX_HAVE_LOWAT_EVENT)
    ngx_event_flags |= NGX_USE_LOWAT_EVENT;
#endif

    nevents = kcf->events;

    ngx_io = ngx_os_io;

    ngx_event_actions = ngx_kqueue_module_ctx.actions;

    return NGX_OK;
}


#ifdef EVFILT_USER

static ngx_int_t
ngx_kqueue_notify_init(ngx_log_t *log)
{
    notify_kev.ident = 0;
    notify_kev.filter = EVFILT_USER;
    notify_kev.data = 0;
    notify_kev.flags = EV_ADD|EV_CLEAR;
    notify_kev.fflags = 0;
    notify_kev.udata = 0;

    if (kevent(ngx_kqueue, &notify_kev, 1, NULL, 0, NULL) == -1) {
        ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
                      "kevent(EVFILT_USER, EV_ADD) failed");
        return NGX_ERROR;
    }

    notify_event.active = 1;
    notify_event.log = log;

    notify_kev.flags = 0;
    notify_kev.fflags = NOTE_TRIGGER;
    notify_kev.udata = NGX_KQUEUE_UDATA_T ((uintptr_t) &notify_event);

    return NGX_OK;
}

#endif


static void
ngx_kqueue_done(ngx_cycle_t *cycle)
{
    if (close(ngx_kqueue) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "kqueue close() failed");
    }

    ngx_kqueue = -1;

    ngx_free(change_list);
    ngx_free(event_list);

    change_list = NULL;
    event_list = NULL;
    max_changes = 0;
    nchanges = 0;
    nevents = 0;
}


static ngx_int_t
ngx_kqueue_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_int_t          rc;
#if 0
    ngx_event_t       *e;
    ngx_connection_t  *c;
#endif

    ev->active = 1;
    ev->disabled = 0;
    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;

#if 0

    if (ev->index < nchanges
        && ((uintptr_t) change_list[ev->index].udata & (uintptr_t) ~1)
            == (uintptr_t) ev)
    {
        if (change_list[ev->index].flags == EV_DISABLE) {

            /*
             * if the EV_DISABLE is still not passed to a kernel
             * we will not pass it
             */

            ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                           "kevent activated: %d: ft:%i",
                           ngx_event_ident(ev->data), event);

            if (ev->index < --nchanges) {
                e = (ngx_event_t *)
                    ((uintptr_t) change_list[nchanges].udata & (uintptr_t) ~1);
                change_list[ev->index] = change_list[nchanges];
                e->index = ev->index;
            }

            return NGX_OK;
        }

        c = ev->data;

        ngx_log_error(NGX_LOG_ALERT, ev->log, 0,
                      "previous event on #%d were not passed in kernel", c->fd);

        return NGX_ERROR;
    }

#endif

    rc = ngx_kqueue_set_event(ev, event, EV_ADD|EV_ENABLE|flags);

    return rc;
}


static ngx_int_t
ngx_kqueue_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_int_t     rc;
    ngx_event_t  *e;

    ev->active = 0;
    ev->disabled = 0;

    if (ev->index < nchanges
        && ((uintptr_t) change_list[ev->index].udata & (uintptr_t) ~1)
            == (uintptr_t) ev)
    {
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "kevent deleted: %d: ft:%i",
                       ngx_event_ident(ev->data), event);

        /* if the event is still not passed to a kernel we will not pass it */

        nchanges--;

        if (ev->index < nchanges) {
            e = (ngx_event_t *)
                    ((uintptr_t) change_list[nchanges].udata & (uintptr_t) ~1);
            change_list[ev->index] = change_list[nchanges];
            e->index = ev->index;
        }

        return NGX_OK;
    }

    /*
     * when the file descriptor is closed the kqueue automatically deletes
     * its filters so we do not need to delete explicitly the event
     * before the closing the file descriptor.
     */

    if (flags & NGX_CLOSE_EVENT) {
        return NGX_OK;
    }

    if (flags & NGX_DISABLE_EVENT) {
        ev->disabled = 1;

    } else {
        flags |= EV_DELETE;
    }

    rc = ngx_kqueue_set_event(ev, event, flags);

    return rc;
}


static ngx_int_t
ngx_kqueue_set_event(ngx_event_t *ev, ngx_int_t filter, ngx_uint_t flags)
{
    // kq的事件体 在udata阈上存放的是nginx封装的事件体
    struct kevent     *kev;
    struct timespec    ts;
    ngx_connection_t  *c;

    c = ev->data;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "kevent set event: %d: ft:%i fl:%04Xi",
                   c->fd, filter, flags);

    if (nchanges >= max_changes) {
        ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                      "kqueue change list is filled up");

        ts.tv_sec = 0;
        ts.tv_nsec = 0;

        if (kevent(ngx_kqueue, change_list, (int) nchanges, NULL, 0, &ts)
            == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno, "kevent() failed");
            return NGX_ERROR;
        }

        nchanges = 0;
    }

    kev = &change_list[nchanges];

    kev->ident = c->fd;
    kev->filter = (short) filter;
    kev->flags = (u_short) flags;
    /*
     * 这个地方的设计是用来防事件过期的校验
     * udata存的是一个nginx封装的事件的伪地址 包括两部分信息
     * <ul>
     *   <li>nginx事件的真实地址信息</li>
     *   <li>事件伪触发过期的校验码</li>
     * </ul>
     * 首先关于地址对齐
     * <ul>
     *   <li>64位架构是8Byte对齐 地址低3位是0</li>
     *   <li>32位架构是4Byte对齐 地址低2位是0</li>
     * </ul>
     * 也就是说指针的最低位是没有用了 可以复用 只要在解引用的时候还原成0就行了
     * 那么就可以在指针的最低位放上版本号
     */
    kev->udata = NGX_KQUEUE_UDATA_T ((uintptr_t) ev | ev->instance);

    if (filter == EVFILT_VNODE) {
        kev->fflags = NOTE_DELETE|NOTE_WRITE|NOTE_EXTEND
                                 |NOTE_ATTRIB|NOTE_RENAME
#if (__FreeBSD__ == 4 && __FreeBSD_version >= 430000) \
    || __FreeBSD_version >= 500018
                                 |NOTE_REVOKE
#endif
                      ;
        kev->data = 0;

    } else {
#if (NGX_HAVE_LOWAT_EVENT)
        if (flags & NGX_LOWAT_EVENT) {
            kev->fflags = NOTE_LOWAT;
            kev->data = ev->available;

        } else {
            kev->fflags = 0;
            kev->data = 0;
        }
#else
        kev->fflags = 0;
        kev->data = 0;
#endif
    }

    ev->index = nchanges;
    nchanges++;

    if (flags & NGX_FLUSH_EVENT) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ev->log, 0, "kevent flush");

        if (kevent(ngx_kqueue, change_list, (int) nchanges, NULL, 0, &ts)
            == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno, "kevent() failed");
            return NGX_ERROR;
        }

        nchanges = 0;
    }

    return NGX_OK;
}


#ifdef EVFILT_USER

static ngx_int_t
ngx_kqueue_notify(ngx_event_handler_pt handler)
{
    notify_event.handler = handler;

    if (kevent(ngx_kqueue, &notify_kev, 1, NULL, 0, NULL) == -1) {
        ngx_log_error(NGX_LOG_ALERT, notify_event.log, ngx_errno,
                      "kevent(EVFILT_USER, NOTE_TRIGGER) failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}

#endif

/**
 * 发起一次kq系统调用看看有没有就绪的事件
 * @param cycle
 * @param timer 定时器 selector系统调用涉及阻塞 系统提供的API带超时 指定系统调用的超时时间ms
 *              <ul>定时器有2种实现方式
 *                <li>1是借助系统调用超时机制</li>
 *                <li>2是借助kq的定时器事件 这种定时器是高精度定时器</li>
 *              </ul>
 *              定时器的作用是周期性更新系统时间
 * @param flags
 * @return
 */
static ngx_int_t
ngx_kqueue_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags)
{
    int               events, n;
    ngx_int_t         i, instance;
    ngx_uint_t        level;
    ngx_err_t         err;
    ngx_event_t      *ev;
    ngx_queue_t      *queue;
    struct timespec   ts, *tp;

    n = (int) nchanges;
    nchanges = 0;

    if (timer == NGX_TIMER_INFINITE) {
        // 这个标识说明定时器用的是高精度定时器机制
        tp = NULL;

    } else {
        // 设置了明确的复用器调用超时时间 通过系统调用超时非阻塞方式达到定时器机制
        ts.tv_sec = timer / 1000;
        ts.tv_nsec = (timer % 1000) * 1000000;

        /*
         * 64-bit Darwin kernel has the bug: kernel level ts.tv_nsec is
         * the int32_t while user level ts.tv_nsec is the long (64-bit),
         * so on the big endian PowerPC all nanoseconds are lost.
         */

#if (NGX_DARWIN_KEVENT_BUG)
        ts.tv_nsec <<= 32;
#endif

        tp = &ts;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "kevent timer: %M, changes: %d", timer, n);
    /*
     * @param ngx_kqueue kq实例
     * @param tp 系统调用的超时设置 没有事件到达唤醒线程 就阻塞到这个时间后唤醒线程不要一直阻塞
     *           <ul>
     *             <li>有值 超时唤醒</li>
     *             <li>没值 用kq定时器事件 配置kq实例化时注册定时器事件</li>
     *           </ul>
     * @return events 就绪事件数量
     */
    events = kevent(ngx_kqueue, change_list, n, event_list, (int) nevents, tp);

    err = (events == -1) ? ngx_errno : 0;

    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "kevent events: %d", events);

    if (err) {
        if (err == NGX_EINTR) {

            if (ngx_event_timer_alarm) {
                ngx_event_timer_alarm = 0;
                return NGX_OK;
            }

            level = NGX_LOG_INFO;

        } else {
            level = NGX_LOG_ALERT;
        }

        ngx_log_error(level, cycle->log, err, "kevent() failed");
        return NGX_ERROR;
    }

    if (events == 0) {
        if (timer != NGX_TIMER_INFINITE) {
            // 就绪事件0个 系统调用为什么会返回 肯定是系统调用超时时间到了
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "kevent() returned no events without timeout");
        return NGX_ERROR;
    }
    // 遍历就绪事件 分门别类放到任务队列中等待处理
    for (i = 0; i < events; i++) {

        ngx_kqueue_dump_event(cycle->log, &event_list[i]);

        if (event_list[i].flags & EV_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, event_list[i].data,
                          "kevent() error on %d filter:%d flags:%04Xd",
                          (int) event_list[i].ident, event_list[i].filter,
                          event_list[i].flags);
            continue;
        }

#if (NGX_HAVE_TIMER_EVENT)

        // 就绪的事件是个定时器事件 借助这个事件更新系统时间 等会函数调用方的主线程要触发定时任务执行 依赖更新过后的系统时间来判断任务是否到期
        if (event_list[i].filter == EVFILT_TIMER) {
            // 更新系统时间
            ngx_time_update();
            continue;
        }

#endif

        ev = (ngx_event_t *) event_list[i].udata;
        // 就绪事件类型
        switch (event_list[i].filter) {

        case EVFILT_READ: // 可读
        case EVFILT_WRITE: // 可写
            /*
             * 读写事件的处理
             * <ul>
             *   <li>可读的触发条件
             *     <ul>
             *       <li>socket中有数据没有被读取</li>
             *       <li>文件 设备准备好可以读取</li>
             *       <li>连接被关闭 连接被关闭的时候会返回事件可读并且可读的data长度是0</li>
             *     </ul>
             *   </li>
             *   <li>可写的触发条件
             *     <ul>
             *       <li>socket写缓冲区中有数据</li>
             *       <li>文件描述符已就绪可写 但不表示对方一定能收完数据</li>
             *     </ul>
             *   </li>
             * </ul>
             */
            /*
             * 伪事件的防御设计
             * 在复用器kq的udata中存放的是一个变种地址
             * <ul>
             *   <li>指针后3位是0</li>
             *   <li>最低位被放上了翻转版本号</li>
             * </ul>
             * 所以拿到内核返回的udata
             * <ul>
             *   <li>只要把最低位抹成0就是真正的用户事件地址</li>
             *   <li>只解析最低位的1bit就是翻转版本号 防伪码</li>
             * </ul>
             */
            instance = (uintptr_t) ev & 1;
            ev = (ngx_event_t *) ((uintptr_t) ev & (uintptr_t) ~1);
            /*
             * 解决事件伪触发问题的体现
             * 这边有几个关注点
             * <ul>
             *   <li>1 防伪码为什么只要2种就行 也就是0和1翻转为什么可以达到验伪事件效果 为什么不需要考虑更久之前的连接</li>
             *   <li>2 event是nginx抽象的 在向复用器注册事件时翻转instance值 所谓反转就是上次是0这次就是1 上次是1这次就是0 所以nginx是怎么知道event上一次的instance值是多少的</li>
             * </ul>
             * 这两个问题
             * <ul>
             *   <li>第1个问题 是操作系统保证的 内核中过时事件的保留是短暂的 只会保留一次触发后没被消费掉的伪事件 之后会被清理或覆盖 也就是伪事件根本不会出现更早的连接 最多只有上一次的连接 所以nginx要做的事情就是不要把伪事件注册回复用器就行 识别出伪事件什么也不用做</li>
             *   <li>第2个问题 nginx中有内存池 所谓的连接关闭仅仅是在结构体标识位打上关闭标识然后把内存还给内存池 并没有真正把内存free给操作系统 所以下一次分配到的event地址里面就是上一次遗留的instance值</li>
             * </ul>
             * 操作系统的伪事件留存机制和nginx内存池设计一起作用 只要翻转instance就足够保证防御伪事件
             * event在内存池中 在上一次释放后 再拿到同一个event地址后 event状态无非就两种
             * <ul>
             *   <li>再没被分配出去 也就是没有被复用 它的状态还是close</li>
             *   <li>被分配出去了 也就是被复用了 它的状态不是close 所以要进行验证 看看是不是过期了 也就是伪事件</li>
             * </ul>
             */
            if (ev->closed || ev->instance != instance) {
                /*
                 * the stale event from a file descriptor
                 * that was just closed in this iteration
                 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                               "kevent: stale event %p", ev);
                // 不用处理 操作系统自会回收清除没有被处理的伪事件
                continue;
            }

            if (ev->log && (ev->log->log_level & NGX_LOG_DEBUG_CONNECTION)) {
                ngx_kqueue_dump_event(ev->log, &event_list[i]);
            }

            if (ev->oneshot) {
                ev->active = 0;
            }

            ev->available = event_list[i].data;

            if (event_list[i].flags & EV_EOF) {
                ev->pending_eof = 1;
                ev->kq_errno = event_list[i].fflags;
            }

            ev->ready = 1;

            break;

        case EVFILT_VNODE: // 监听文件变化 监听文件或目录的变化(某个文件是否被修改 删除 重命名)
            ev->kq_vnode = 1;

            break;

        case EVFILT_AIO: // 异步IO完成通知
            ev->complete = 1;
            ev->ready = 1;

            break;

#ifdef EVFILT_USER
        case EVFILT_USER: // 用户自定义触发事件
            break;
#endif

        default:
            ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                          "unexpected kevent() filter %d",
                          event_list[i].filter);
            continue;
        }

        if (flags & NGX_POST_EVENTS) {
            /*
             * 为什么要分开 因为这两类事件的处理场景 优先级 调度策略都不同
             * <ul>
             *   <li>ev->accept==1 新连接事件 投递到ngx_posted_accept_events队列</li>
             *   <li>已有连接上的读写 投递到ngx_posted_events队列</li>
             * </ul>
             * 为什么要分开处理
             * <ul>
             *   <li>Accept事件处理通常更轻 但更频繁 Accept事件只需要调用accept()接收新连接 然后创建连接结构体 这一步很快 但在高并发场景中非常频繁 如果和业务请求混在一起处理 可能会导致请求被延迟处理 所以优先或独立处理accept 可以提升请求接收效率</li>
             *   <li>防止惊群效应 Nginx多进程时 每个进程都可能监听相同的端口 如果同时处理accept和业务事件 很容易导致惊群 通过单独调度ngx_posted_accept_events 可以设置为只有一个进程处理accept 其余进程处理业务 提高负载均衡效果</li>
             *   <li>便于定制不同的处理策略 分开队列就能做到<ul>
             *     <li>accept队列 可以批量处理多个连接再处理请求</li>
             *     <li>普通事件队列 按照负载控制 节流处理业务请求</li>
             *   </ul></li>
             * </ul>
             * Nginx甚至可以配置multi_accept 一次处理多个accept事件 这种策略就只对ngx_posted_accept_events起作用
             */
            queue = ev->accept ? &ngx_posted_accept_events
                               : &ngx_posted_events;
            // 事件投递到队列
            ngx_post_event(ev, queue);

            continue;
        }
        // 默认情况下都是把就绪事件入队处理 不是同步处理 因此不会执行到这
        ev->handler(ev);
    }

    return NGX_OK;
}


static ngx_inline void
ngx_kqueue_dump_event(ngx_log_t *log, struct kevent *kev)
{
    if (kev->ident > 0x8000000 && kev->ident != (unsigned) -1) {
        ngx_log_debug6(NGX_LOG_DEBUG_EVENT, log, 0,
                       "kevent: %p: ft:%d fl:%04Xd ff:%08Xd d:%d ud:%p",
                       (void *) kev->ident, kev->filter,
                       kev->flags, kev->fflags,
                       (int) kev->data, kev->udata);

    } else {
        ngx_log_debug6(NGX_LOG_DEBUG_EVENT, log, 0,
                       "kevent: %d: ft:%d fl:%04Xd ff:%08Xd d:%d ud:%p",
                       (int) kev->ident, kev->filter,
                       kev->flags, kev->fflags,
                       (int) kev->data, kev->udata);
    }
}


static void *
ngx_kqueue_create_conf(ngx_cycle_t *cycle)
{
    ngx_kqueue_conf_t  *kcf;

    kcf = ngx_palloc(cycle->pool, sizeof(ngx_kqueue_conf_t));
    if (kcf == NULL) {
        return NULL;
    }

    kcf->changes = NGX_CONF_UNSET;
    kcf->events = NGX_CONF_UNSET;

    return kcf;
}

/*
 * kq的changes和events默认值设置
 */
static char *
ngx_kqueue_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_kqueue_conf_t *kcf = conf;
    // changes和events没有设置就给定默认值512
    ngx_conf_init_uint_value(kcf->changes, 512);
    ngx_conf_init_uint_value(kcf->events, 512);

    return NGX_CONF_OK;
}

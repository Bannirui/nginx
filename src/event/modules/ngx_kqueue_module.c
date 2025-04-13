
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
// 监听队列 用来缓存准备提交给kq还没提交的事件
static struct kevent  *change_list;
// 就绪队列 kq返回用户态的就绪事件集合
static struct kevent  *event_list;
/*
 * max_changes 全局变量 批量注册事件的上限 一次最多向kq注册多少个事件 最大值512 因此change_list队列攒一批待注册事件的上限就是512
 * nchanges 攒了多少个要注册到kq中的事件还没提交给内核 暂存在change_list中 nchanges也就是change_list下一个脚标
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
 * @param timer ms时间 向kq注册个定时器事件 间隔就是这个时间 利用kq实现高精度定时器
 *              NULL表示不需要借助kq实现定时器功能
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
    // 初始化的时候max_changes是static全局变量默认值0 初始化change_list
    if (max_changes < kcf->changes) {
        // nchanges是要注册的变更事件个数 就是change_list长度 nchanges也是static修饰的全局变量默认值0
        if (nchanges) {
            // 初始化的时候nchanges是0 一定会进这个分支
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
            // 这一步骤相当于测试下注册事件系统调用 此时change_list里面是空的 nchanges是0 并没有真正注册事件
            if (kevent(ngx_kqueue, change_list, (int) nchanges, NULL, 0, &ts)
                == -1)
            {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                              "kevent() failed");
                return NGX_ERROR;
            }
            nchanges = 0;
        }

        // change_list用来存储要注册的事件 也是个全局变量 分配好内存
        if (change_list) {
            ngx_free(change_list);
        }
        change_list = ngx_alloc(kcf->changes * sizeof(struct kevent),
                                cycle->log);
        if (change_list == NULL) {
            return NGX_ERROR;
        }
    }
    // kq模块提供的批量注册事件的上限
    max_changes = kcf->changes;
    // 初始化event_list
    if (nevents < kcf->events) {
        if (event_list) {
            ngx_free(event_list);
        }

        event_list = ngx_alloc(kcf->events * sizeof(struct kevent), cycle->log);
        if (event_list == NULL) {
            return NGX_ERROR;
        }
    }
    // 操作指令 一次性事件 支持vnode事件
    ngx_event_flags = NGX_USE_ONESHOT_EVENT
                      |NGX_USE_KQUEUE_EVENT
                      |NGX_USE_VNODE_EVENT;

#if (NGX_HAVE_TIMER_EVENT)
    // 需要借助kq实现高精度定时器 定时器间隔就是timer(ms)
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
        // 全局变量标识使用了定时器
        ngx_event_flags |= NGX_USE_TIMER_EVENT;
    }

#endif

#if (NGX_HAVE_CLEAR_EVENT)
    // 边缘式触发
    ngx_event_flags |= NGX_USE_CLEAR_EVENT;
#else
    // 水平式触发
    ngx_event_flags |= NGX_USE_LEVEL_EVENT;
#endif

#if (NGX_HAVE_LOWAT_EVENT)
    ngx_event_flags |= NGX_USE_LOWAT_EVENT;
#endif

    nevents = kcf->events;

    ngx_io = ngx_os_io;
    // 关于事件的一系列接口 包括监听事件增删改 就绪事件处理 为什么要放到全局变量上 跨平台\多实现 让上层只关注接口不关注具体实现细节
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

/**
 * kq中注册事件和获取就绪事件是同一个系统调用kevent
 * <ul>
 *   <li>通过不同的changelist和eventlist来控制是注册事件还是获取就绪事件</li>
 *   <li>通过不同的flags动作指令达到注册 删除 修改操作</li>
 * </ul>
 * 在kevent上封装一层主义清晰的事件注册增删改接口
 * 注册事件
 * @param flags NGX_FLUSH_EVENT指令控制及时注册到内核
 */
static ngx_int_t
ngx_kqueue_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_int_t          rc;
#if 0
    ngx_event_t       *e;
    ngx_connection_t  *c;
#endif
	/*
	 * 网络事件的读写事件是跟着连接事件跑的
	 * 每个worker进程都有自己的事件循环处理器
	 * 当决定了哪个进程监听对某个端口的连接 那么之后这个端口的读写一定也是注册在那个进程自己的多路复用器上 自然那个端口的读写事件一定由那个进程自己处理
	 * <ul>
	 *   <li>由谁注册监听端口的连接由进程自己抢锁决定</li>
	 *   <li>因为事件循环器是在for循环线程中一直工作的 所以一旦进程抢锁成功注册了某个端口的连接监听 就要标记端口读事件已经在用了 别的进程即使在别的轮次的事件循环中抢到了accept锁 也不要再重复监听这个端口的连接了</li>
	 * </ul>
	 */
    ev->active = 1;
    ev->disabled = 0;
    /*
     * 标识事件是一次性事件
	 */
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
    // 添加到kq监听列表并立即生效
    rc = ngx_kqueue_set_event(ev, event, EV_ADD|EV_ENABLE|flags);

    return rc;
}

/*
 * 删除事件
 * ngx_kqueue_set_event这个方法是nginx对kq的kevent方法的封装 内部用到了change_list
 * 因此在删除事件的时候
 * <ul>
 *   <li>如果要删除的事件还在change_list中 就说明nginx还没有执行kevent提交给内核 直接在数组change_list删除就行</li>
 *   <li>如果在change_list中已经没有 说明要向keven注册个新的删除事件</li>
 * </ul>
 */
static ngx_int_t
ngx_kqueue_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    ngx_int_t     rc;
    ngx_event_t  *e;

    ev->active = 0;
    ev->disabled = 0;
	/*
	 * nginx层面的change_list是个缓存队列 意味着缓存在缓存队列中的事件可能已经被注册到了内核
	 * 所以
	 * <ul>
	 *   <li>index有效 在[0...nchanges)之间 说明事件可能还驻留在change_list缓存队列中</li>
	 *   <li>index无效 不在[0...nchanges)之间 说明事件肯定已经被注册到内核了 而不在change_list中缓存了</li>
	 * </ul>
	 * 经过初步的判断之后就从缓存脚标上拿到事件 比较指针
	 * change_list中存放的是内核kq的事件 从udata上拿到伪地址 把低位抹0拿到真是的nginx事件地址
	 */
    if (ev->index < nchanges
        && ((uintptr_t) change_list[ev->index].udata & (uintptr_t) ~1)
            == (uintptr_t) ev)
    {
        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                       "kevent deleted: %d: ft:%i",
                       ngx_event_ident(ev->data), event);

        /* if the event is still not passed to a kernel we will not pass it */
		/*
		 * 事件并没有真正注册到内核上 从change_list缓存中删除就行
		 * 删除方式也是经典的数组原地删除 数组长度sz
		 * <ul>
		 *   <li>移动数组末脚标达到删除效果 此时数组长度sz-1</li>
		 *   <li>要删除的刚好就是刚才被删除的位置就结束了</li>
		 *   <li>否则就在原来数组[0...sz-2]上多了一个待删除位置 相当于数组空洞 用原来[sz-1]填上这个位置</li>
		 * </ul>
		 */
        nchanges--;

        if (ev->index < nchanges) {
			// 要保留的事件 用这个事件把因为删除产生的数组空洞填上
            e = (ngx_event_t *)
                    ((uintptr_t) change_list[nchanges].udata & (uintptr_t) ~1);
			// 空洞放上要保留的事件
            change_list[ev->index] = change_list[nchanges];
			// 事件在change_list上缓存脚标更新
            e->index = ev->index;
        }

        return NGX_OK;
    }
	// 执行到这说明之前缓存时候的change_list已经被批量注册到了内核 那么现在得再向内核申请一次kevent注册 操作指令为删除事件
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
		// 给kq的操作指令为删除事件
        flags |= EV_DELETE;
    }
	// 调用事件注册
    rc = ngx_kqueue_set_event(ev, event, flags);

    return rc;
}


/*
 * 注册事件 这个注册可能是个延迟注册 需要立即注册到内核需要指定falgs操作指令
 * 对kevent系统调用的封装 因为kevent支持批量提交 因此nginx维护了change_list作缓存实现特定时机的批量提交
 * @param ev nginx封装的事件
 * @param event 监听的事件类型 EVFILT_READ
 * @param flags 操作指令
 *              <ul>
 *                <li>EV_ADD 添加事件</li>
 *                <li>EV_ENABLE 启用事件</li>
 *                <li>EV_ONESHOT 触发一次后自动移除</li>
 *                <li>EV_CLEAR 边缘触发模式</li>
 *                <li>NGX_FLUSH_EVENT 立即注册事件到kq</li>
 *              </ul>
 */
static ngx_int_t
ngx_kqueue_set_event(ngx_event_t *ev, ngx_int_t filter, ngx_uint_t flags)
{
    // kq的事件体 在udata域上存放的是nginx封装的事件体
    struct kevent     *kev;
    struct timespec    ts;
    ngx_connection_t  *c;
    // 连接
    c = ev->data;

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "kevent set event: %d: ft:%i fl:%04Xi",
                   c->fd, filter, flags);
    // kq支持批量注册 当前可能是立即注册可能是懒注册 不管咋样都要把事件先缓存在change_list中 所以先看看缓存满了没有
    if (nchanges >= max_changes) {
        // change_list队列满了 先批量注册到kq 把change_list空出来
        ngx_log_error(NGX_LOG_WARN, ev->log, 0,
                      "kqueue change list is filled up");

        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        // 批量注册
        if (kevent(ngx_kqueue, change_list, (int) nchanges, NULL, 0, &ts)
            == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno, "kevent() failed");
            return NGX_ERROR;
        }
        // 移动change_list的脚标 逻辑上就清空了change_list队列了 可以继续缓存事件了
        nchanges = 0;
    }
    // 把要注册的事件缓存到change_list中
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
    // 记录当前事件在change_list数组的脚标 方便后面快速索引进行修改更新
    ev->index = nchanges;
    // 待注册事件已经缓存到了change_list中 更新当前change_list队列数量
    nchanges++;

    if (flags & NGX_FLUSH_EVENT) {
        /*
         * 这个地方等于是通过NGX_FLUSH_EVENT控制了注册时机
         * <ul>
         *   <li>可以及时注册</li>
         *   <li>可能缓存在change_list中等到下一次调用方指定及时注册</li>
         *   <li>也可能一直等到change_list满了 等到下一次调用时才注册</li>
         * </ul>
         * 所以把控制权交给调用方 对于时延有要求的场景把NGX_FLUSH_EVENT进行立即注册
         */
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
 * 发起一次kq系统调用看看有没有就绪的事件 拿到就绪事件后就做两步处理
 * <ul>
 *   <li>事件打上标识 区分连接事件 读写事件<ul>
 *     <li>连接事件有多少个连接请求进来</li>
 *     <li>读事件有多少内容可读</li>
 *     <li>写事件可以写多少数据</li>
 *   </ul></li>
 *   <li>把事件投递到队列<ul>
 *      <li>连接事件入ngx_posted_accept_events队列</li>
 *      <li>读写事件入ngx_posted_events队列</li>
 *   </ul></li>
 * </ul>
 * @param cycle
 * @param timer 定时器 selector系统调用涉及阻塞 系统提供的API带超时 指定系统调用的超时时间ms
 *              <ul>定时器有2种实现方式
 *                <li>1是借助系统调用超时机制</li>
 *                <li>2是借助kq的定时器事件 这种定时器是高精度定时器</li>
 *              </ul>
 *              定时器的作用是周期性更新系统时间
 * @param flags 这个地方有个细节 NGX_POST_EVENTS的处理
 *              什么时候有这个控制指令什么时候没有
 *              <ul>
 *                <li>单进程下没有事件入队指令 连接事件 读事件 写事件 拿到一个处理一个</li>
 *                <li>多进程模式下</li>
 *              </u>
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
        // 这个标识说明定时器用的是高精度定时器机制 不用再特意设置超时防止kq的kevent方法陷入阻塞出不来 定时器事件一定会保证kevent方法被唤醒的
        tp = NULL;

    } else {
		// nginx并没有在初始化kq的时候指定定时器事件 所以为了保证kevent系统调用不会一直阻塞 要设置个超时时间让方法唤醒
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
     * 因为kevent方法有两个功能 一次系统调用一点也浪费性能干了两件事情
     * <ul>
     *   <li>注册事件</li>
     *   <li>拿到就绪事件</li>
     * </ul>
     * 这也是为什么虽然用了change_list最多也只是延迟懒注册 而不会丢失注册 因为事件循环线程会一直尝试调用kevent拿就绪事件 趁机对change_list缓存事件进行注册
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
		// 拿到伪地址 对应nginx的event和instance防伪码
        ev = (ngx_event_t *) event_list[i].udata;
        // 就绪事件类型 看看是不是读写事件 连接事件也是可写事件 只是可写内容是0而已
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
             *   <li>64位架构地址是64位8Byte对齐 说明指针的后3位是0</li>
             *   <li>最低位被放上了翻转版本号</li>
             * </ul>
             * 所以拿到内核返回的udata
             * <ul>
             *   <li>只要把最低位抹成0就是真正的用户事件地址 nginx封装的通用事件event</li>
             *   <li>只解析最低位的1bit就是翻转版本号 防伪码</li>
             * </ul>
             */
			// 拿到fd的防伪码
            instance = (uintptr_t) ev & 1;
			// 拿到nginx的event
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
				/*
				 * <ul>
				 *   <li>event已经close了说明内核给的fd是僵尸事件 因为在注册事件的时候指定的触发模式是边缘式触发 事件只会触发一次 所以不处理 让事件继续挂在内核监听列表也无所谓</li>
				 *   <li>instance防伪码不一致说明fd是伪事件 那就更不能处理了 后面自然会有fd真正的event</li>
				 * </ul>
				 */
                continue;
            }

            if (ev->log && (ev->log->log_level & NGX_LOG_DEBUG_CONNECTION)) {
                ngx_kqueue_dump_event(ev->log, &event_list[i]);
            }
			// 一次性事件
            if (ev->oneshot) {
                ev->active = 0;
            }
			/**
			 * 记录
			 * 连接事件 有多少个连接请求过来
			 * 可读事件 有多少Byte数据过来 可以read
			 * 可写事件 有多少Byte空间进行write
			 */
            ev->available = event_list[i].data;

            if (event_list[i].flags & EV_EOF) {
                ev->pending_eof = 1;
                ev->kq_errno = event_list[i].fflags;
            }
			// 事件就绪
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
        /*
         * 什么时候会执行到这 两个情况
         * <ul>
         *   <li>1 单进程模式下 只有一个工作进程 拿到任务就同步处理</li>
         *   <li>2 多进程模式下 当前worker进程没有抢到锁</li>
         * </ul>
		 */
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

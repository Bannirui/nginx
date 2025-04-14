
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define DEFAULT_CONNECTIONS  512


extern ngx_module_t ngx_kqueue_module;
extern ngx_module_t ngx_eventport_module;
extern ngx_module_t ngx_devpoll_module;
extern ngx_module_t ngx_epoll_module;
extern ngx_module_t ngx_select_module;


static char *ngx_event_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_event_module_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_event_process_init(ngx_cycle_t *cycle);
static char *ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static void *ngx_event_core_create_conf(ngx_cycle_t *cycle);
static char *ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf);

/*
 * nginx借助向多路复用器注册定时器事件方式实现自由可控的定时机制
 * 当系统有精度很高的定时任务需要管理的时候通过这个高精度定时器机制实现管理
 * 单位是毫秒 每过这么多毫秒定时器事件就就绪 唤醒阻塞等到的kevent系统调用 让事件循环器线程得到一次执行机会 以便依次处理待处理任务
 * <ul>
 *   <li>网络连接任务</li>
 *   <li>定时任务</li>
 *   <li>网络读写任务</li>
 * </ul>
 */
static ngx_uint_t     ngx_timer_resolution;
sig_atomic_t          ngx_event_timer_alarm;

static ngx_uint_t     ngx_event_max_module;

/*
 * 操作指令
 * <ul>
 *   <li>一次性事件</li>
 *   <li>kq还是epoll</li>
 *   <li>支持vnode事件</li>
 *   <li>注册了定时器</li>
 *   <li>边缘式触发</li>
 * </ul>
 */
ngx_uint_t            ngx_event_flags;
// 多路复用器涉及到的api 兼容跨平台 所以nginx为epoll\kq封装了一层 kq模块到时候把真正的实现暴露出来
ngx_event_actions_t   ngx_event_actions;


static ngx_atomic_t   connection_counter = 1;
ngx_atomic_t         *ngx_connection_counter = &connection_counter;


ngx_atomic_t         *ngx_accept_mutex_ptr;
ngx_shmtx_t           ngx_accept_mutex;
/*
 * nginx支持多进程 在多进程模式下 大家都监听在相同端口下 所有工作进程都处于监听状态
 * 当有新的连接进来时 如果没有控制就会导致多个进程同时接收到同一个请求 这种情况被称为竞争接收 可能会导致的问题 连接被多个进程同时处理 最终只有一个进程会成功 其他进程都会失败
 * 为了避免这种情况引入了accept mutex即一个互斥锁 确保在同一时刻只有一个进程能够接收新的连接 其他进程需要等待
 * ngx_use_accept_mutex这个标识就用来标识要不要开启互斥机制
 * <ul>
 *   <li>单进程模式下肯定就不用开启 0标识</li>
 *   <li>多进程模式下需要启用 1标识</li>
 * </ul>
 */
ngx_uint_t            ngx_use_accept_mutex;
ngx_uint_t            ngx_accept_events;
/*
 * 标识当前进程是不是已经抢到了accept锁 什么意思 有什么用处
 * <ul>
 *   <li>首先在多进程下 防止accept惊群 要对accept事件处理进行上锁</li>
 *   <li>然后 如果在进程间发现了负载不均衡 有些进程连接多 有些进程连接少 连接多的满了的进程需要冷静冷静不要再接收新连接 就可以通过放弃抢锁这个行为达到放弃处理accept事件的效果 进而达到负载均衡</li>
 * </ul>
 */
ngx_uint_t            ngx_accept_mutex_held;
// 抢accept锁失败后多久再重试
ngx_msec_t            ngx_accept_mutex_delay;
ngx_int_t             ngx_accept_disabled;
ngx_uint_t            ngx_use_exclusive_accept;


#if (NGX_STAT_STUB)

static ngx_atomic_t   ngx_stat_accepted0;
ngx_atomic_t         *ngx_stat_accepted = &ngx_stat_accepted0;
static ngx_atomic_t   ngx_stat_handled0;
ngx_atomic_t         *ngx_stat_handled = &ngx_stat_handled0;
static ngx_atomic_t   ngx_stat_requests0;
ngx_atomic_t         *ngx_stat_requests = &ngx_stat_requests0;
static ngx_atomic_t   ngx_stat_active0;
ngx_atomic_t         *ngx_stat_active = &ngx_stat_active0;
static ngx_atomic_t   ngx_stat_reading0;
ngx_atomic_t         *ngx_stat_reading = &ngx_stat_reading0;
static ngx_atomic_t   ngx_stat_writing0;
ngx_atomic_t         *ngx_stat_writing = &ngx_stat_writing0;
static ngx_atomic_t   ngx_stat_waiting0;
ngx_atomic_t         *ngx_stat_waiting = &ngx_stat_waiting0;

#endif



static ngx_command_t  ngx_events_commands[] = {

    { ngx_string("events"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_events_block,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_events_module_ctx = {
    ngx_string("events"),
    NULL,
    ngx_event_init_conf
};


ngx_module_t  ngx_events_module = {
    NGX_MODULE_V1,
    &ngx_events_module_ctx,                /* module context */
    ngx_events_commands,                   /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t  event_core_name = ngx_string("event_core");


static ngx_command_t  ngx_event_core_commands[] = {

    { ngx_string("worker_connections"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_connections,
      0,
      0,
      NULL },

    { ngx_string("use"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_use,
      0,
      0,
      NULL },

    { ngx_string("multi_accept"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, multi_accept),
      NULL },

    { ngx_string("accept_mutex"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex),
      NULL },

    { ngx_string("accept_mutex_delay"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex_delay),
      NULL },

    { ngx_string("debug_connection"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_debug_connection,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_event_core_module_ctx = {
    &event_core_name,
    ngx_event_core_create_conf,            /* create configuration */
    ngx_event_core_init_conf,              /* init configuration */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};


ngx_module_t  ngx_event_core_module = {
    NGX_MODULE_V1,
    &ngx_event_core_module_ctx,            /* module context */
    ngx_event_core_commands,               /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    // 负责初始化事件模块需要的全局资源和配置
    ngx_event_module_init,                 /* init module */
    // 事件初始化函数 在nginx工作进程启动后 为工作进程的事件循环做初始化
    ngx_event_process_init,                /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

/**
 * 事件循环器
 * 不管是单进程模式还是多进程模式 区别只是
 * <ul>
 *   <li>单进程下 工作进程执行这个事件循环</li>
 *   <li>多进程下 master进程不参与工作仅仅负责管理worker进程 但是每个worker进程一定要自己参与处理事件循环的</li>
 * </ul>
 * 对于网络连接事件 为了避免惊群现象 在多进程下要进行上锁操作
 */
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    /*
     * 这个标识用来控制就绪事件
     * <ul>
     *   <li>NGX_POST_EVENTS 让事件入队后异步处理</li>
     * </ul>
     */
    ngx_uint_t  flags;
    /*
     * timer定时器
     * 可能的值
     * <ul>
     *   <li>NGX_TIMER_INFINITE</li>
     *   <li>普通任务队列中最近的超时时间</li>
     *   <li>ngx_accept_mutex_delay</li>
     *   <li>0</li>
     * </ul>
     */
    ngx_msec_t  timer, delta;

    if (ngx_timer_resolution) {
        // ngx_timer_resolution标识nginx系统需要高精度定时器 说明当初初始化复用器的时候已经给kq注册了定时器事件 此时调用复用器kevent获取就绪事件就不要指定超时了 阻塞方式调用就行 即使没有网络事件到达内核定时器事件自会唤醒的
        timer = NGX_TIMER_INFINITE;
        flags = 0;

    } else {
		// ngx_timer_resolution没有设置说明nginx没有使用复用器实现定时器 在调用kevent的时候就需要手动指定超时时间防止陷入阻塞导致事件循环器线程唤醒不了 超时多久就看定时任务队列情况了
        timer = ngx_event_find_timer();
        flags = NGX_UPDATE_TIME;

#if (NGX_WIN32)

        /* handle signals from master in case of network inactivity */

        if (timer == NGX_TIMER_INFINITE || timer > 500) {
            timer = 500;
        }

#endif
    }

    if (ngx_use_accept_mutex) {
		// 多进程master-worker模式下 每个worker要进行上锁的 防止网络accept事件惊群 在单进程下不会进到这个分支
        if (ngx_accept_disabled > 0) {
            ngx_accept_disabled--;

        } else {
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {
                return;
            }

            if (ngx_accept_mutex_held) {
				//
                flags |= NGX_POST_EVENTS;

            } else {
                if (timer == NGX_TIMER_INFINITE
                    || timer > ngx_accept_mutex_delay)
                {
                    timer = ngx_accept_mutex_delay;
                }
            }
        }
    }

    if (!ngx_queue_empty(&ngx_posted_next_events)) {
        ngx_event_move_posted_next(cycle);
        timer = 0;
    }

    delta = ngx_current_msec;
    /*
     * <ul>
     *   <li>ngx_process_events是个宏定义</li>
     *   <li>这个宏是接口ngx_event_actions中的方法process_events</li>
     *   <li>至于接口对应的实现是在编译时根据启用的模块进行赋值 kqueue的是ngx_kqueue_module_ctx中的actions</li>
     * </ul>
     * 最终调用到系统的kq 拿到就绪的网络IO事件
     * 能继续执行下去的场景一定是
     * <ul>
     *   <li>虽然没有设置系统调用超时 但是有网络事件就绪</li>
     *   <li>设置了系统调用超时 在超时到期之前就有网络事件就绪</li>
     *   <li>设置了系统调用超时 一直没有网络事件就绪 直到超时到期</li>
     *   <li>在复用器上注册了定时器事件 虽然没有网络事件 但是定时器事件触发了</li>
     * </ul>
     */
    (void) ngx_process_events(cycle, timer, flags);
	/*
	 * 能跳出内核多路复用器执行到这
     * 也就是事件循环器的循环线程得到了一次处理任务的机会 这个时候是一定有任务要处理的
     * <ul>
     *   <li>要么是有网络任务要处理 可能是连接事件任务 可能是读写事件任务</li>
     *   <li>要么是定时任务要处理</li>
     *   <li>要么既有网络事件任务要处理 又有定时任务要处理</li>
     * </ul>
	 */
    delta = ngx_current_msec - delta;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "timer delta: %M", delta);
    // 优先级1 尝试看看有没有网络accept连接任务要处理
    ngx_event_process_posted(cycle, &ngx_posted_accept_events);

    if (ngx_accept_mutex_held) {
		// 为什么在这个地方就要释放锁 因为这个锁的目的就是同步accept事件处理的 处理完了就可以释放了 最小化上锁粒度也是保证系统性能的方式
        ngx_shmtx_unlock(&ngx_accept_mutex);
    }
    /*
     * 优先级2 尝试看看有没有定时任务要处理
     * 执行到这的情况有两种
     * <ul>
     *   <li>系统调用kq指定的超时时间 超时到期了 而这个超时时间就是在系统调用前根据任务队列的到期时间算出来的</li>
     *   <li>系统调用kq没有指定超时时间 让系统调用阻塞执行 但是这种情况会搭配往复用器注册定时器事件来唤醒阻塞线程</li>
     * </ul>
     * 其实不管哪种方式执行到这 都是为了配合系统时间才起作用
     * 执行定时任务的时候的逻辑是拿着任务的过期时间跟当前系统时间比较 到期了就执行
     * 想要获得系统时间就要调用gettimeofday 对于高性能服务器而言 频繁的系统调用是笔很大的开销
     * nginx在性能和定时任务的执行精度做了权衡
     * <ul>
     *   <li>每次都系统调用获取系统时间开销大 时间精确</li>
     *   <li>缓存一个系统时间 每次从内存拿开销小 时间一定会不精准 有滞后</li>
     * </ul>
     * 所以现在的矛盾变成了怎么解决内存上缓存着的时间精度 换言之就是怎么更新缓存的系统时间 所以引申出来的机制就是更新缓存的系统时间的频率就是系统时间的精度
     * 怎么更新系统时间 对应的方式是向复用器注册定时器事件 定时器事件就绪就去更新系统时间
     * 更新系统时间的精度 对应的就是定时器事件的执行间隔 比如设置定时器间隔是100ms 那么每隔100s就会去更新一次缓存的系统时间 也就意味着缓存的系统时间比实时的系统时间滞后最多100ms
     * 意味着当执行定时任务的时候参考的系统时间存在的精度误差导致定时任务的执行精度问题
     * 所以
     * <ul>
     *   <li>如果不在乎定时任务的管理精度 就没必要启用高精度定时器机制</li>
     *   <li>如果需要精细管理定时任务 就可以依赖高精度定时器机制</li>
     * </ul>
     */
    ngx_event_expire_timers();
    // 优先级3 尝试看看有没有理网络IO的读写任务要处理
    ngx_event_process_posted(cycle, &ngx_posted_events);
}


ngx_int_t
ngx_handle_read_event(ngx_event_t *rev, ngx_uint_t flags)
{
#if (NGX_QUIC)

    ngx_connection_t  *c;

    c = rev->data;

    if (c->quic) {
        return NGX_OK;
    }

#endif

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!rev->active && !rev->ready) {
            // 向多路复用器注册事件 监听读 设置为边缘式触发方式 将来配合instance机制防御僵尸事件和伪事件
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_CLEAR_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {

        /* select, poll, /dev/poll */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->active && (rev->ready || (flags & NGX_CLOSE_EVENT))) {
            if (ngx_del_event(rev, NGX_READ_EVENT, NGX_LEVEL_EVENT | flags)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!rev->active && !rev->ready) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (rev->oneshot && rev->ready) {
            if (ngx_del_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}


ngx_int_t
ngx_handle_write_event(ngx_event_t *wev, size_t lowat)
{
    ngx_connection_t  *c;

    c = wev->data;

#if (NGX_QUIC)
    if (c->quic) {
        return NGX_OK;
    }
#endif

    if (lowat) {
        if (ngx_send_lowat(c, lowat) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue, epoll */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT,
                              NGX_CLEAR_EVENT | (lowat ? NGX_LOWAT_EVENT : 0))
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    } else if (ngx_event_flags & NGX_USE_LEVEL_EVENT) {

        /* select, poll, /dev/poll */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->active && wev->ready) {
            if (ngx_del_event(wev, NGX_WRITE_EVENT, NGX_LEVEL_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

    } else if (ngx_event_flags & NGX_USE_EVENTPORT_EVENT) {

        /* event ports */

        if (!wev->active && !wev->ready) {
            if (ngx_add_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (wev->oneshot && wev->ready) {
            if (ngx_del_event(wev, NGX_WRITE_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    /* iocp */

    return NGX_OK;
}

/*
 * event模块的初始化
 * event模块属于core核心模块
 * 会在ngx_cycle中回调到 在ngx_cycle中
 * <ul>
 *   <li>先回调到这 根据监听端口reuse情况决定要不要给后面worker进程复制端口副本 worker进程人手一份</li>
 *   <li>可能是一份监听端口 也可能多份监听端口 创建好socket进行listen</li>
 * </ul>
 * 怎么为worker进程创建监听端口副本呢 假设总共有n个worker进程
 * 在ngx_cycle中回调到这之前 ngx_cycle已经从配置文件中解析了要监听哪些端口 假设80跟81 并切系统是支持端口复用的
 * <ul>
 *   <li>总共有n个worker进程 人手一份 总共需要n份 现在只有一份</li>
 *   <li>n个进程编号依次是[0...n-1]</li>
 *   <li>现在已经有的这份80跟81给0号进程用</li>
 *   <li>再复制3份80跟81出来从1到n-1编号</li>
 *   <li>一起放到cycle的listening数组 那么就总共有n份了</li>
 * </ul>
 */
static char *
ngx_event_init_conf(ngx_cycle_t *cycle, void *conf)
{
#if (NGX_HAVE_REUSEPORT)
    ngx_uint_t        i;
    ngx_core_conf_t  *ccf;
    ngx_listening_t  *ls;
#endif

    if (ngx_get_conf(cycle->conf_ctx, ngx_events_module) == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "no \"events\" section in configuration");
        return NGX_CONF_ERROR;
    }

    if (cycle->connection_n < cycle->listening.nelts + 1) {

        /*
         * there should be at least one connection for each listening
         * socket, plus an additional connection for channel
         */

        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "%ui worker_connections are not enough "
                      "for %ui listening sockets",
                      cycle->connection_n, cycle->listening.nelts);

        return NGX_CONF_ERROR;
    }

#if (NGX_HAVE_REUSEPORT)

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    if (!ngx_test_config && ccf->master) {
		// 多进程模式下
        ls = cycle->listening.elts;
        for (i = 0; i < cycle->listening.nelts; i++) {
			// 端口重用reuseport下才允许为每个worker都复制监听端口 并且已经存在的那份就是给0号进程的
            if (!ls[i].reuseport || ls[i].worker != 0) {
                continue;
            }
			// 除了0号进程外 给其它进程复制一份要监听的端口编上进程索引号放加listening数组
            if (ngx_clone_listening(cycle, &ls[i]) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            /* cloning may change cycle->listening.elts */

            ls = cycle->listening.elts;
        }
    }

#endif

    return NGX_CONF_OK;
}

/**
 * ngx_event_core_module 模块初始化的时候会回调
 * <ul>
 *   <li>互斥锁创建</li>
 * </ul>
 * @param cycle
 * @return 操作状态码
 */
static ngx_int_t
ngx_event_module_init(ngx_cycle_t *cycle)
{
    void              ***cf;
    u_char              *shared;
    size_t               size, cl;
    ngx_shm_t            shm;
    ngx_time_t          *tp;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    /*
     * event模块的配置
     * 得到的cf是void***
     * 解引用得到void** 是一个void*的数组
     * 按照脚标索引到的是void* 具体某种类型的某个模块的配置
     */
    cf = ngx_get_conf(cycle->conf_ctx, ngx_events_module);
    // 配置是用指针指向的void* 强转到event模块的配置
    ecf = (*cf)[ngx_event_core_module.ctx_index];

    if (!ngx_test_config && ngx_process <= NGX_PROCESS_MASTER) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                      "using the \"%s\" event method", ecf->name);
    }
    // nginx core模块的配置
    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    // 定时器的时间精度
    ngx_timer_resolution = ccf->timer_resolution;

#if !(NGX_WIN32)
    {
    ngx_int_t      limit;
    struct rlimit  rlmt;
    // 拿到进程同时打开文件描述符数量
    if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                      "getrlimit(RLIMIT_NOFILE) failed, ignored");

    } else {
        if (ecf->connections > (ngx_uint_t) rlmt.rlim_cur
            && (ccf->rlimit_nofile == NGX_CONF_UNSET
                || ecf->connections > (ngx_uint_t) ccf->rlimit_nofile))
        {
            limit = (ccf->rlimit_nofile == NGX_CONF_UNSET) ?
                         (ngx_int_t) rlmt.rlim_cur : ccf->rlimit_nofile;

            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "%ui worker_connections exceed "
                          "open file resource limit: %i",
                          ecf->connections, limit);
        }
    }
    }
#endif /* !(NGX_WIN32) */


    if (ccf->master == 0) {
        return NGX_OK;
    }

    if (ngx_accept_mutex_ptr) {
        return NGX_OK;
    }


    /* cl should be equal to or greater than cache line size */

    cl = 128;

    size = cl            /* ngx_accept_mutex */
           + cl          /* ngx_connection_counter */
           + cl;         /* ngx_temp_number */

#if (NGX_STAT_STUB)

    size += cl           /* ngx_stat_accepted */
           + cl          /* ngx_stat_handled */
           + cl          /* ngx_stat_requests */
           + cl          /* ngx_stat_active */
           + cl          /* ngx_stat_reading */
           + cl          /* ngx_stat_writing */
           + cl;         /* ngx_stat_waiting */

#endif
    // 共享内存的大小
    shm.size = size;
    // 共享内存的名称
    ngx_str_set(&shm.name, "nginx_shared_zone");
    shm.log = cycle->log;
    // 映射共享内存
    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }
    // 共享内存的起始地址 这个地方放的是该共享内存的互斥锁
    shared = shm.addr;

    ngx_accept_mutex_ptr = (ngx_atomic_t *) shared;
    // 标识互斥锁不是自旋锁
    ngx_accept_mutex.spin = (ngx_uint_t) -1;
    // 互斥锁不是自旋锁 设置一下互斥锁的锁对象
    if (ngx_shmtx_create(&ngx_accept_mutex, (ngx_shmtx_sh_t *) shared,
                         cycle->lock_file.data)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_connection_counter = (ngx_atomic_t *) (shared + 1 * cl);

    (void) ngx_atomic_cmp_set(ngx_connection_counter, 0, 1);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "counter: %p, %uA",
                   ngx_connection_counter, *ngx_connection_counter);

    ngx_temp_number = (ngx_atomic_t *) (shared + 2 * cl);

    tp = ngx_timeofday();

    ngx_random_number = (tp->msec << 16) + ngx_pid;

#if (NGX_STAT_STUB)

    ngx_stat_accepted = (ngx_atomic_t *) (shared + 3 * cl);
    ngx_stat_handled = (ngx_atomic_t *) (shared + 4 * cl);
    ngx_stat_requests = (ngx_atomic_t *) (shared + 5 * cl);
    ngx_stat_active = (ngx_atomic_t *) (shared + 6 * cl);
    ngx_stat_reading = (ngx_atomic_t *) (shared + 7 * cl);
    ngx_stat_writing = (ngx_atomic_t *) (shared + 8 * cl);
    ngx_stat_waiting = (ngx_atomic_t *) (shared + 9 * cl);

#endif

    return NGX_OK;
}


#if !(NGX_WIN32)

static void
ngx_timer_signal_handler(int signo)
{
    ngx_event_timer_alarm = 1;

#if 1
    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, ngx_cycle->log, 0, "timer signal");
#endif
}

#endif

/**
 * 在nginx工作进程启动后 初始化事件模块
 * 在多进程模式下
 * <ul>
 *   <li>如果启用accept锁 在这个方法中是不注册连接事件的 因为开启了accept锁的目的就是为了防止accept惊群 所以注册连接事件为后置到事件循环中先抢锁 抢到再注册</li>
 *   <li>如果没有启用accept锁 在这个方法会给每个worker进程都注册连接事件 将来可能会引起accept惊群</li>
 * </ul>
 */
static ngx_int_t
ngx_event_process_init(ngx_cycle_t *cycle)
{
    ngx_uint_t           m, i;
    ngx_event_t         *rev, *wev;
    ngx_listening_t     *ls;
    ngx_connection_t    *c, *next, *old;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    ngx_event_module_t  *module;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    ecf = ngx_event_get_conf(cycle->conf_ctx, ngx_event_core_module);

    if (ccf->master && ccf->worker_processes > 1 && ecf->accept_mutex) {
        /*
         * 多进程模式下开启竞争接收
         * 为什么要对网络连接进行互斥 在多进程下每个worker进程都有自己的循环处理 如果不对连接进行互斥 就意味着同一时刻多个进程同时进行accept操作 结果是只有一个进程能执行accept成功 其他都失败
         * 这就是accept引起了惊群
         * 所以要对accept操作进行上锁
         * 为什么其他读写事件任务不需要加锁呢 因为每个进程有自己的事件循环 accept后会将事件注册在各自的事件循环器 所以将来对应的读写事件也只有自己处理 不存在竞争问题
		 */
        ngx_use_accept_mutex = 1;
		// 这个函数调用时机是在worker进程创建好后 所以此时工作进程初始化占锁标识为0 标识没有抢到accept锁
        ngx_accept_mutex_held = 0;
        ngx_accept_mutex_delay = ecf->accept_mutex_delay;

    } else {
        // 单进程模式下不需要开启竞争接收
        ngx_use_accept_mutex = 0;
    }

#if (NGX_WIN32)

    /*
     * disable accept mutex on win32 as it may cause deadlock if
     * grabbed by a process which can't accept connections
     */

    ngx_use_accept_mutex = 0;

#endif

    ngx_use_exclusive_accept = 0;

    ngx_queue_init(&ngx_posted_accept_events);
    ngx_queue_init(&ngx_posted_next_events);
    ngx_queue_init(&ngx_posted_events);

    if (ngx_event_timer_init(cycle->log) == NGX_ERROR) {
        return NGX_ERROR;
    }

    for (m = 0; cycle->modules[m]; m++) {
        if (cycle->modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        if (cycle->modules[m]->ctx_index != ecf->use) {
            continue;
        }

        module = cycle->modules[m]->ctx;

        if (module->actions.init(cycle, ngx_timer_resolution) != NGX_OK) {
            /* fatal */
            exit(2);
        }

        break;
    }

#if !(NGX_WIN32)

    if (ngx_timer_resolution && !(ngx_event_flags & NGX_USE_TIMER_EVENT)) {
        struct sigaction  sa;
        struct itimerval  itv;

        ngx_memzero(&sa, sizeof(struct sigaction));
        sa.sa_handler = ngx_timer_signal_handler;
        sigemptyset(&sa.sa_mask);

        if (sigaction(SIGALRM, &sa, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "sigaction(SIGALRM) failed");
            return NGX_ERROR;
        }

        itv.it_interval.tv_sec = ngx_timer_resolution / 1000;
        itv.it_interval.tv_usec = (ngx_timer_resolution % 1000) * 1000;
        itv.it_value.tv_sec = ngx_timer_resolution / 1000;
        itv.it_value.tv_usec = (ngx_timer_resolution % 1000 ) * 1000;

        if (setitimer(ITIMER_REAL, &itv, NULL) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "setitimer() failed");
        }
    }

    if (ngx_event_flags & NGX_USE_FD_EVENT) {
        struct rlimit  rlmt;

        if (getrlimit(RLIMIT_NOFILE, &rlmt) == -1) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno,
                          "getrlimit(RLIMIT_NOFILE) failed");
            return NGX_ERROR;
        }

        cycle->files_n = (ngx_uint_t) rlmt.rlim_cur;

        cycle->files = ngx_calloc(sizeof(ngx_connection_t *) * cycle->files_n,
                                  cycle->log);
        if (cycle->files == NULL) {
            return NGX_ERROR;
        }
    }

#else

    if (ngx_timer_resolution && !(ngx_event_flags & NGX_USE_TIMER_EVENT)) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "the \"timer_resolution\" directive is not supported "
                      "with the configured event method, ignored");
        ngx_timer_resolution = 0;
    }

#endif

    cycle->connections =
        ngx_alloc(sizeof(ngx_connection_t) * cycle->connection_n, cycle->log);
    if (cycle->connections == NULL) {
        return NGX_ERROR;
    }

    c = cycle->connections;

    cycle->read_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                   cycle->log);
    if (cycle->read_events == NULL) {
        return NGX_ERROR;
    }

    rev = cycle->read_events;
    for (i = 0; i < cycle->connection_n; i++) {
        rev[i].closed = 1;
        rev[i].instance = 1;
    }

    cycle->write_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                    cycle->log);
    if (cycle->write_events == NULL) {
        return NGX_ERROR;
    }

    wev = cycle->write_events;
    for (i = 0; i < cycle->connection_n; i++) {
        wev[i].closed = 1;
    }

    i = cycle->connection_n;
    next = NULL;

    do {
        i--;

        c[i].data = next;
        c[i].read = &cycle->read_events[i];
        c[i].write = &cycle->write_events[i];
        c[i].fd = (ngx_socket_t) -1;

        next = &c[i];
    } while (i);

    cycle->free_connections = next;
    cycle->free_connection_n = cycle->connection_n;

    /* for each listening socket */

    ls = cycle->listening.elts;
	/*
	 * 在worker进程启动后会加调到这儿 下面要对监听端口注册连接事件
	 */
    for (i = 0; i < cycle->listening.nelts; i++) {

#if (NGX_HAVE_REUSEPORT)
        if (ls[i].reuseport && ls[i].worker != ngx_worker) {
			// 在reuseport情况下 listening数组中放了所有worker的监听副本 用worker进程编号标识归属 每个worker进程只处理属于自己的端口
            continue;
        }
#endif

        c = ngx_get_connection(ls[i].fd, cycle->log);

        if (c == NULL) {
            return NGX_ERROR;
        }

        c->type = ls[i].type;
        c->log = &ls[i].log;

        c->listening = &ls[i];
        ls[i].connection = c;

        rev = c->read;

        rev->log = c->log;
        rev->accept = 1;

#if (NGX_HAVE_DEFERRED_ACCEPT)
        rev->deferred_accept = ls[i].deferred_accept;
#endif

        if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)
            && cycle->old_cycle)
        {
            if (ls[i].previous) {

                /*
                 * delete the old accept events that were bound to
                 * the old cycle read events array
                 */

                old = ls[i].previous->connection;

                if (ngx_del_event(old->read, NGX_READ_EVENT, NGX_CLOSE_EVENT)
                    == NGX_ERROR)
                {
                    return NGX_ERROR;
                }

                old->fd = (ngx_socket_t) -1;
            }
        }

#if (NGX_WIN32)

        if (ngx_event_flags & NGX_USE_IOCP_EVENT) {
            ngx_iocp_conf_t  *iocpcf;

            rev->handler = ngx_event_acceptex;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, 0, NGX_IOCP_ACCEPT) == NGX_ERROR) {
                return NGX_ERROR;
            }

            ls[i].log.handler = ngx_acceptex_log_error;

            iocpcf = ngx_event_get_conf(cycle->conf_ctx, ngx_iocp_module);
            if (ngx_event_post_acceptex(&ls[i], iocpcf->post_acceptex)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

        } else {
            rev->handler = ngx_event_accept;

            if (ngx_use_accept_mutex) {
                continue;
            }

            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

#else

        if (c->type == SOCK_STREAM) {
            rev->handler = ngx_event_accept;

#if (NGX_QUIC)
        } else if (ls[i].quic) {
            rev->handler = ngx_quic_recvmsg;
#endif
        } else {
            rev->handler = ngx_event_recvmsg;
        }

#if (NGX_HAVE_REUSEPORT)

        if (ls[i].reuseport) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            continue;
        }

#endif

        if (ngx_use_accept_mutex) {
			// 下面的逻辑是向多复用器注册连接事件 如果启用了accept互斥锁就不是每个worker进程启动后就注册连接事件而是把注册动作后置到事件循环中
            continue;
        }

#if (NGX_HAVE_EPOLLEXCLUSIVE)

        if ((ngx_event_flags & NGX_USE_EPOLL_EVENT)
            && ccf->worker_processes > 1)
        {
            ngx_use_exclusive_accept = 1;

            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_EXCLUSIVE_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            continue;
        }

#endif
		// worker进程注册对端口的连接事件监听注册到多路复用器上 这个时机在worker进程启动后就注册 上面有个判断是不是启用了accept锁 如果没有启动互斥锁 每个进程启动后就开始注册连接事件 这种方式可能会引起accept惊群
        if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

#endif

    }

    return NGX_OK;
}


ngx_int_t
ngx_send_lowat(ngx_connection_t *c, size_t lowat)
{
    int  sndlowat;

#if (NGX_HAVE_LOWAT_EVENT)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        c->write->available = lowat;
        return NGX_OK;
    }

#endif

    if (lowat == 0 || c->sndlowat) {
        return NGX_OK;
    }

    sndlowat = (int) lowat;

    if (setsockopt(c->fd, SOL_SOCKET, SO_SNDLOWAT,
                   (const void *) &sndlowat, sizeof(int))
        == -1)
    {
        ngx_connection_error(c, ngx_socket_errno,
                             "setsockopt(SO_SNDLOWAT) failed");
        return NGX_ERROR;
    }

    c->sndlowat = 1;

    return NGX_OK;
}


static char *
ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                 *rv;
    void               ***ctx;
    ngx_uint_t            i;
    ngx_conf_t            pcf;
    ngx_event_module_t   *m;

    if (*(void **) conf) {
        return "is duplicate";
    }

    /* count the number of the event modules and set up their indices */

    ngx_event_max_module = ngx_count_modules(cf->cycle, NGX_EVENT_MODULE);

    ctx = ngx_pcalloc(cf->pool, sizeof(void *));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *ctx = ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *));
    if (*ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *(void **) conf = ctx;

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;

        if (m->create_conf) {
            (*ctx)[cf->cycle->modules[i]->ctx_index] =
                                                     m->create_conf(cf->cycle);
            if ((*ctx)[cf->cycle->modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    pcf = *cf;
    cf->ctx = ctx;
    cf->module_type = NGX_EVENT_MODULE;
    cf->cmd_type = NGX_EVENT_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = pcf;

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;

        if (m->init_conf) {
            rv = m->init_conf(cf->cycle,
                              (*ctx)[cf->cycle->modules[i]->ctx_index]);
            if (rv != NGX_CONF_OK) {
                return rv;
            }
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_event_connections(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_str_t  *value;

    if (ecf->connections != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    value = cf->args->elts;
    ecf->connections = ngx_atoi(value[1].data, value[1].len);
    if (ecf->connections == (ngx_uint_t) NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number \"%V\"", &value[1]);

        return NGX_CONF_ERROR;
    }

    cf->cycle->connection_n = ecf->connections;

    return NGX_CONF_OK;
}


static char *
ngx_event_use(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             m;
    ngx_str_t            *value;
    ngx_event_conf_t     *old_ecf;
    ngx_event_module_t   *module;

    if (ecf->use != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->cycle->old_cycle->conf_ctx) {
        old_ecf = ngx_event_get_conf(cf->cycle->old_cycle->conf_ctx,
                                     ngx_event_core_module);
    } else {
        old_ecf = NULL;
    }


    for (m = 0; cf->cycle->modules[m]; m++) {
        if (cf->cycle->modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        module = cf->cycle->modules[m]->ctx;
        if (module->name->len == value[1].len) {
            if (ngx_strcmp(module->name->data, value[1].data) == 0) {
                ecf->use = cf->cycle->modules[m]->ctx_index;
                ecf->name = module->name->data;

                if (ngx_process == NGX_PROCESS_SINGLE
                    && old_ecf
                    && old_ecf->use != ecf->use)
                {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "when the server runs without a master process "
                               "the \"%V\" event type must be the same as "
                               "in previous configuration - \"%s\" "
                               "and it cannot be changed on the fly, "
                               "to change it you need to stop server "
                               "and start it again",
                               &value[1], old_ecf->name);

                    return NGX_CONF_ERROR;
                }

                return NGX_CONF_OK;
            }
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid event type \"%V\"", &value[1]);

    return NGX_CONF_ERROR;
}


static char *
ngx_event_debug_connection(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
#if (NGX_DEBUG)
    ngx_event_conf_t  *ecf = conf;

    ngx_int_t             rc;
    ngx_str_t            *value;
    ngx_url_t             u;
    ngx_cidr_t            c, *cidr;
    ngx_uint_t            i;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    value = cf->args->elts;

#if (NGX_HAVE_UNIX_DOMAIN)

    if (ngx_strcmp(value[1].data, "unix:") == 0) {
        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        cidr->family = AF_UNIX;
        return NGX_CONF_OK;
    }

#endif

    rc = ngx_ptocidr(&value[1], &c);

    if (rc != NGX_ERROR) {
        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "low address bits of %V are meaningless",
                               &value[1]);
        }

        cidr = ngx_array_push(&ecf->debug_connection);
        if (cidr == NULL) {
            return NGX_CONF_ERROR;
        }

        *cidr = c;

        return NGX_CONF_OK;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.host = value[1];

    if (ngx_inet_resolve_host(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in debug_connection \"%V\"",
                               u.err, &u.host);
        }

        return NGX_CONF_ERROR;
    }

    cidr = ngx_array_push_n(&ecf->debug_connection, u.naddrs);
    if (cidr == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(cidr, u.naddrs * sizeof(ngx_cidr_t));

    for (i = 0; i < u.naddrs; i++) {
        cidr[i].family = u.addrs[i].sockaddr->sa_family;

        switch (cidr[i].family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) u.addrs[i].sockaddr;
            cidr[i].u.in6.addr = sin6->sin6_addr;
            ngx_memset(cidr[i].u.in6.mask.s6_addr, 0xff, 16);
            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) u.addrs[i].sockaddr;
            cidr[i].u.in.addr = sin->sin_addr.s_addr;
            cidr[i].u.in.mask = 0xffffffff;
            break;
        }
    }

#else

    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "\"debug_connection\" is ignored, you need to rebuild "
                       "nginx using --with-debug option to enable it");

#endif

    return NGX_CONF_OK;
}


static void *
ngx_event_core_create_conf(ngx_cycle_t *cycle)
{
    ngx_event_conf_t  *ecf;

    ecf = ngx_palloc(cycle->pool, sizeof(ngx_event_conf_t));
    if (ecf == NULL) {
        return NULL;
    }

    ecf->connections = NGX_CONF_UNSET_UINT;
    ecf->use = NGX_CONF_UNSET_UINT;
    ecf->multi_accept = NGX_CONF_UNSET;
    ecf->accept_mutex = NGX_CONF_UNSET;
    ecf->accept_mutex_delay = NGX_CONF_UNSET_MSEC;
    ecf->name = (void *) NGX_CONF_UNSET;

#if (NGX_DEBUG)

    if (ngx_array_init(&ecf->debug_connection, cycle->pool, 4,
                       sizeof(ngx_cidr_t)) == NGX_ERROR)
    {
        return NULL;
    }

#endif

    return ecf;
}


static char *
ngx_event_core_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_event_conf_t  *ecf = conf;

#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)
    int                  fd;
#endif
    ngx_int_t            i;
    ngx_module_t        *module;
    ngx_event_module_t  *event_module;

    module = NULL;

#if (NGX_HAVE_EPOLL) && !(NGX_TEST_BUILD_EPOLL)

    fd = epoll_create(100);

    if (fd != -1) {
        (void) close(fd);
        module = &ngx_epoll_module;

    } else if (ngx_errno != NGX_ENOSYS) {
        module = &ngx_epoll_module;
    }

#endif

#if (NGX_HAVE_DEVPOLL) && !(NGX_TEST_BUILD_DEVPOLL)

    module = &ngx_devpoll_module;

#endif

#if (NGX_HAVE_KQUEUE)

    module = &ngx_kqueue_module;

#endif

#if (NGX_HAVE_SELECT)

    if (module == NULL) {
        module = &ngx_select_module;
    }

#endif

    if (module == NULL) {
        for (i = 0; cycle->modules[i]; i++) {

            if (cycle->modules[i]->type != NGX_EVENT_MODULE) {
                continue;
            }

            event_module = cycle->modules[i]->ctx;

            if (ngx_strcmp(event_module->name->data, event_core_name.data) == 0)
            {
                continue;
            }

            module = cycle->modules[i];
            break;
        }
    }

    if (module == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "no events module found");
        return NGX_CONF_ERROR;
    }

    ngx_conf_init_uint_value(ecf->connections, DEFAULT_CONNECTIONS);
    cycle->connection_n = ecf->connections;

    ngx_conf_init_uint_value(ecf->use, module->ctx_index);

    event_module = module->ctx;
    ngx_conf_init_ptr_value(ecf->name, event_module->name->data);

    ngx_conf_init_value(ecf->multi_accept, 0);
	/*
	 * 配置文件中
	 * events {
	 * accept_mutex on;  # 默认就是 on
	 * worker_connections 1024;
     * }
     * 配置了开启accept锁就会开启
     * 没有配置就用默认值 关闭accept锁
	 */
    ngx_conf_init_value(ecf->accept_mutex, 0);
    ngx_conf_init_msec_value(ecf->accept_mutex_delay, 500);

    return NGX_CONF_OK;
}

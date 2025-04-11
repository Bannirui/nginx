
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static ngx_msec_t ngx_monotonic_time(time_t sec, ngx_uint_t msec);


/*
 * The time may be updated by signal handler or by several threads.
 * The time update operations are rare and require to hold the ngx_time_lock.
 * The time read operations are frequent, so they are lock-free and get time
 * values and strings from the current slot.  Thus thread may get the corrupted
 * values only if it is preempted while copying and then it is not scheduled
 * to run more than NGX_TIME_SLOTS seconds.
 */

#define NGX_TIME_SLOTS   64
/*
 * cached_time数组 指向的是当前缓存的最新的系统时间 数组脚标移动实现环形数组
 * 这个slot可以理解成只给写线程用的写指针
 * 读线程不会直接用这个指针 读线程用的是ngx_cached_time
 */
static ngx_uint_t        slot;
static ngx_atomic_t      ngx_time_lock;
/*
 * 缓存的系统时间 格式是毫秒 语义是这个时间表达的是系统启动后x毫秒 是个相对系统启动的相对时间
 * 是通过clock_gettime得到的单调时间
 * 为什么下面的ngx_cached_time要用环形设计 而这个系统时间不需要环形设计而只要一个变量
 * <ul>
 *   <li>首先ngx_current_msec就是个整数 而ngx_cached_time是结构化的时间 有多个结构体成员</li>
 *   <li>其次系统对于long读写是原子的 不存在并发不安全问题</li>
 * </ul>
 * 这个时间的唯一作用就是判断定时任务是不是到期该执行了 这个使用场景决定了
 * 必须单调 不能出现时间倒退导致对定时任务的误判
 * 为什么结构化时间缓存用的是环形数组形式 而单调时间用的就是一个变量
 * <ul>
 *   <li>首先 语义是就不同 单调时间就是一个很明确的数字</li>
 *   <li>第二 类型 它就是一个整型 不存在更新期间读到一半新数据 一半老数据</li>
 * </ul>
 */
volatile ngx_msec_t      ngx_current_msec;
/*
 * 下面这几个都是给读线程直接用的 目的就直接读到缓存的最新的系统时间
 * ngx_cached_time比较特殊 它是一个指针 本质就是cached_time环形数组的读指针
 * <ul>
 *   <li>读操作的是ngx_cached_time 无锁</li>
 *   <li>写操作的是slot 需要竞争锁 互斥操作 写完移动读指针到最新位置</li>
 * </ul>
 */
volatile ngx_time_t     *ngx_cached_time;
volatile ngx_str_t       ngx_cached_err_log_time;
volatile ngx_str_t       ngx_cached_http_time;
volatile ngx_str_t       ngx_cached_http_log_time;
volatile ngx_str_t       ngx_cached_http_log_iso8601;
volatile ngx_str_t       ngx_cached_syslog_time;

#if !(NGX_WIN32)

/*
 * localtime() and localtime_r() are not Async-Signal-Safe functions, therefore,
 * they must not be called by a signal handler, so we use the cached
 * GMT offset value. Fortunately the value is changed only two times a year.
 */

static ngx_int_t         cached_gmtoff;
#endif
/*
 * 系统时间缓冲区
 * 缓存时间 减少系统调用 数组长度通过宏NGX_TIME_SLOTS控制 长度64
 * 首先为什么把缓冲区设计成环形 这样设计的目的是为了保证有锁单线程写 无锁多进程读
 * 既然环形的设计目的是为了解耦读写操作 那么是不是只要保证缓冲区大小是2就行 写操作一直在两个位置轮流交替
 * 长度2不是不可以但是存在的隐患是读时被写覆盖
 * 假设A在读系统时间 拿到的指针是1
 * 在A读期间B发生了多次对系统时间的更新
 *   - B更新系统时间 指针1
 *   - B更新系统时间 指针2
 *   - B更新系统时间 指针1
 *   - ...
 * 也就是意味着A使用的系统时间已经发生了更新 原来的值被覆盖了 这种场景可能导致误判
 * 所以本质问题就是最好留给更新操作多点时间缓冲 用空间来换 只要环形缓冲区大一点 就足够避免上面的事情发生
 * 可能这就是作者设计缓冲区默认长度64的原因
 */
static ngx_time_t        cached_time[NGX_TIME_SLOTS];
static u_char            cached_err_log_time[NGX_TIME_SLOTS]
                                    [sizeof("1970/09/28 12:00:00")];
static u_char            cached_http_time[NGX_TIME_SLOTS]
                                    [sizeof("Mon, 28 Sep 1970 06:00:00 GMT")];
static u_char            cached_http_log_time[NGX_TIME_SLOTS]
                                    [sizeof("28/Sep/1970:12:00:00 +0600")];
static u_char            cached_http_log_iso8601[NGX_TIME_SLOTS]
                                    [sizeof("1970-09-28T12:00:00+06:00")];
static u_char            cached_syslog_time[NGX_TIME_SLOTS]
                                    [sizeof("Sep 28 12:00:00")];


static char  *week[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static char  *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void
ngx_time_init(void)
{
    ngx_cached_err_log_time.len = sizeof("1970/09/28 12:00:00") - 1;
    ngx_cached_http_time.len = sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1;
    ngx_cached_http_log_time.len = sizeof("28/Sep/1970:12:00:00 +0600") - 1;
    ngx_cached_http_log_iso8601.len = sizeof("1970-09-28T12:00:00+06:00") - 1;
    ngx_cached_syslog_time.len = sizeof("Sep 28 12:00:00") - 1;

    ngx_cached_time = &cached_time[0];

    ngx_time_update();
}

/**
 * 更新系统时间的缓存 更新两个地方
 * <ul>
 *   <li>ngx_current_msec 单调的开机毫秒时长 单个变量</li>
 *   <li>cached_time 结构化格式 环形缓冲区</li>
 * </ul>
 * 为什么这么做呢 因为对于高性能服务器如果每次都调用gettimeofday会严重影响性能
 * 什么时机才会调用这个方法更新系统时间呢
 * <ul>
 *   <li>每次事件循环时</li>
 *   <li>使用高精度定时器时 kqueue/epoll返回前或后</li>
 *   <li>写日志时</li>
 *   <li>输出HTTP时间头</li>
 * </ul>
 */
void
ngx_time_update(void)
{
    u_char          *p0, *p1, *p2, *p3, *p4;
    ngx_tm_t         tm, gmt;
    time_t           sec;
    ngx_uint_t       msec;
    ngx_time_t      *tp;
    struct timeval   tv;

    if (!ngx_trylock(&ngx_time_lock)) {
        return;
    }
    // 获取系统当前时间
    ngx_gettimeofday(&tv);
    // 系统时间的s
    sec = tv.tv_sec;
    // 系统时间的毫秒
    msec = tv.tv_usec / 1000;
    // 更新缓存时间 单调时间
    ngx_current_msec = ngx_monotonic_time(sec, msec);
    /*
     * 正常情况下更新时间的步骤是
     * <ul>
     *   <li>数组维护的最新的时间在slot上 slot是直接给写线程使用的 对应这个slot位置的是ngx_cached_time给读线程使用的</li>
     *   <li>拿到逻辑上的下一个位置 slot等于0或slot+1</li>
     *   <li>将新时间写到缓存上</li>
     *   <li>写完后更新读指针ngx_cached_time</li>
     *   <li>释放写锁</li>
     * </ul>
     * 为什么要上写锁 为了保护写这个资源的原子性 为什么呢 因为要这是个结构体要写秒和毫秒两个成员
     * 那是不是如果只写一个long型数字就可以不用锁 天然原子性
     * 所以并没有直接去更新到下一个位置上 而是看下秒级没变 那就只用更新毫秒 只更新毫秒就是只更新一个long字段 不怕无锁读的地方读到更新一半这种情况
     */
    tp = &cached_time[slot];

    if (tp->sec == sec) {
        // 只更新毫秒这个字段 这样做的的目的是为了快速返回 减少持锁时长 减少写并发的锁竞争
        tp->msec = msec;
        ngx_unlock(&ngx_time_lock);
        return;
    }
    /*
     * 什么时候执行到这 当前的系统时间跟最近缓存的系统时间差异超过了秒级
     * 也就是说明接下来要更新的是两个字段 秒和毫秒
     * 那为什么不直接更新当前缓冲区位置 而要更新下一个缓冲区位置呢
     * 因为此时要更新的不是一个字段 而是两个字段 不保证原子性的话 读的地方可能读到更新一半的数据
     * 假设我们直接修改当前槽slot不变
     * <ul>
     *   <li>此时某个worker正在读取 读线程用的是ngx_cached_time指针在读 而这个指针指向的真是旧的slot</li>
     *   <li>同时ngx_time_update()正在覆盖这个槽 更新新的sec和msec</li>
     *   <li>中间态可能发生 读到一半旧时间 一半新时间 例如tp->sec=新时间 tp->msec=旧值</li>
     * </ul>
     * 这就会导致时间错乱 日志错乱 甚至逻辑bug
     * 所以要去更新下一个槽 等写完了再移动ngx_cached_time到最新的槽上
     */
    if (slot == NGX_TIME_SLOTS - 1) {
        slot = 0;
    } else {
        slot++;
    }
    // 开始更新秒和毫秒两个值
    tp = &cached_time[slot];

    tp->sec = sec;
    tp->msec = msec;
    // 时间格式转换 秒转年月日时分秒
    ngx_gmtime(sec, &gmt);


    p0 = &cached_http_time[slot][0];

    (void) ngx_sprintf(p0, "%s, %02d %s %4d %02d:%02d:%02d GMT",
                       week[gmt.ngx_tm_wday], gmt.ngx_tm_mday,
                       months[gmt.ngx_tm_mon - 1], gmt.ngx_tm_year,
                       gmt.ngx_tm_hour, gmt.ngx_tm_min, gmt.ngx_tm_sec);

#if (NGX_HAVE_GETTIMEZONE)

    tp->gmtoff = ngx_gettimezone();
    ngx_gmtime(sec + tp->gmtoff * 60, &tm);

#elif (NGX_HAVE_GMTOFF)

    ngx_localtime(sec, &tm);
    cached_gmtoff = (ngx_int_t) (tm.ngx_tm_gmtoff / 60);
    tp->gmtoff = cached_gmtoff;

#else

    ngx_localtime(sec, &tm);
    cached_gmtoff = ngx_timezone(tm.ngx_tm_isdst);
    tp->gmtoff = cached_gmtoff;

#endif


    p1 = &cached_err_log_time[slot][0];

    (void) ngx_sprintf(p1, "%4d/%02d/%02d %02d:%02d:%02d",
                       tm.ngx_tm_year, tm.ngx_tm_mon,
                       tm.ngx_tm_mday, tm.ngx_tm_hour,
                       tm.ngx_tm_min, tm.ngx_tm_sec);


    p2 = &cached_http_log_time[slot][0];

    (void) ngx_sprintf(p2, "%02d/%s/%d:%02d:%02d:%02d %c%02i%02i",
                       tm.ngx_tm_mday, months[tm.ngx_tm_mon - 1],
                       tm.ngx_tm_year, tm.ngx_tm_hour,
                       tm.ngx_tm_min, tm.ngx_tm_sec,
                       tp->gmtoff < 0 ? '-' : '+',
                       ngx_abs(tp->gmtoff / 60), ngx_abs(tp->gmtoff % 60));

    p3 = &cached_http_log_iso8601[slot][0];

    (void) ngx_sprintf(p3, "%4d-%02d-%02dT%02d:%02d:%02d%c%02i:%02i",
                       tm.ngx_tm_year, tm.ngx_tm_mon,
                       tm.ngx_tm_mday, tm.ngx_tm_hour,
                       tm.ngx_tm_min, tm.ngx_tm_sec,
                       tp->gmtoff < 0 ? '-' : '+',
                       ngx_abs(tp->gmtoff / 60), ngx_abs(tp->gmtoff % 60));

    p4 = &cached_syslog_time[slot][0];

    (void) ngx_sprintf(p4, "%s %2d %02d:%02d:%02d",
                       months[tm.ngx_tm_mon - 1], tm.ngx_tm_mday,
                       tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec);
    // gcc内存屏障 保证cpu读写顺序 防止指令重排
    ngx_memory_barrier();
    // 已经更新好了最新的时间 此时ngx_cached_time读指针还指向在旧的槽上 更新读指针到最新位置
    ngx_cached_time = tp;
    ngx_cached_http_time.data = p0;
    ngx_cached_err_log_time.data = p1;
    ngx_cached_http_log_time.data = p2;
    ngx_cached_http_log_iso8601.data = p3;
    ngx_cached_syslog_time.data = p4;

    ngx_unlock(&ngx_time_lock);
}

/*
 * 依赖系统调用clock_gettime
 * 这个系统调用对比gettimeofday有什么区别
 * <ul>
 *   <li>gettimeofday的精度是us clock_gettime的精度是ns</li>
 *   <li>gettimeofday是墙上时间也就是系统时间 是可以被管理员修改的 CLOCK_MONOTONIC是自系统启动后过了多久 是一个相对时间</li>
 *   <li>gettimeofday可能出现时间倒退 t1=gettimeofday() t2=gettimeofday() 因为系统时间被修改导致t2可能比t1小 而CLOCK_MONOTONIC是单调时间</li>
 * </ul>
 * 对于定时任务这种肯定是不希望出现时间倒退的 时间倒退可能出现误判导致重复执行/漏执行某些定时任务
 * 所以在定时任务和时间比较的场景肯定是用单调时间的
 *
 * @return 系统时间ms格式时间戳 语义是当前时间是系统启动后x毫秒
 */
static ngx_msec_t
ngx_monotonic_time(time_t sec, ngx_uint_t msec)
{
#if (NGX_HAVE_CLOCK_MONOTONIC)
    struct timespec  ts;

#if defined(CLOCK_MONOTONIC_FAST)
    clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    // 系统开机后多少秒
    sec = ts.tv_sec;
    // 系统开机后多少毫秒
    msec = ts.tv_nsec / 1000000;

#endif

    return (ngx_msec_t) sec * 1000 + msec;
}


#if !(NGX_WIN32)

void
ngx_time_sigsafe_update(void)
{
    u_char          *p, *p2;
    ngx_tm_t         tm;
    time_t           sec;
    ngx_time_t      *tp;
    struct timeval   tv;

    if (!ngx_trylock(&ngx_time_lock)) {
        return;
    }

    ngx_gettimeofday(&tv);

    sec = tv.tv_sec;

    tp = &cached_time[slot];

    if (tp->sec == sec) {
        ngx_unlock(&ngx_time_lock);
        return;
    }

    if (slot == NGX_TIME_SLOTS - 1) {
        slot = 0;
    } else {
        slot++;
    }

    tp = &cached_time[slot];

    tp->sec = 0;

    ngx_gmtime(sec + cached_gmtoff * 60, &tm);

    p = &cached_err_log_time[slot][0];

    (void) ngx_sprintf(p, "%4d/%02d/%02d %02d:%02d:%02d",
                       tm.ngx_tm_year, tm.ngx_tm_mon,
                       tm.ngx_tm_mday, tm.ngx_tm_hour,
                       tm.ngx_tm_min, tm.ngx_tm_sec);

    p2 = &cached_syslog_time[slot][0];

    (void) ngx_sprintf(p2, "%s %2d %02d:%02d:%02d",
                       months[tm.ngx_tm_mon - 1], tm.ngx_tm_mday,
                       tm.ngx_tm_hour, tm.ngx_tm_min, tm.ngx_tm_sec);

    ngx_memory_barrier();

    ngx_cached_err_log_time.data = p;
    ngx_cached_syslog_time.data = p2;

    ngx_unlock(&ngx_time_lock);
}

#endif


u_char *
ngx_http_time(u_char *buf, time_t t)
{
    ngx_tm_t  tm;

    ngx_gmtime(t, &tm);

    return ngx_sprintf(buf, "%s, %02d %s %4d %02d:%02d:%02d GMT",
                       week[tm.ngx_tm_wday],
                       tm.ngx_tm_mday,
                       months[tm.ngx_tm_mon - 1],
                       tm.ngx_tm_year,
                       tm.ngx_tm_hour,
                       tm.ngx_tm_min,
                       tm.ngx_tm_sec);
}


u_char *
ngx_http_cookie_time(u_char *buf, time_t t)
{
    ngx_tm_t  tm;

    ngx_gmtime(t, &tm);

    /*
     * Netscape 3.x does not understand 4-digit years at all and
     * 2-digit years more than "37"
     */

    return ngx_sprintf(buf,
                       (tm.ngx_tm_year > 2037) ?
                                         "%s, %02d-%s-%d %02d:%02d:%02d GMT":
                                         "%s, %02d-%s-%02d %02d:%02d:%02d GMT",
                       week[tm.ngx_tm_wday],
                       tm.ngx_tm_mday,
                       months[tm.ngx_tm_mon - 1],
                       (tm.ngx_tm_year > 2037) ? tm.ngx_tm_year:
                                                 tm.ngx_tm_year % 100,
                       tm.ngx_tm_hour,
                       tm.ngx_tm_min,
                       tm.ngx_tm_sec);
}

/**
 * 时间格式转换 秒转年月日时分秒
 * @param t 要转换的时间 单位秒
 * @param tp 年月日时分秒格式
 */
void
ngx_gmtime(time_t t, ngx_tm_t *tp)
{
    ngx_int_t   yday;
    ngx_uint_t  sec, min, hour, mday, mon, year, wday, days, leap;

    /* the calculation is valid for positive time_t only */

    if (t < 0) {
        t = 0;
    }

    days = t / 86400;
    sec = t % 86400;

    /*
     * no more than 4 year digits supported,
     * truncate to December 31, 9999, 23:59:59
     */

    if (days > 2932896) {
        days = 2932896;
        sec = 86399;
    }

    /* January 1, 1970 was Thursday */

    wday = (4 + days) % 7;

    hour = sec / 3600;
    sec %= 3600;
    min = sec / 60;
    sec %= 60;

    /*
     * the algorithm based on Gauss' formula,
     * see src/core/ngx_parse_time.c
     */

    /* days since March 1, 1 BC */
    days = days - (31 + 28) + 719527;

    /*
     * The "days" should be adjusted to 1 only, however, some March 1st's go
     * to previous year, so we adjust them to 2.  This causes also shift of the
     * last February days to next year, but we catch the case when "yday"
     * becomes negative.
     */

    year = (days + 2) * 400 / (365 * 400 + 100 - 4 + 1);

    yday = days - (365 * year + year / 4 - year / 100 + year / 400);

    if (yday < 0) {
        leap = (year % 4 == 0) && (year % 100 || (year % 400 == 0));
        yday = 365 + leap + yday;
        year--;
    }

    /*
     * The empirical formula that maps "yday" to month.
     * There are at least 10 variants, some of them are:
     *     mon = (yday + 31) * 15 / 459
     *     mon = (yday + 31) * 17 / 520
     *     mon = (yday + 31) * 20 / 612
     */

    mon = (yday + 31) * 10 / 306;

    /* the Gauss' formula that evaluates days before the month */

    mday = yday - (367 * mon / 12 - 30) + 1;

    if (yday >= 306) {

        year++;
        mon -= 10;

        /*
         * there is no "yday" in Win32 SYSTEMTIME
         *
         * yday -= 306;
         */

    } else {

        mon += 2;

        /*
         * there is no "yday" in Win32 SYSTEMTIME
         *
         * yday += 31 + 28 + leap;
         */
    }

    tp->ngx_tm_sec = (ngx_tm_sec_t) sec;
    tp->ngx_tm_min = (ngx_tm_min_t) min;
    tp->ngx_tm_hour = (ngx_tm_hour_t) hour;
    tp->ngx_tm_mday = (ngx_tm_mday_t) mday;
    tp->ngx_tm_mon = (ngx_tm_mon_t) mon;
    tp->ngx_tm_year = (ngx_tm_year_t) year;
    tp->ngx_tm_wday = (ngx_tm_wday_t) wday;
}


time_t
ngx_next_time(time_t when)
{
    time_t     now, next;
    struct tm  tm;

    now = ngx_time();

    ngx_libc_localtime(now, &tm);

    tm.tm_hour = (int) (when / 3600);
    when %= 3600;
    tm.tm_min = (int) (when / 60);
    tm.tm_sec = (int) (when % 60);

    next = mktime(&tm);

    if (next == -1) {
        return -1;
    }

    if (next - now > 0) {
        return next;
    }

    tm.tm_mday++;

    /* mktime() should normalize a date (Jan 32, etc) */

    next = mktime(&tm);

    if (next != -1) {
        return next;
    }

    return -1;
}

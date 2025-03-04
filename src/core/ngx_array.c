
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


/**
 * 实例化数组
 * @param p 内存池
 * @param n 数组支持多少个元素
 * @param size 每个元素大小(byte)
 */
ngx_array_t *
ngx_array_create(ngx_pool_t *p, ngx_uint_t n, size_t size)
{
    ngx_array_t *a;
    // 分配一小段内存给数组结构
    a = ngx_palloc(p, sizeof(ngx_array_t));
    if (a == NULL) {
        return NULL;
    }

    // 初始化数组
    if (ngx_array_init(a, p, n, size) != NGX_OK) {
        return NULL;
    }

    return a;
}

/**
 * 回收数组
 * 并不是真正的释放内存 仅仅是释放内存池内存 让内存池内存继续可用
 * 操作的是内存池第一个内存块 从这个可以倒推
 * <ul>
 *   <li>nginx中数组都是小元素 几乎都在内存池的第一个内存块分配 即使大元素也是在内存池大内存块</li>
 *   <li>所谓的回收并不是真正的回收内存 真正的回收还是交给内存池处理</li>
 * </ul>
 * @param a 数组
 */
void
ngx_array_destroy(ngx_array_t *a)
{
    ngx_pool_t  *p;

    p = a->pool;

    if ((u_char *) a->elts + a->size * a->nalloc == p->d.last) {
        p->d.last -= a->size * a->nalloc;
    }

    if ((u_char *) a + sizeof(ngx_array_t) == p->d.last) {
        p->d.last = (u_char *) a;
    }
}


/**
 * 往数组中添加新元素 添加单个元素
 * 但是nginx只完成了一半工作 告知调用方要添加的元素应该存放的内存地址 实际赋值由调用方自己负责
 * 这样设计的考量点
 * <ul>
 *   <li>1 函数签名没有要添加的元素 避免了额外的拷贝 极致地抠性能</li>
 *   <li>2 返回的是内存地址 支持任务数据类型</li>
 *   <li>3 允许直接修改数据 调用方收到的返回值是内存地址 调用方可以任意操作数组元素</li>
 * </ul>
 * @param a 数组
 * @return 要添加的新元素应该放在数组中的地址
 */
void *
ngx_array_push(ngx_array_t *a)
{
    void        *elt, *new;
    size_t       size;
    ngx_pool_t  *p;

    if (a->nelts == a->nalloc) {
        // 数组满了 要先扩容

        /* the array is full */
        // 数组数据区多大
        size = a->size * a->nalloc;

        p = a->pool;

        if ((u_char *) a->elts + size == p->d.last
            && p->d.last + a->size <= p->d.end)
        {
            /*
             * 很巧妙的判断方式
             * 为什么只判断内存池的第一个内存块呢
             * <ul>
             *   <li>首先池中并没有直接的成员可以访问到当前内存池中最后一个在使用的内存块 可以快速访问的是内存池首个内存块 和当前分配内存块 但是当前分配内存块并不是最后一个可用内存块</li>
             *   <li>第一个条件判定数组是不是分配在内存池第一个内存块上<ul>
             *     <li>不在第一个内存块就一定说明曾经发生过数组扩容了 直接扩容就行</li>
             *     <li>如果数组分配在第一个内存块就看内存块剩余可分配内存够不够扩容使用</li>
             * </ul>
             * 第二个条件 扩容机制是按照1个元素方式
             * 为什么是这样的机制呢
             * <ul>
             *   <li>内存池每个内存块很可能大部分场景都不能满足数组2倍扩容需求</li>
             *   <li>1个元素扩容可以极大利用内存块 尽量避免内存碎片</li>
             *   <li>内存当内存块扩容要系统调用alloc 尽量避免系统调用提升性能</li>
             * </ul>
             */
            /*
             * the array allocation is the last in the pool
             * and there is space for new allocation
             */
            // 数组分配在内存池第一个内存块 内存块剩余可分配内存足够扩容使用 1个数组元素方式扩容 调整内存块内存分配状态
            p->d.last += a->size;
            a->nalloc++;

        } else {
            /* allocate a new array */
            // 2倍扩容机制 扩容完把扩容前数据复制到新内存空间上 保证数组元素连续的特性
            new = ngx_palloc(p, 2 * size);
            if (new == NULL) {
                return NULL;
            }
            // 扩容前元素复制到扩容后内存上
            ngx_memcpy(new, a->elts, size);
            a->elts = new;
            // 2倍扩容机制
            a->nalloc *= 2;
        }
    }
    // 要存放数组元素应该填充的内存地址
    elt = (u_char *) a->elts + a->size * a->nelts;
    // 数组元素计数
    a->nelts++;

    return elt;
}


/**
 * 向数组批量添加元素
 * @param a 数组
 * @param n 要向数组添加的元素个数
 * @return 要写入n个新元素的地址
 */
void *
ngx_array_push_n(ngx_array_t *a, ngx_uint_t n)
{
    void        *elt, *new;
    size_t       size;
    ngx_uint_t   nalloc;
    ngx_pool_t  *p;
    // 新增n个元素要占用的空间
    size = n * a->size;

    if (a->nelts + n > a->nalloc) {
        /*
         * 数组要扩容
         * 边界条件是大于 不包含等于 这也是性能考量点
         * 如果等于条件也要扩容 可能遇到的场景就是费劲扩容好了 并不到再往数组添加元素 也就是扩容出来的内存被浪费了
         */

        /* the array is full */

        p = a->pool;

        if ((u_char *) a->elts + a->size * a->nalloc == p->d.last
            && p->d.last + size <= p->d.end)
        {
            // 内存块内存扩容 不用分配新内存块
            /*
             * the array allocation is the last in the pool
             * and there is space for new allocation
             */

            p->d.last += size;
            a->nalloc += n;

        } else {
            /* allocate a new array */
            // 扩容
            nalloc = 2 * ((n >= a->nalloc) ? n : a->nalloc);

            new = ngx_palloc(p, nalloc * a->size);
            if (new == NULL) {
                return NULL;
            }

            ngx_memcpy(new, a->elts, a->nelts * a->size);
            a->elts = new;
            a->nalloc = nalloc;
        }
    }
    // 添加到数组的元素写到哪个内存地址
    elt = (u_char *) a->elts + a->size * a->nelts;
    a->nelts += n;

    return elt;
}

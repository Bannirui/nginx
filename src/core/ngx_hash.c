
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>

/*
 * 查询hash表 根据键找到对应的值
 * @param hash hash表
 * @param key 键的hash值
 * @param name 键
 * @param len 键的长度
 * @return 值
 */
void *
ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len)
{
    ngx_uint_t       i;
    ngx_hash_elt_t  *elt;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "hf:\"%*s\"", len, name);
#endif
    // 键落在的桶
    elt = hash->buckets[key % hash->size];
    // 桶顶指针是空的 说明桶是空的
    if (elt == NULL) {
        return NULL;
    }
    // 遍历hash桶里面所有的键值对
    while (elt->value) {
        if (len != (size_t) elt->len) {
            goto next;
        }
        // 找到键 确保找到的键的每个字符都是要找的
        for (i = 0; i < len; i++) {
            if (name[i] != elt->name[i]) {
                goto next;
            }
        }
        // 找到了对应键的键值对
        return elt->value;

    next:

        elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                               sizeof(void *));
        continue;
    }

    return NULL;
}


void *
ngx_hash_find_wc_head(ngx_hash_wildcard_t *hwc, u_char *name, size_t len)
{
    void        *value;
    ngx_uint_t   i, n, key;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "wch:\"%*s\"", len, name);
#endif

    n = len;

    while (n) {
        if (name[n - 1] == '.') {
            break;
        }

        n--;
    }

    key = 0;

    for (i = n; i < len; i++) {
        key = ngx_hash(key, name[i]);
    }

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "key:\"%ui\"", key);
#endif

    value = ngx_hash_find(&hwc->hash, key, &name[n], len - n);

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "value:\"%p\"", value);
#endif

    if (value) {

        /*
         * the 2 low bits of value have the special meaning:
         *     00 - value is data pointer for both "example.com"
         *          and "*.example.com";
         *     01 - value is data pointer for "*.example.com" only;
         *     10 - value is pointer to wildcard hash allowing
         *          both "example.com" and "*.example.com";
         *     11 - value is pointer to wildcard hash allowing
         *          "*.example.com" only.
         */

        if ((uintptr_t) value & 2) {

            if (n == 0) {

                /* "example.com" */

                if ((uintptr_t) value & 1) {
                    return NULL;
                }

                hwc = (ngx_hash_wildcard_t *)
                                          ((uintptr_t) value & (uintptr_t) ~3);
                return hwc->value;
            }

            hwc = (ngx_hash_wildcard_t *) ((uintptr_t) value & (uintptr_t) ~3);

            value = ngx_hash_find_wc_head(hwc, name, n - 1);

            if (value) {
                return value;
            }

            return hwc->value;
        }

        if ((uintptr_t) value & 1) {

            if (n == 0) {

                /* "example.com" */

                return NULL;
            }

            return (void *) ((uintptr_t) value & (uintptr_t) ~3);
        }

        return value;
    }

    return hwc->value;
}


void *
ngx_hash_find_wc_tail(ngx_hash_wildcard_t *hwc, u_char *name, size_t len)
{
    void        *value;
    ngx_uint_t   i, key;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "wct:\"%*s\"", len, name);
#endif

    key = 0;

    for (i = 0; i < len; i++) {
        if (name[i] == '.') {
            break;
        }

        key = ngx_hash(key, name[i]);
    }

    if (i == len) {
        return NULL;
    }

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "key:\"%ui\"", key);
#endif

    value = ngx_hash_find(&hwc->hash, key, name, i);

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "value:\"%p\"", value);
#endif

    if (value) {

        /*
         * the 2 low bits of value have the special meaning:
         *     00 - value is data pointer;
         *     11 - value is pointer to wildcard hash allowing "example.*".
         */

        if ((uintptr_t) value & 2) {

            i++;

            hwc = (ngx_hash_wildcard_t *) ((uintptr_t) value & (uintptr_t) ~3);

            value = ngx_hash_find_wc_tail(hwc, &name[i], len - i);

            if (value) {
                return value;
            }

            return hwc->value;
        }

        return value;
    }

    return hwc->value;
}


void *
ngx_hash_find_combined(ngx_hash_combined_t *hash, ngx_uint_t key, u_char *name,
    size_t len)
{
    void  *value;

    if (hash->hash.buckets) {
        value = ngx_hash_find(&hash->hash, key, name, len);

        if (value) {
            return value;
        }
    }

    if (len == 0) {
        return NULL;
    }

    if (hash->wc_head && hash->wc_head->hash.buckets) {
        value = ngx_hash_find_wc_head(hash->wc_head, name, len);

        if (value) {
            return value;
        }
    }

    if (hash->wc_tail && hash->wc_tail->hash.buckets) {
        value = ngx_hash_find_wc_tail(hash->wc_tail, name, len);

        if (value) {
            return value;
        }
    }

    return NULL;
}

/**
* 计算一个键值对放到hash桶实际要占用桶里面多大空间
* <ul>
*   <li>一个指针 指向值->sizeof(void*)</li>
*   <li>short 存储键的长度->2</li>
*   <li>键字符串->name->key.len</li>
* </ul>
*/
#define NGX_HASH_ELT_SIZE(name)                                               \
    (sizeof(void *) + ngx_align((name)->key.len + 2, sizeof(void *)))

/*
 * 把所有键值对names都放到hash表中
 * @param hinit hash表
 * @param names 键值对列表
 * @param nelts 有多少个键值对要存到hash表
 */
ngx_int_t
ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names, ngx_uint_t nelts)
{
    u_char          *elts;
    size_t           len;
    u_short         *test;
    ngx_uint_t       i, n, key, size, start, bucket_size;
    ngx_hash_elt_t  *elt, **buckets;

    if (hinit->max_size == 0) {
        ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                      "could not build %s, you should "
                      "increase %s_max_size: %i",
                      hinit->name, hinit->name, hinit->max_size);
        return NGX_ERROR;
    }

    if (hinit->bucket_size > 65536 - ngx_cacheline_size) {
        // hash桶过大会导致将来查询效率低下
        ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                      "could not build %s, too large "
                      "%s_bucket_size: %i",
                      hinit->name, hinit->name, hinit->bucket_size);
        return NGX_ERROR;
    }
    // 遍历键值对 防御性校验 防止桶连一个键值对都放不下
    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
            continue;
        }
        // 防御性校验 防止桶连一个键值对都放不下
        if (hinit->bucket_size < NGX_HASH_ELT_SIZE(&names[n]) + sizeof(void *))
        {
            /*
             * 什么情况会这样呢
             * <ul>
             *   <li>1是桶大小指定太小</li>
             *   <li>2是键值对的键过长</li>
             * </ul>
             */
            ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                          "could not build %s, you should "
                          "increase %s_bucket_size: %i",
                          hinit->name, hinit->name, hinit->bucket_size);
            return NGX_ERROR;
        }
    }
    /*
     * test用来记录hash桶填充状态避免冲突 记录的是hash桶中已经存放了多大空间 放置超过桶空间上限
     * 为啥用系统调用alloc而不用内存池palloc
     * 因为仅仅是辅助使用 在hash表初始化好就释放了 不需要长期存储
     * max_size是hash桶数量的上限 实际上不一定用这么多 下面会去计算出真正需要的hash桶数量
     * 这个辅助数组就按照max_size分配就行 反正这个初始化方法结束完就释放内存了
     */
    test = ngx_alloc(hinit->max_size * sizeof(u_short), hinit->pool->log);
    if (test == NULL) {
        return NGX_ERROR;
    }
    // 抠除桶底的NULL分隔符 剩下来的就是实际可以存放键值对的空间
    bucket_size = hinit->bucket_size - sizeof(void *);
    /*
     * 牛逼
     * 首先明确hash桶中1个键值对占用空间
     * <ul>
     *   <li>指向值的指针->占sizeof(void*)大小 8byte</li>
     *   <li>键的长度->short类型 2byte</li>
     *   <li>键->不到实际存储的时候都是未知的</li>
     * </ul>
     * 那么上面这一坨经过对齐 至少就是16byte 也就是2个sizeof(void*)
     * 再者 为什么不根据实际键值对计算出来真正需要的桶数据量
     * 没必要 因为已经定了hash数组长度上限 只要定好下限 然后轮询尝试就行
     * 那么这个下限是不是可以极限一下到0 当然可以
     * 下面这个公式的目的就是为了初步定下来最小的桶数量
     * 一个键对最少占sz=2*sizeof(void*)
     * 那么一个桶最多盛放的元素数量n=bucket_size/sz
     * 整个hash表最少需要的桶数量=nelts/n
     */
    start = nelts / (bucket_size / (2 * sizeof(void *)));
    start = start ? start : 1;

    if (hinit->max_size > 10000 && nelts && hinit->max_size / nelts < 100) {
        start = hinit->max_size - 1000;
    }
    /*
     * 经过初步计算需要start个hash桶比较合适 从[start....max_size]开始尝试找到真正合适的hash桶数量
     * 为什么需要尝试
     * <ul>
     *   <li>hash桶太少 hash碰撞的概率就大 从而单个hash桶空间的使用可能达到上限</li>
     *   <li>hash桶太多 占用空间大而且查询效率低</li>
     * </ul>
     * 因此在尝试定桶数量过程中发现桶大小超限就增加桶数量
     */
    for (size = start; size <= hinit->max_size; size++) {
        // 辅助数组初始化0
        ngx_memzero(test, size * sizeof(u_short));
        // 计算将所有键值对放到hash表中会不会导致桶过大 用辅助表test记录每个桶大小
        for (n = 0; n < nelts; n++) {
            if (names[n].key.data == NULL) {
                continue;
            }
            // 键值对应该放在哪个hash桶 数组的脚标
            key = names[n].key_hash % size;
            // 要是继续把当前键值对放在这个桶里面 之后桶空间的大小达到多大
            len = test[key] + NGX_HASH_ELT_SIZE(&names[n]);

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %ui %uz \"%V\"",
                          size, key, len, &names[n].key);
#endif
            // 选定桶数量是size后 存放nelts个键值对过程中发现有桶的大小超限了 因此要尝试使用更多桶数量
            if (len > bucket_size) {
                goto next;
            }
            // 更新辅助表记录桶大小
            test[key] = (u_short) len;
        }
        // 找到了合适的桶数量size个 可以保证桶数量金可能少并且桶不过大
        goto found;

    next:

        continue;
    }

    size = hinit->max_size;

    ngx_log_error(NGX_LOG_WARN, hinit->pool->log, 0,
                  "could not build optimal %s, you should increase "
                  "either %s_max_size: %i or %s_bucket_size: %i; "
                  "ignoring %s_bucket_size",
                  hinit->name, hinit->name, hinit->max_size,
                  hinit->name, hinit->bucket_size, hinit->name);

found:
    /*
     * 此时定下来桶的数量size个 也就是hash表数组长度是size对应脚标是[0...size-1]
     * test辅助数组的作用依然是记录每个桶被占用的空间
     * 在上面test已经被使用过一轮了 已经不干净了因此要初始化一下[0...size]脚标
     * 初始化的时候先把每个桶的桶底指针占用的空间统计上
     */
    for (i = 0; i < size; i++) {
        test[i] = sizeof(void *);
    }
    // 如果把所有元素放到hash表 在辅助表上记录每个hash桶的大小
    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
            continue;
        }
        // 根据hash定位到键值对放到哪个hash桶
        key = names[n].key_hash % size;
        len = test[key] + NGX_HASH_ELT_SIZE(&names[n]);

        if (len > 65536 - ngx_cacheline_size) {
            ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                          "could not build %s, you should "
                          "increase %s_max_size: %i",
                          hinit->name, hinit->name, hinit->max_size);
            ngx_free(test);
            return NGX_ERROR;
        }

        test[key] = (u_short) len;
    }
    // 统计所有hash桶占用多大空间
    len = 0;
    // 遍历hash桶 找到不是空桶 统计所有桶占用的大小
    for (i = 0; i < size; i++) {
        if (test[i] == sizeof(void *)) {
            // 辅助数组记录了桶占用空间就一个指针 说明是空桶
            continue;
        }

        test[i] = (u_short) (ngx_align(test[i], ngx_cacheline_size));

        len += test[i];
    }

    if (hinit->hash == NULL) {
        hinit->hash = ngx_pcalloc(hinit->pool, sizeof(ngx_hash_wildcard_t)
                                             + size * sizeof(ngx_hash_elt_t *));
        if (hinit->hash == NULL) {
            ngx_free(test);
            return NGX_ERROR;
        }

        buckets = (ngx_hash_elt_t **)
                      ((u_char *) hinit->hash + sizeof(ngx_hash_wildcard_t));

    } else {
        // hash数组
        buckets = ngx_pcalloc(hinit->pool, size * sizeof(ngx_hash_elt_t *));
        if (buckets == NULL) {
            ngx_free(test);
            return NGX_ERROR;
        }
    }
    // hash表盛满键值对 hash桶要多大内存
    elts = ngx_palloc(hinit->pool, len + ngx_cacheline_size);
    if (elts == NULL) {
        ngx_free(test);
        return NGX_ERROR;
    }

    elts = ngx_align_ptr(elts, ngx_cacheline_size);
    /*
     * 已经给hash桶的内存已经分配好 起始地址是elts 每个hash桶要占用多大空间这个信息在test辅助数组中记录着
     * 很容易就可以把整个内存瓜分给每个hash桶
     * 此时test辅助数组中存放[0...size-1]每个桶需要的分配空间 空桶只有一个NULL指针占位 自然不用实际分配内存
     */
    for (i = 0; i < size; i++) {
        if (test[i] == sizeof(void *)) {
            // 空桶不用管
            continue;
        }
        /*
         * 每个hash桶分配的内存空间 起始地址 等真正存放元素的时候就后移指针就行
         * 分配给桶的空间已经包含了一个占位指针
         * 等键值对根据key的hash值定位到桶依次放完之后 桶里面就会在桶底剩下一个指针空间
         */
        buckets[i] = (ngx_hash_elt_t *) elts;
        elts += test[i];
    }
    // 下面要开始真正放键值对 键值对从桶顶开始放 放元素过程中用test辅助数组统计hash桶使用的空间 因此要在这个地方初始化0
    for (i = 0; i < size; i++) {
        test[i] = 0;
    }
    // 遍历键值对 逐个放到对应hash桶里面
    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
            continue;
        }
        // hash桶数组脚标
        key = names[n].key_hash % size;
        // test数组中已经缓存好了每个桶的使用大小了 buckets[key]就是桶顶地址 键值对应该存放在桶里面位置
        elt = (ngx_hash_elt_t *) ((u_char *) buckets[key] + test[key]);
        // 存放键值对
        // 值
        elt->value = names[n].value;
        // 键的长度
        elt->len = (u_short) names[n].key.len;
        // 键
        ngx_strlow(elt->name, names[n].key.data, names[n].key.len);
        // 新的键值对已经存到了hash桶 此时hash桶的实际使用空间也要更新 加上新增键值对的占用空间
        test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));
    }
    /*
     * 这个时候已经实际上把所有键值对放到了hash桶
     * 但是在分配hash桶空间的时候是多分配了个sizeof(void*)的 这个指针的用处就是作为桶与桶之间分割符的
     * 遍历hash桶 在每个桶的底部插上NULL 也就是让每个桶底的指针都指向NULL
     */
    for (i = 0; i < size; i++) {
        if (buckets[i] == NULL) {
            // 空桶
            continue;
        }
        // 找到桶底插上NULL标识桶分界
        elt = (ngx_hash_elt_t *) ((u_char *) buckets[i] + test[i]);

        elt->value = NULL;
    }
    // 回收辅助数组
    ngx_free(test);

    hinit->hash->buckets = buckets;
    hinit->hash->size = size;

#if 0

    for (i = 0; i < size; i++) {
        ngx_str_t   val;
        ngx_uint_t  key;

        elt = buckets[i];

        if (elt == NULL) {
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: NULL", i);
            continue;
        }

        while (elt->value) {
            val.len = elt->len;
            val.data = &elt->name[0];

            key = hinit->key(val.data, val.len);

            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %p \"%V\" %ui", i, elt, &val, key);

            elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                                   sizeof(void *));
        }
    }

#endif

    return NGX_OK;
}


ngx_int_t
ngx_hash_wildcard_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names,
    ngx_uint_t nelts)
{
    size_t                len, dot_len;
    ngx_uint_t            i, n, dot;
    ngx_array_t           curr_names, next_names;
    ngx_hash_key_t       *name, *next_name;
    ngx_hash_init_t       h;
    ngx_hash_wildcard_t  *wdc;

    if (ngx_array_init(&curr_names, hinit->temp_pool, nelts,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&next_names, hinit->temp_pool, nelts,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    for (n = 0; n < nelts; n = i) {

#if 0
        ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                      "wc0: \"%V\"", &names[n].key);
#endif

        dot = 0;

        for (len = 0; len < names[n].key.len; len++) {
            if (names[n].key.data[len] == '.') {
                dot = 1;
                break;
            }
        }

        name = ngx_array_push(&curr_names);
        if (name == NULL) {
            return NGX_ERROR;
        }

        name->key.len = len;
        name->key.data = names[n].key.data;
        name->key_hash = hinit->key(name->key.data, name->key.len);
        name->value = names[n].value;

#if 0
        ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                      "wc1: \"%V\" %ui", &name->key, dot);
#endif

        dot_len = len + 1;

        if (dot) {
            len++;
        }

        next_names.nelts = 0;

        if (names[n].key.len != len) {
            next_name = ngx_array_push(&next_names);
            if (next_name == NULL) {
                return NGX_ERROR;
            }

            next_name->key.len = names[n].key.len - len;
            next_name->key.data = names[n].key.data + len;
            next_name->key_hash = 0;
            next_name->value = names[n].value;

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "wc2: \"%V\"", &next_name->key);
#endif
        }

        for (i = n + 1; i < nelts; i++) {
            if (ngx_strncmp(names[n].key.data, names[i].key.data, len) != 0) {
                break;
            }

            if (!dot
                && names[i].key.len > len
                && names[i].key.data[len] != '.')
            {
                break;
            }

            next_name = ngx_array_push(&next_names);
            if (next_name == NULL) {
                return NGX_ERROR;
            }

            next_name->key.len = names[i].key.len - dot_len;
            next_name->key.data = names[i].key.data + dot_len;
            next_name->key_hash = 0;
            next_name->value = names[i].value;

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "wc3: \"%V\"", &next_name->key);
#endif
        }

        if (next_names.nelts) {

            h = *hinit;
            h.hash = NULL;

            if (ngx_hash_wildcard_init(&h, (ngx_hash_key_t *) next_names.elts,
                                       next_names.nelts)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            wdc = (ngx_hash_wildcard_t *) h.hash;

            if (names[n].key.len == len) {
                wdc->value = names[n].value;
            }

            name->value = (void *) ((uintptr_t) wdc | (dot ? 3 : 2));

        } else if (dot) {
            name->value = (void *) ((uintptr_t) name->value | 1);
        }
    }

    if (ngx_hash_init(hinit, (ngx_hash_key_t *) curr_names.elts,
                      curr_names.nelts)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_uint_t
ngx_hash_key(u_char *data, size_t len)
{
    ngx_uint_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = ngx_hash(key, data[i]);
    }

    return key;
}


ngx_uint_t
ngx_hash_key_lc(u_char *data, size_t len)
{
    ngx_uint_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = ngx_hash(key, ngx_tolower(data[i]));
    }

    return key;
}


ngx_uint_t
ngx_hash_strlow(u_char *dst, u_char *src, size_t n)
{
    ngx_uint_t  key;

    key = 0;

    while (n--) {
        *dst = ngx_tolower(*src);
        key = ngx_hash(key, *dst);
        dst++;
        src++;
    }

    return key;
}


ngx_int_t
ngx_hash_keys_array_init(ngx_hash_keys_arrays_t *ha, ngx_uint_t type)
{
    ngx_uint_t  asize;

    if (type == NGX_HASH_SMALL) {
        asize = 4;
        ha->hsize = 107;

    } else {
        asize = NGX_HASH_LARGE_ASIZE;
        ha->hsize = NGX_HASH_LARGE_HSIZE;
    }

    if (ngx_array_init(&ha->keys, ha->temp_pool, asize, sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&ha->dns_wc_head, ha->temp_pool, asize,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (ngx_array_init(&ha->dns_wc_tail, ha->temp_pool, asize,
                       sizeof(ngx_hash_key_t))
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ha->keys_hash = ngx_pcalloc(ha->temp_pool, sizeof(ngx_array_t) * ha->hsize);
    if (ha->keys_hash == NULL) {
        return NGX_ERROR;
    }

    ha->dns_wc_head_hash = ngx_pcalloc(ha->temp_pool,
                                       sizeof(ngx_array_t) * ha->hsize);
    if (ha->dns_wc_head_hash == NULL) {
        return NGX_ERROR;
    }

    ha->dns_wc_tail_hash = ngx_pcalloc(ha->temp_pool,
                                       sizeof(ngx_array_t) * ha->hsize);
    if (ha->dns_wc_tail_hash == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_hash_add_key(ngx_hash_keys_arrays_t *ha, ngx_str_t *key, void *value,
    ngx_uint_t flags)
{
    size_t           len;
    u_char          *p;
    ngx_str_t       *name;
    ngx_uint_t       i, k, n, skip, last;
    ngx_array_t     *keys, *hwc;
    ngx_hash_key_t  *hk;

    last = key->len;

    if (flags & NGX_HASH_WILDCARD_KEY) {

        /*
         * supported wildcards:
         *     "*.example.com", ".example.com", and "www.example.*"
         */

        n = 0;

        for (i = 0; i < key->len; i++) {

            if (key->data[i] == '*') {
                if (++n > 1) {
                    return NGX_DECLINED;
                }
            }

            if (key->data[i] == '.' && key->data[i + 1] == '.') {
                return NGX_DECLINED;
            }

            if (key->data[i] == '\0') {
                return NGX_DECLINED;
            }
        }

        if (key->len > 1 && key->data[0] == '.') {
            skip = 1;
            goto wildcard;
        }

        if (key->len > 2) {

            if (key->data[0] == '*' && key->data[1] == '.') {
                skip = 2;
                goto wildcard;
            }

            if (key->data[i - 2] == '.' && key->data[i - 1] == '*') {
                skip = 0;
                last -= 2;
                goto wildcard;
            }
        }

        if (n) {
            return NGX_DECLINED;
        }
    }

    /* exact hash */

    k = 0;

    for (i = 0; i < last; i++) {
        if (!(flags & NGX_HASH_READONLY_KEY)) {
            key->data[i] = ngx_tolower(key->data[i]);
        }
        k = ngx_hash(k, key->data[i]);
    }

    k %= ha->hsize;

    /* check conflicts in exact hash */

    name = ha->keys_hash[k].elts;

    if (name) {
        for (i = 0; i < ha->keys_hash[k].nelts; i++) {
            if (last != name[i].len) {
                continue;
            }

            if (ngx_strncmp(key->data, name[i].data, last) == 0) {
                return NGX_BUSY;
            }
        }

    } else {
        if (ngx_array_init(&ha->keys_hash[k], ha->temp_pool, 4,
                           sizeof(ngx_str_t))
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    name = ngx_array_push(&ha->keys_hash[k]);
    if (name == NULL) {
        return NGX_ERROR;
    }

    *name = *key;

    hk = ngx_array_push(&ha->keys);
    if (hk == NULL) {
        return NGX_ERROR;
    }

    hk->key = *key;
    hk->key_hash = ngx_hash_key(key->data, last);
    hk->value = value;

    return NGX_OK;


wildcard:

    /* wildcard hash */

    k = ngx_hash_strlow(&key->data[skip], &key->data[skip], last - skip);

    k %= ha->hsize;

    if (skip == 1) {

        /* check conflicts in exact hash for ".example.com" */

        name = ha->keys_hash[k].elts;

        if (name) {
            len = last - skip;

            for (i = 0; i < ha->keys_hash[k].nelts; i++) {
                if (len != name[i].len) {
                    continue;
                }

                if (ngx_strncmp(&key->data[1], name[i].data, len) == 0) {
                    return NGX_BUSY;
                }
            }

        } else {
            if (ngx_array_init(&ha->keys_hash[k], ha->temp_pool, 4,
                               sizeof(ngx_str_t))
                != NGX_OK)
            {
                return NGX_ERROR;
            }
        }

        name = ngx_array_push(&ha->keys_hash[k]);
        if (name == NULL) {
            return NGX_ERROR;
        }

        name->len = last - 1;
        name->data = ngx_pnalloc(ha->temp_pool, name->len);
        if (name->data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(name->data, &key->data[1], name->len);
    }


    if (skip) {

        /*
         * convert "*.example.com" to "com.example.\0"
         *      and ".example.com" to "com.example\0"
         */

        p = ngx_pnalloc(ha->temp_pool, last);
        if (p == NULL) {
            return NGX_ERROR;
        }

        len = 0;
        n = 0;

        for (i = last - 1; i; i--) {
            if (key->data[i] == '.') {
                ngx_memcpy(&p[n], &key->data[i + 1], len);
                n += len;
                p[n++] = '.';
                len = 0;
                continue;
            }

            len++;
        }

        if (len) {
            ngx_memcpy(&p[n], &key->data[1], len);
            n += len;
        }

        p[n] = '\0';

        hwc = &ha->dns_wc_head;
        keys = &ha->dns_wc_head_hash[k];

    } else {

        /* convert "www.example.*" to "www.example\0" */

        last++;

        p = ngx_pnalloc(ha->temp_pool, last);
        if (p == NULL) {
            return NGX_ERROR;
        }

        ngx_cpystrn(p, key->data, last);

        hwc = &ha->dns_wc_tail;
        keys = &ha->dns_wc_tail_hash[k];
    }


    /* check conflicts in wildcard hash */

    name = keys->elts;

    if (name) {
        len = last - skip;

        for (i = 0; i < keys->nelts; i++) {
            if (len != name[i].len) {
                continue;
            }

            if (ngx_strncmp(key->data + skip, name[i].data, len) == 0) {
                return NGX_BUSY;
            }
        }

    } else {
        if (ngx_array_init(keys, ha->temp_pool, 4, sizeof(ngx_str_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    name = ngx_array_push(keys);
    if (name == NULL) {
        return NGX_ERROR;
    }

    name->len = last - skip;
    name->data = ngx_pnalloc(ha->temp_pool, name->len);
    if (name->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(name->data, key->data + skip, name->len);


    /* add to wildcard hash */

    hk = ngx_array_push(hwc);
    if (hk == NULL) {
        return NGX_ERROR;
    }

    hk->key.len = last - 1;
    hk->key.data = p;
    hk->key_hash = 0;
    hk->value = value;

    return NGX_OK;
}

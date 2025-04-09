
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_RBTREE_H_INCLUDED_
#define _NGX_RBTREE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


typedef ngx_uint_t  ngx_rbtree_key_t;
typedef ngx_int_t   ngx_rbtree_key_int_t;


typedef struct ngx_rbtree_node_s  ngx_rbtree_node_t;
/*
 * 红黑树组织着待处理事件 排序的权重是key
 */
struct ngx_rbtree_node_s {
    // 事件的到期时间 啥时候要处理这个事件 在红黑树中用这个时间为权重进行排序
    ngx_rbtree_key_t       key;
    ngx_rbtree_node_t     *left;
    ngx_rbtree_node_t     *right;
    ngx_rbtree_node_t     *parent;
    u_char                 color;
    u_char                 data;
};


typedef struct ngx_rbtree_s  ngx_rbtree_t;

typedef void (*ngx_rbtree_insert_pt) (ngx_rbtree_node_t *root,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);

struct ngx_rbtree_s {
    ngx_rbtree_node_t     *root;
    // 红黑树的哨兵节点 这个节点是红黑树实现中的特殊节点 它的left和right都指向自己 表示空节点 标识着红黑树的边界
    ngx_rbtree_node_t     *sentinel;
    ngx_rbtree_insert_pt   insert;
};


#define ngx_rbtree_init(tree, s, i)                                           \
    ngx_rbtree_sentinel_init(s);                                              \
    (tree)->root = s;                                                         \
    (tree)->sentinel = s;                                                     \
    (tree)->insert = i

/*
 * node是type的一个成员 成员名是link 根据node的指针找到到type本身的地址
 * <ul>
 *   <li>有了事件定时器的指针</li>
 *   <li>计算出timer在结构体ngx_event_t的偏移量是多少个字节 再知晓定时器在事件中的偏移</li>
 * </ul>
 * 就可以向前偏移找到事件的地址
 * @param node ngx_rbtree_node_t 事件队列中的一个红黑树节点
 * @param type 结构体ngx_event_t
 * @param link 结构体成员timer
 * @return type的地址
 */
#define ngx_rbtree_data(node, type, link)                                     \
    (type *) ((u_char *) (node) - offsetof(type, link))


void ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
void ngx_rbtree_delete(ngx_rbtree_t *tree, ngx_rbtree_node_t *node);
void ngx_rbtree_insert_value(ngx_rbtree_node_t *root, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel);
void ngx_rbtree_insert_timer_value(ngx_rbtree_node_t *root,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
ngx_rbtree_node_t *ngx_rbtree_next(ngx_rbtree_t *tree,
    ngx_rbtree_node_t *node);


#define ngx_rbt_red(node)               ((node)->color = 1)
#define ngx_rbt_black(node)             ((node)->color = 0)
#define ngx_rbt_is_red(node)            ((node)->color)
#define ngx_rbt_is_black(node)          (!ngx_rbt_is_red(node))
#define ngx_rbt_copy_color(n1, n2)      (n1->color = n2->color)


/* a sentinel must be black */

#define ngx_rbtree_sentinel_init(node)  ngx_rbt_black(node)

/*
 * 红黑树的搜索 找到树里面的最小节点
 * @param node 根节点
 * @param sentinel 红黑树的哨兵节点 这是个特殊的节点 用来表示树的边界 它的left和right都指向自身 表示自己是个空节点
 */
static ngx_inline ngx_rbtree_node_t *
ngx_rbtree_min(ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    // 找到树里面的最左子节点 就是权重最小的节点
    while (node->left != sentinel) {
        node = node->left;
    }

    return node;
}


#endif /* _NGX_RBTREE_H_INCLUDED_ */

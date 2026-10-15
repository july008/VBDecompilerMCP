/*
 * khash.h - 轻量字符串哈希表（i18n 翻译表使用）
 * khash.h - Minimal string hash table (used by the i18n translation map)
 *
 * 为什么自带而不是引入完整 khash：
 *   khash 上游是单头文件库（MIT），但完整版依赖大量宏（__ac_HASHMAP /
 *   KHASH_MAP_INIT_STR / kh_put / kh_get ...）与 C++ 不完全兼容的内部实现。
 *   本项目只需要"字符串键 → 指针值"这一个场景，因此这里提供一个
 *   **API 兼容子集**的紧凑实现：
 *
 *     khash_t / khint_t / kh_init_str / kh_put / kh_get / kh_destroy
 *     kh_size / kh_foreach / kh_str_hash_func / kh_str_hash_equal
 *
 *   语义：
 *     - kh_put(h, key, val, &ret)：ret = 1 新增键 / 2 键已存在（值被覆盖）/ 0 失败
 *     - 哈希表**不拥有** key/value 指针的内存（由调用方 strdup 并在销毁时 free）
 *     - 表发布后即只读：查询可被多线程并发调用，写入/释放只在 UI 主线程
 *
 * 算法：FNV-1a + 开放寻址（线性探测）+ 负载因子 0.75 自动扩容。
 *
 * Why not vendor the full upstream khash:
 *   Upstream khash is a single-header MIT library, but the full version relies on
 *   heavy macro machinery (__ac_HASHMAP / KHASH_MAP_INIT_STR / kh_put / kh_get
 *   ...) and internals that are not fully C++ compatible. This project only needs
 *   "string key -> pointer value", so a compact API-compatible subset is provided:
 *
 *     khash_t / khint_t / kh_init_str / kh_put / kh_get / kh_destroy
 *     kh_size / kh_foreach / kh_str_hash_func / kh_str_hash_equal
 *
 *   Semantics:
 *     - kh_put(h, key, val, &ret): ret = 1 key added / 2 key exists (value
 *       overwritten) / 0 failure
 *     - the table does NOT own key/value memory (caller strdups and frees them)
 *     - a published table is read-only: concurrent lookups are safe, writes and
 *       destruction happen on the UI main thread only
 *
 * Algorithm: FNV-1a + open addressing (linear probing) + auto grow at 0.75 load.
 */
#ifndef VBDMCP_KHASH_H
#define VBDMCP_KHASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t khint_t;

/* 不透明句柄：调用方只通过下面的函数访问。
 * Opaque handle: accessed only through the functions below. */
typedef struct khash_t khash_t;

typedef khint_t (*kh_hash_fn)(const char *key);
typedef int      (*kh_equal_fn)(const char *a, const char *b);
/* 遍历回调：key / value / 用户上下文。
 * Iteration callback: key / value / user context. */
typedef void     (*kh_foreach_fn)(const char *key, void *val, void *user);

/* ---------------- 哈希函数 / Hash functions ---------------- */

/* FNV-1a 32bit */
static khint_t kh_str_hash_func(const char *s)
{
    khint_t h = 2166136261u;
    if (!s) return 0;
    for (; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h ? h : 1u;   /* 0 保留给"空槽" / 0 is reserved for "empty slot" */
}

static int kh_str_hash_equal(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* ---------------- 内部桶 / Internal buckets ---------------- */

typedef struct {
    khint_t hash;      /* 0 = 空槽 / 0 = empty slot */
    const char *key;
    void *val;
} kh_bucket_t;

struct khash_t {
    kh_bucket_t *buckets;
    khint_t      n_buckets;   /* 总是 2 的幂 / always a power of two */
    khint_t      size;        /* 已用槽数 / used slots */
};

/* ---------------- 生命周期 / Lifecycle ---------------- */

static khash_t *kh_init_str(kh_hash_fn hash_fn, kh_equal_fn equal_fn)
{
    /* 两个函数参数为 API 兼容保留：当前实现固定字符串语义。
     * Both function parameters are kept for API compatibility; the current
     * implementation is fixed to string semantics. */
    (void)hash_fn; (void)equal_fn;
    khash_t *h = (khash_t *)calloc(1, sizeof(khash_t));
    if (!h) return NULL;
    h->n_buckets = 64;
    h->buckets = (kh_bucket_t *)calloc(h->n_buckets, sizeof(kh_bucket_t));
    if (!h->buckets) { free(h); return NULL; }
    return h;
}

/* 只释放桶数组：不释放 key / value（所有权在调用方）。
 * Frees the bucket array only; key / value are owned by the caller. */
static void kh_destroy(khash_t *h)
{
    if (!h) return;
    free(h->buckets);
    free(h);
}

static khint_t kh_size(const khash_t *h)
{
    return h ? h->size : 0;
}

/* 负载因子 > 0.75 时翻倍扩容。
 * Doubles the bucket count when the load factor exceeds 0.75. */
static int kh_resize(khash_t *h, khint_t new_n)
{
    kh_bucket_t *old = h->buckets;
    khint_t old_n = h->n_buckets;

    kh_bucket_t *nb = (kh_bucket_t *)calloc(new_n, sizeof(kh_bucket_t));
    if (!nb) return 0;                    /* 扩容失败：保持原状，仍可继续插入 / grow failed: keep the old table, inserts still work */

    h->buckets = nb;
    h->n_buckets = new_n;
    h->size = 0;

    for (khint_t i = 0; i < old_n; ++i) {
        if (!old[i].hash) continue;
        khint_t j = old[i].hash & (new_n - 1);
        while (nb[j].hash) j = (j + 1) & (new_n - 1);
        nb[j] = old[i];
        h->size++;
    }
    free(old);
    return 1;
}

/* ---------------- 增删查 / Insert, lookup, iterate ---------------- */

/*
 * 插入 / 更新。返回 1 = 成功，0 = 失败（h / key 为 NULL）。
 * ret（可选）：1 = 新增键；2 = 键已存在（val 被覆盖）。
 * key 与 val 的内存所有权归调用方（通常 strdup），需在销毁前自行 free。
 *
 * Insert / update. Returns 1 on success, 0 on failure (h / key is NULL).
 * ret (optional): 1 = key added; 2 = key already existed (val overwritten).
 * key and val are owned by the caller (usually strdup'ed) and must be freed
 * before destroying the table.
 */
static int kh_put(khash_t *h, const char *key, void *val, int *ret)
{
    int status = 0;
    if (ret) *ret = 0;
    if (!h || !key) return 0;

    if ((h->size + 1) * 4 >= h->n_buckets * 3) {
        kh_resize(h, h->n_buckets * 2);    /* 扩容失败也继续（原表仍可用） / grow failure is non-fatal (the old table still works) */
    }

    khint_t hv = kh_str_hash_func(key);
    khint_t mask = h->n_buckets - 1;
    khint_t i = hv & mask;

    while (h->buckets[i].hash) {
        if (h->buckets[i].hash == hv && kh_str_hash_equal(h->buckets[i].key, key)) {
            h->buckets[i].val = val;
            if (ret) *ret = 2;
            return 1;
        }
        i = (i + 1) & mask;
    }

    h->buckets[i].hash = hv;
    h->buckets[i].key  = key;
    h->buckets[i].val  = val;
    h->size++;
    if (ret) *ret = 1;
    return 1;
}

/* 查找：命中返回 val（可以是 NULL），未命中返回 NULL。
 * 需要区分"存在但值为 NULL"时，请配合 kh_size / 遍历使用。
 *
 * Lookup: returns val on hit (val itself may be NULL), NULL on miss.
 * To distinguish "present but NULL", use kh_size and iteration. */
static void *kh_get(khash_t *h, const char *key)
{
    if (!h || !key) return NULL;
    khint_t hv = kh_str_hash_func(key);
    khint_t mask = h->n_buckets - 1;
    khint_t i = hv & mask;
    while (h->buckets[i].hash) {
        if (h->buckets[i].hash == hv && kh_str_hash_equal(h->buckets[i].key, key)) {
            return h->buckets[i].val;
        }
        i = (i + 1) & mask;
    }
    return NULL;
}


/* 遍历所有键值对（顺序不保证）。
 * Iterates all key/value pairs (order not guaranteed). */
static void kh_foreach(khash_t *h, kh_foreach_fn fn, void *user)
{
    if (!h || !fn) return;
    for (khint_t i = 0; i < h->n_buckets; ++i) {
        if (h->buckets[i].hash) fn(h->buckets[i].key, h->buckets[i].val, user);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* VBDMCP_KHASH_H */

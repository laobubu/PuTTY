/*
 * platform-independent sub thread executor
 */

#include <assert.h>

#include "putty.h"
#include "subthd.h"
#include "terminal.h"
#include "ldisc.h"

//FIXME: SSH may fail due to OUR_V2_PACKETLIMIT
#define cache_count 16
#define POOL_SIZE 4096

typedef struct pool_tag { char* data, *ptr; int cap; } pool_t;
void pool_init(pool_t *p, int cap) { p->data = p->ptr = smalloc(cap); p->cap = cap; }
int pool_capable(pool_t *p, int len) { return p->ptr - p->data <= p->cap - len; }   // check if there is enough space
void pool_push(pool_t *p, void* data, int len) { memcpy(p->ptr, data, len); p->ptr += len; }
void pool_reset(pool_t *p) { p->ptr = p->data; }
void pool_free(pool_t *p) { sfree(p->data); sfree(p); }
int pool_len(pool_t *p) { return p->ptr - p->data; }

int i_sent = 0, i_to_send = 0, i_cache = -1; 
    // i_sent   : the pool that alrady sent
    // i_to_send: keep sending until this pool is sent
    // i_cache  : currently not fully filled pool. -1 means not inited.
struct pool_tag* cache[cache_count] = { NULL };
int cache_sp = -1; // only one Telnet_Special

// inject extra loop work into the mainloop
int subthd_extra_loop_process()
{
    Ldisc ldisc = ((Ldisc)term->ldisc);

    if (!ldisc) return 0;

    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

    int  xyz_Process(Backend *back, void *backhandle, Terminal *term);

    xyz_Process(back, backhandle, term);     //ZModem handler
    subthd_back_flush_2();                   //subthd_back_write handler

    return term->xyz_transfering || term->inbuf2_enabled;
}

static void subthd_create_next_pool()
{
    // end (and queue) current pool and create next pool
    int next_i_cache, next_i_to_send;

    assert(i_cache != -1);

    next_i_to_send = i_cache;
    next_i_cache = (i_cache == cache_count - 1) ? 0 : (i_cache + 1);
    while (i_sent == next_i_cache)
        subthd_sleep(10);

    if (!cache[next_i_cache]) {
        pool_t *newpool = snew(pool_t);
        pool_init(newpool, POOL_SIZE);
        cache[next_i_cache] = newpool;
    }

    i_cache = next_i_cache;
    i_to_send = next_i_to_send;
}

void subthd_back_write(char * data, int len)
{
    if (i_cache == -1) {
        pool_t *newpool = snew(pool_t);
        pool_init(newpool, POOL_SIZE);
        cache[1] = newpool;
        i_cache = 1;
    }

    if (!pool_capable(cache[i_cache], len)) {
        subthd_create_next_pool();
    }

    pool_push(cache[i_cache], data, len);
}

void subthd_back_special(Telnet_Special s)
{
    cache_sp = s;
    while (cache_sp != -1)
        subthd_sleep(10);
}

void subthd_back_flush()
{
    if (i_cache == -1) return;  // nothing sent yet.
    if (pool_len(cache[i_cache]) > 0) subthd_create_next_pool();

    while (i_sent != i_to_send) 
        subthd_sleep(10);
}

int subthd_back_flush_2()
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;
    
    if (cache_sp != -1) {
        back->special(backhandle, cache_sp);
        cache_sp = -1;
    }

    if (!back->sendbuffer(backhandle) && i_sent != i_to_send) {
        int next_sent = i_sent == (cache_count - 1) ? 0 : (i_sent + 1);
        pool_t *cp = cache[next_sent];
        back->send(backhandle, cp->data, pool_len(cp));
        pool_reset(cp);
        i_sent = next_sent;
    }

    return 1;
}

int subthd_back_wait(int len, int timeout)
{
    time_t until = time(NULL) + timeout;
    while (subthd_back_read_buflen() < len) {
        if (time(NULL) > until) return 0;
        subthd_sleep(10);
    }
    return 1;
}

void subthd_back_read(char * buf, int len)
{
    while (subthd_back_read_buflen() < len) subthd_sleep(10);

    subthd_mutex_lock(term->inbuf2_mutex, 0);

    bufchain_fetch(&term->inbuf2, buf, len);
    bufchain_consume(&term->inbuf2, len);
    
    subthd_mutex_unlock(term->inbuf2_mutex);
}

void subthd_back_read_empty_buf()
{
    subthd_mutex_lock(term->inbuf2_mutex, 0);

    bufchain_clear(&term->inbuf2);

    subthd_mutex_unlock(term->inbuf2_mutex);
}

int subthd_back_read_buflen()
{
    int retval;
    subthd_mutex_lock(term->inbuf2_mutex, 0);

    retval = bufchain_size(&term->inbuf2);

    subthd_mutex_unlock(term->inbuf2_mutex);
    return retval;
}

/*
 * platform-independent sub thread executor
 */

#include "putty.h"
#include "subthd.h"
#include "terminal.h"
#include "ldisc.h"

#define cache_count 16

struct cache_part {
    void *data;
    unsigned long len;
};

int i_sent = 0, i_cache = 0;
struct cache_part* cache[cache_count];
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

static void subthd_wait_for_writing()
{
    // wait until there is a empty buffer
    while ((i_sent == 0 && i_cache == cache_count - 1) || (i_cache == i_sent - 1)) 
        subthd_sleep(10);
}

void subthd_back_write(char * data, int len)
{
    subthd_wait_for_writing();

    void *newcache = smalloc(len);
    memcpy(newcache, data, len);

    struct cache_part* newpt = snew(struct cache_part);
    newpt->data = newcache;
    newpt->len = len;

    int next_room = i_cache == (cache_count - 1) ? 0 : (i_cache + 1);
    cache[next_room] = newpt;
    i_cache = next_room;
}

void subthd_back_special(Telnet_Special s)
{
    cache_sp = s;
    while (cache_sp != -1)
        subthd_sleep(10);
}

void subthd_back_flush()
{
    while (i_sent != i_cache) 
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

    if (!back->sendbuffer(backhandle) && i_sent != i_cache) {
        int next_sent = i_sent == (cache_count - 1) ? 0 : (i_sent + 1);
        struct cache_part *cp = cache[next_sent];
        back->send(backhandle, cp->data, cp->len);
        sfree(cp->data);
        sfree(cp);
        i_sent = next_sent;
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

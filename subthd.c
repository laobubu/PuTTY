/*
 * platform-independent sub thread executor
 */

#include "putty.h"
#include "subthd.h"
#include "terminal.h"
#include "ldisc.h"

//sending to backend
static char buffer[2048], 
    *bufptr = buffer, 
    *bufptr_boundary = buffer + sizeof(buffer);

void subthd_back_write(char * buf, int len)
{
    if (bufptr + len >= bufptr_boundary) subthd_back_flush();
    memcpy(bufptr, buf, len);
    bufptr += len;
}

void subthd_back_flush()
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

    volatile int sendbuf_left;

    if (bufptr == buffer) return; //nothign to send
    back->send(backhandle, buffer, bufptr - buffer);

    do {
        subthd_sleep(10);
        sendbuf_left = back->sendbuffer(backhandle);
    } while (sendbuf_left);

    bufptr = buffer;
}

void subthd_back_read(char * buf, int len)
{
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

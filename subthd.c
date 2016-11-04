/*
 * platform-independent sub thread executor
 */

#include "putty.h"
#include "subthd.h"
#include "terminal.h"
#include "ldisc.h"

//sending to backend
enum {
    FLUSH_IDLE = 0,
    FLUSH_PENDING,
    FLUSH_PENDING_SPECIAL,
    FLUSH_SENDING
};
static char buffer[10240], 
    *bufptr = buffer, 
    *bufptr_boundary = buffer + sizeof(buffer);
static volatile unsigned char flushing = FLUSH_IDLE;
static volatile Telnet_Special flushing_spec;

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
    while (flushing != FLUSH_IDLE) subthd_sleep(10);
}

void subthd_back_write(char * data, int len)
{
    subthd_wait_for_writing();

    if (bufptr + len >= bufptr_boundary) subthd_back_flush();
    memcpy(bufptr, data, len);
    bufptr += len;
}

void subthd_back_special(Telnet_Special s)
{
    subthd_wait_for_writing();

    flushing_spec = s;
    flushing = FLUSH_PENDING_SPECIAL;
    subthd_wait_for_writing();
}

void subthd_back_flush()
{
    subthd_wait_for_writing();

    if (bufptr == buffer) return;   // if nothign to send, return.
    flushing = FLUSH_PENDING;       // mark as pending to be sent

    subthd_wait_for_writing();      // then wait until wrote
}

int subthd_back_flush_2()
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;
    unsigned char fstatus = flushing;

    switch(fstatus) {
    case FLUSH_SENDING:
        // query status
        if (back->sendbuffer(backhandle) <= 0) flushing = FLUSH_IDLE;
        break;
    
    case FLUSH_PENDING_SPECIAL:
        back->special(backhandle, flushing_spec);
        flushing = FLUSH_IDLE;
        break;

    case FLUSH_PENDING:
        // start send
        back->send(backhandle, buffer, bufptr - buffer);
        bufptr = buffer;
        flushing = FLUSH_SENDING;
        break;

    case FLUSH_IDLE:
        return 0;   // this function does not work yet
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

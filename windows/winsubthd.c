/*
 * winsubthd.c
 * 
 * This module implements some function that subthd.c not implemented.
 */

#include <assert.h>

#include "putty.h"

#include "terminal.h"
#include "subthd.h"

static DWORD WINAPI subthd_win_threadfunc(void *subthd)
{
    struct subthd_tag *s = (struct subthd_tag*)subthd;
    
    s->exitcode = s->func(s->userdata);
    s->running = 0;

    return 0;
}

 void subthd_start(subthd_func func, void *userdata)
{
    DWORD in_threadid;
    struct subthd_tag *s;

    assert(!term->subthd || !term->subthd->running);

    s = snew(struct subthd_tag);
    s->func = func;
    s->userdata = userdata;
    s->running = 1;
    s->handle = (void*)CreateThread(NULL, 0, subthd_win_threadfunc, s, 0, &in_threadid);

    sfree(term->subthd);
    term->subthd = s;
}

void subthd_sleep(unsigned long ms)
{
    Sleep(ms);
}

int subthd_wait(int timeout)
{
    struct subthd_tag *s = term->subthd;
    if (!s || !s->running) return -1;
    return WaitForSingleObject(
        (HANDLE)s->handle, timeout <= 0 ? INFINITE : timeout);
}

struct subthd_mutex_t *subthd_mutex_init(char *name)
{
    struct subthd_mutex_t *mutex = snew(struct subthd_mutex_t);

    mutex->handle = CreateMutex(NULL, FALSE, name);
    mutex->name = snmalloc(1, strlen(name) + 1);
    memcpy(mutex->name, name, strlen(name) + 1);
    
    return mutex;
}

int subthd_mutex_lock(struct subthd_mutex_t *mutex, int timeout)
{
    return WAIT_OBJECT_0 == WaitForSingleObject(
        (HANDLE)mutex->handle, timeout <= 0 ? INFINITE : timeout);
}

void subthd_mutex_unlock(struct subthd_mutex_t *mutex)
{
    ReleaseMutex((HANDLE)mutex->handle);
}

void subthd_mutex_free(struct subthd_mutex_t *mutex)
{
    CloseHandle((HANDLE)mutex->handle);
    sfree(mutex->name);

    sfree(mutex);
}

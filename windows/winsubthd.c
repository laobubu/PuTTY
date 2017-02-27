/*
 * winsubthd.c
 * 
 * This module implements some function that subthd.c not implemented.
 */

#include <assert.h>

#include "putty.h"
#include "subthd.h"
#include "ldisc.h"
#include "terminal.h"

static HANDLE thd2backend_r = NULL, thd2backend_w = NULL; // pipe

// communicate with remote

size_t subthd_back_write(char* data, const size_t len)
{
    SECURITY_ATTRIBUTES saAttr;

    if (!thd2backend_r) {
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        CreatePipe(&thd2backend_r, &thd2backend_w, &saAttr, 0xFFFF); //FIXME: TBD: buffer size enough? flush data?
    }

    size_t written;
    WriteFile(thd2backend_w, data, len, &written, NULL);
    if (written) subthd_knock();

    return written;
}

typedef struct HANDLE_CHAIN_tag {
    HANDLE r, w; // pipe
    struct HANDLE_CHAIN_tag *prev, *next;
} HANDLE_CHAIN;
HANDLE_CHAIN ReadHandleRoot = { NULL, NULL, NULL, NULL };

void subthd_back_on_data_internal(char* data, size_t len)
{
    HANDLE_CHAIN *h = &ReadHandleRoot;
    DWORD writelen;
    while (h = h->next) {
        WriteFile(h->w, data, len, &writelen, NULL);
    }
}

subthd_back_read_handle subthd_back_read_open() // open a buffer to read from backend.
{
    HANDLE_CHAIN *fin = &ReadHandleRoot, *n = snew(HANDLE_CHAIN);
    while (fin->next) fin = fin->next;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    fin->next = n;
    n->prev = fin;
    n->next = NULL;
    CreatePipe(&n->r, &n->w, &saAttr, 0xFFFF); //FIXME: TBD 0xFFFF

    return (subthd_back_read_handle)n;
}

size_t subthd_back_read_read(subthd_back_read_handle h1, char* buf, const size_t size)
{
    HANDLE_CHAIN *h = (HANDLE_CHAIN*)h1;
    size_t readlen = 0;
    ReadFile(h->r, buf, size, &readlen, NULL);

    return readlen;
}

void subthd_back_read_close(subthd_back_read_handle h1)
{
    HANDLE_CHAIN *h = (HANDLE_CHAIN*)h1;
    h->prev->next = h->next;

    CloseHandle(h->r);
    CloseHandle(h->w);
    sfree(h);
}

size_t subthd_back_read_peek(subthd_back_read_handle h1)
{
    HANDLE_CHAIN *h = (HANDLE_CHAIN*)h1;
    DWORD bytesAvail = 0;
    PeekNamedPipe(h->r, NULL, 0, NULL, &bytesAvail, NULL);
    return bytesAvail;
}

void subthd_extra_loop_process_phase2() 
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    if (!ldisc) return;

    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

#define BUFSIZE 2048
    DWORD size_read = 0, bytesAvail = 0;
    static char* buf = NULL;
    if (buf == NULL) buf = malloc(BUFSIZE);

    PeekNamedPipe(thd2backend_r, NULL, 0, NULL, &bytesAvail, NULL);

    if (bytesAvail) {
        while (bytesAvail) {
            size_read = 0;
            ReadFile(thd2backend_r, buf, BUFSIZE, &size_read, NULL);
            if (!size_read) break;
            bytesAvail -= size_read;

            back->send(backhandle, buf, size_read);
        }
        back->sendbuffer(backhandle);
    }
#undef BUFSIZE

    
}

HANDLE subthd_win_knock_sem = NULL;

static DWORD WINAPI subthd_win_threadfunc(void *subthd)
{
    struct subthd_tag *t = (struct subthd_tag*)subthd;
    
    t->exitcode = t->func(t, t->userdata);
    t->running = 0;

    return 0;
}

void subthd_create_phase2(struct subthd_tag * t)
{
    DWORD in_threadid;
    t->handle = (void*)CreateThread(NULL, 0, subthd_win_threadfunc, t, 0, &in_threadid);
}

void subthd_free_phase2(struct subthd_tag *t)
{
    //TODO: kill thread if not exited
}

void subthd_sleep(unsigned long ms)
{
    Sleep(ms);
}

int subthd_wait(struct subthd_tag *t, int timeout)
{
    if (!t || !t->running) return -1;
    return WaitForSingleObject((HANDLE)t->handle, timeout <= 0 ? INFINITE : timeout);
}

// mutex

subthd_mutex_t *subthd_mutex_create(char *name)
{
    subthd_mutex_t *mutex = snew(subthd_mutex_t);
    mutex->handle = (void*) CreateMutex(NULL, FALSE, name);
    mutex->name = snewn(strlen(name) + 1, char);
    strcpy(mutex->name, name);
    return mutex;
}

void subthd_mutex_free(subthd_mutex_t **mutex)
{
    CloseHandle((HANDLE)(*mutex)->handle);
    sfree((*mutex)->name);
    sfree(*mutex);
    *mutex = 0;
}

int subthd_mutex_lock(subthd_mutex_t *mutex, int timeout)
{
    assert(mutex);
    return WAIT_OBJECT_0 == WaitForSingleObject(
        (HANDLE)mutex->handle, timeout <= 0 ? INFINITE : timeout);
}

void subthd_mutex_unlock(subthd_mutex_t *mutex)
{
    assert(mutex);
    ReleaseMutex((HANDLE)mutex->handle);
}

// semaphore

subthd_sem_t *subthd_sem_create(char *name)
{
    subthd_sem_t *sem = snew(subthd_sem_t);
    sem->handle = (void*)CreateSemaphore(NULL, 0, 32767, name);
    sem->name = snewn(strlen(name) + 1, char);
    strcpy(sem->name, name);
    return sem;
}
void subthd_sem_free(subthd_sem_t **sem)
{
    CloseHandle((HANDLE)(*sem)->handle);
    sfree((*sem)->name);
    sfree(*sem);
    *sem = 0;
}

void subthd_sem_post(subthd_sem_t *sem)
{
    assert(sem);
    ReleaseSemaphore((HANDLE)sem->handle, 1, NULL);
}

void subthd_sem_wait(subthd_sem_t *sem)
{
    assert(sem);
    WaitForSingleObject((HANDLE)sem->handle, INFINITE);
}

int subthd_sem_trywait(subthd_sem_t *sem)
{
    assert(sem);
    return WAIT_OBJECT_0 == WaitForSingleObject((HANDLE)sem->handle, 0);
}

// subthread knocks main thread's door, breaking main thread's idle status

HANDLE subthd_win_create_knock_sem() {
    return subthd_win_knock_sem = CreateSemaphore(NULL, 0, 16, "SubThread Knock Semaphore of PuTTY");
}

void subthd_knock()
{
    ReleaseSemaphore(subthd_win_knock_sem, 1, NULL);
}

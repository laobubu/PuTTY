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

// create pipe
static BOOL createAnonymousPipe(HANDLE *r, HANDLE *w)
{
	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	return CreatePipe(r, w, &saAttr, 0xFFFF); //FIXME: TBD: buffer size enough? flush data?
}

// write things to ldisc (display)

static HANDLE thd2ldisc_r = NULL, thd2ldisc_w = NULL; // pipe
size_t subthd_ldisc_write(char* data, const size_t len)
{
	long bytesLeft = len;
	size_t written;

	if (!thd2ldisc_r) createAnonymousPipe(&thd2ldisc_r, &thd2ldisc_w);

	while (bytesLeft > 0) {
		subthd_knock();
		WriteFile(thd2ldisc_w, data, len, &written, NULL);
		bytesLeft -= written;
	}

	return len;
}

// communicate with remote

static HANDLE thd2backend_r = NULL, thd2backend_w = NULL; // pipe
static struct {
	subthd_mutex_t *sending;
	subthd_sem_t *tobesent, *sent;
	Telnet_Special data;
} thd2backed_special = { NULL };

size_t subthd_back_write(char* data, const size_t len)
{
	long bytesLeft = len;
	size_t written;

	if (!thd2backend_r) createAnonymousPipe(&thd2backend_r, &thd2backend_w);

	while (bytesLeft > 0) {
		subthd_knock();
		WriteFile(thd2backend_w, data, len, &written, NULL);
		bytesLeft -= written;
	}

	subthd_knock();

    return len;
}

void subthd_back_write_special(Telnet_Special s)
{
	subthd_mutex_lock(thd2backed_special.sending, -1);

	thd2backed_special.data = s;
	subthd_sem_post(thd2backed_special.tobesent);
	subthd_sem_wait(thd2backed_special.sent);
	subthd_knock();

	subthd_mutex_unlock(thd2backed_special.sending);
}

void subthd_back_flush()
{
	BOOL size_read;
	DWORD bytesAvail;
	subthd_knock();
	while (1) {
		size_read = PeekNamedPipe(thd2backend_r, NULL, 0, NULL, &bytesAvail, NULL);
		if (!size_read || !bytesAvail) return;
	}
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

    n->prev = fin;
    n->next = NULL;
    CreatePipe(&n->r, &n->w, &saAttr, 0xFFFF); //FIXME: TBD 0xFFFF

	fin->next = n;

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

int subthd_back_read_select(subthd_back_read_handle h1, int timeout_ms)
{
	HANDLE_CHAIN *h = (HANDLE_CHAIN*)h1;
	DWORD bytesAvail = 0;
	BOOL success = 0;
	clock_t untilClock = clock() + timeout_ms * CLOCKS_PER_SEC / 1000;
	// Windows doesn't support Wait for Pipes.
	while (clock() < untilClock) {
		success = PeekNamedPipe(h->r, NULL, 0, NULL, &bytesAvail, NULL);
		if (success && bytesAvail) return bytesAvail;
	}
	return 0;
}

void subthd_extra_loop_process_phase2() 
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    if (!ldisc) return;

    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

	// sending special

	if (!thd2backed_special.sending)
	{
		// init
		thd2backed_special.sending = subthd_mutex_create("SubThd Sending Special Mutex");
		thd2backed_special.tobesent = subthd_sem_create("SubThd Sending Special has data to be sent");
		thd2backed_special.sent = subthd_sem_create("SubThd Sending Special data sent");
	}
	else if (subthd_sem_trywait(thd2backed_special.tobesent))
	{
		// get data to be sent
		back->special(backhandle, thd2backed_special.data);
		subthd_sem_post(thd2backed_special.sent);
	}

#define BUFSIZE 4096 //TBD
    DWORD size_read = 0, bytesAvail = 0;
	long gotData = 0;
    static char* buf = NULL;
    if (buf == NULL) buf = malloc(BUFSIZE);

	// sending data to backend

	while (1) {
		size_read = PeekNamedPipe(thd2backend_r, NULL, 0, NULL, &bytesAvail, NULL);
		if (size_read && bytesAvail) {
			if (bytesAvail > BUFSIZE)
				bytesAvail = BUFSIZE;
			size_read = 0;
			ReadFile(thd2backend_r, buf, bytesAvail, &size_read, NULL);
			gotData += size_read;
			if (size_read) back->send(backhandle, buf, size_read);
		} else {
			break;
		}
	}

	// sending data to ldisc

	while (1) {
		size_read = PeekNamedPipe(thd2ldisc_r, NULL, 0, NULL, &bytesAvail, NULL);
		if (size_read && bytesAvail) {
			char *buf2 = malloc(bytesAvail);
			ReadFile(thd2ldisc_r, buf2, bytesAvail, &size_read, NULL);
			if (size_read) from_backend(ldisc->frontend, 0, buf2, size_read);
		} else {
			break;
		}
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
    return subthd_win_knock_sem = CreateSemaphore(NULL, 0, 4, "SubThread Knock Semaphore of PuTTY");
}

void subthd_knock()
{
    ReleaseSemaphore(subthd_win_knock_sem, 1, NULL);
}

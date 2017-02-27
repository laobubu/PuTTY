/*
 * platform-independent sub thread class
 *
 * Note: 
 * 1. This header file contains some declarations whose code
 *    is platform-dependent, and implemented outside `subthd.c`
 * 2. Do NOT directly call the private methods
      whose name ends with "_phase2" or "_internal"
 */

#ifndef __SUBTHD_H__
#define __SUBTHD_H__

typedef int(*subthd_func)(struct subthd_tag *self, void* userdata);
typedef struct { void* handle; char* name; } subthd_mutex_t;
typedef struct { void* handle; char* name; } subthd_sem_t;
typedef void* subthd_back_read_handle;

struct subthd_tag {
    void *handle;      // aka. HANDLE for Windows; pthread_t* for Unix

    subthd_func func;
    void* userdata;

    int running;
    int exitcode;
};

// methods for sub-threads. some methods block the sub-threads.



// methods for PuTTY main thread.

struct subthd_tag * subthd_create(subthd_func func, void *userdata);
void subthd_free(struct subthd_tag** t);

void subthd_extra_loop_process();     // put this into main loop

/* platform-dependent methods ================= */
/* implement these method outside subthd.c */

// internal methods

void subthd_extra_loop_process_phase2(); // write/read remote
void subthd_create_phase2(struct subthd_tag *t);  // called by main thread when creating subthd
void subthd_free_phase2(struct subthd_tag *t);    // called by main thread when destructing subthd

// public util methods

void subthd_sleep(unsigned long ms); // aka. Sleep for windows, usleep for unix
int  subthd_wait(struct subthd_tag *t, int timeout);    // join thread. return non-zero if exited.
                                                        // do NOT forget to subthd_free after using.
// mutex

subthd_mutex_t *subthd_mutex_create(char *name);
void subthd_mutex_free(subthd_mutex_t **mutex);

int  subthd_mutex_lock(subthd_mutex_t *mutex, int timeout); // non-zero if success
void subthd_mutex_unlock(subthd_mutex_t *mutex);

// semaphore

subthd_sem_t *subthd_sem_create(char *name);
void subthd_sem_free(subthd_sem_t **sem);

void subthd_sem_post(subthd_sem_t *sem);
void subthd_sem_wait(subthd_sem_t *sem);
int subthd_sem_trywait(subthd_sem_t *sem); // non-zero if success

// for subthread:
// communicate with backend

size_t subthd_back_write(char*, const size_t); // write data to remote. non-block.

subthd_back_read_handle subthd_back_read_open(); // open a buffer to read from backend.
size_t subthd_back_read_read(subthd_back_read_handle, char*, const size_t);
size_t subthd_back_read_peek(subthd_back_read_handle); // find how many bytes available
void subthd_back_read_close(subthd_back_read_handle);

void subthd_back_on_data_internal(char*, size_t);

// subthread use `knock` to break main thread's waiting phrase
// useful when flushing data to remote.
//
// in Windows, it affects MsgWaitForMultipleObjects window.c:907

void subthd_knock();

/* end of platform-dependent methods ========== */

#endif

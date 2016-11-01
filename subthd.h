/*
 * platform-independent sub thread executor
 */

#ifndef __SUBTHD_H__
#define __SUBTHD_H__

typedef int(*subthd_func)(void *userdata);
struct subthd_mutex_t {
    void *handle;
    char *name;
};

struct subthd_tag {
    void *handle;      // HANDLE for Windows; pthread_t* for linux

    subthd_func func;
    void* userdata;

    int running;
    int exitcode;
};

/* from subthd.c */
void subthd_back_write(char* buf, int len); // process-safe writing-to-end method
void subthd_back_flush();

void subthd_back_read(char* buf, int len);
void subthd_back_read_empty_buf();   // empty bufchain "inbuf2"
int  subthd_back_read_buflen();

/* from platform impl */
void subthd_start(subthd_func func, void *userdata);
void subthd_sleep(unsigned long ms); // Sleep for windows, usleep for unix
int  subthd_wait(int timeout);  // join thread. return non-zero if exited.

struct subthd_mutex_t *subthd_mutex_init(char *name);
void subthd_mutex_free(struct subthd_mutex_t *mutex);
int  subthd_mutex_lock(struct subthd_mutex_t *mutex, int timeout); //get mutex. return non-zero if got
void subthd_mutex_unlock(struct subthd_mutex_t *mutex);

#endif

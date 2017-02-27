/*
 * platform-independent sub thread executor
 */

#include <assert.h>

#include "putty.h"
#include "subthd.h"
#include "terminal.h"
#include "ldisc.h"
#include "misc.h"

// inject extra loop work into the mainloop
// return non-zero if there are unfinished jobs
void subthd_extra_loop_process()
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    if (!ldisc) return;

    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

    subthd_extra_loop_process_phase2();
}

struct subthd_tag * subthd_create(subthd_func func, void *userdata)
{
    struct subthd_tag *t = snew(struct subthd_tag);
    t->func = func;
    t->userdata = userdata;
    t->running = 1;
    subthd_create_phase2(t);

    return t;
}

void subthd_free(struct subthd_tag** t)
{
    subthd_free_phase2(*t);
    sfree(*t);
    *t = 0;
}

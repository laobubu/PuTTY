/*
 * winzmodem.c
 * 
 * This module create a thread handling ZModem.
 */

#include "putty.h"
#include "terminal.h"
#include "ldisc.h"
#include "subthd.h"

#define SZ_STALL_TIME 15

#if defined(WIN32) || defined(_WIN32) 
#define PATH_SEPARATOR '\\'
#else 
#define PATH_SEPARATOR '/' 
#endif 

void xyz_ReceiveInit(Terminal *term);
int xyz_ReceiveData(Terminal *term, const char *buffer, int len);
static int xyz_SpawnProcess(Terminal *term, const char *incommand, const char *inparams);
static int xyz_Check(Backend *back, void *backhandle, Terminal *term, int outerr);

#define MAX_UPLOAD_FILES 512
#define PIPE_SIZE 0

struct zModemInternals {
    PROCESS_INFORMATION pi;
    HANDLE read_stdout;
    HANDLE read_stderr;
    HANDLE write_stdin;
};

static int IsWinNT()
{
    OSVERSIONINFO osv;
    osv.dwOSVersionInfoSize = sizeof(osv);
    GetVersionEx(&osv);
    return (osv.dwPlatformId == VER_PLATFORM_WIN32_NT);
}



void xyz_Done(Terminal *term)
{
    if (term->xyz_transfering != 0) {
        term->xyz_transfering = 0;

        if (term->xyz_Internals) {
            DWORD exitcode = 0;
            CloseHandle(term->xyz_Internals->write_stdin);
            Sleep(500);
            CloseHandle(term->xyz_Internals->read_stdout);
            CloseHandle(term->xyz_Internals->read_stderr);
            GetExitCodeProcess(term->xyz_Internals->pi.hProcess, &exitcode);
            if (exitcode == STILL_ACTIVE || exitcode == STATUS_ABANDONED_WAIT_0) {
                TerminateProcess(term->xyz_Internals->pi.hProcess, 0);
                
                const char canit[] = { 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0};
                
                Sleep(1000);
                
                Ldisc ldisc = ((Ldisc)term->ldisc);
                ldisc->back->send(ldisc->backhandle, canit, sizeof canit);

                const char errmsg[] = "\r[PuTTY] stalling zmodem detected. \r\n";
                from_backend(term, 1, errmsg, sizeof(errmsg) - 1);
            }
            sfree(term->xyz_Internals);
            term->xyz_Internals = NULL;
        }
    }
}

int xyz_Process(Backend *back, void *backhandle, Terminal *term)
{
    return xyz_Check(back, backhandle, term, 0) + xyz_Check(back, backhandle, term, 1);
}

static int xyz_Check(Backend *back, void *backhandle, Terminal *term, int check_stdout)
{
    DWORD exitcode = 0;
    DWORD bread, avail;
    char buf[1024];
    HANDLE h;

    static time_t last_stdout_works = 0;

    if (!term->xyz_transfering) {
        last_stdout_works = time(NULL);
        return 0;
    }

    if (check_stdout) h = term->xyz_Internals->read_stdout;
    else        h = term->xyz_Internals->read_stderr;

    bread = 0;
    PeekNamedPipe(h, buf, 1, &bread, &avail, NULL);
    //check to see if there is any data to read from stdout

    if (bread != 0)
    {
        last_stdout_works = time(NULL);

        while (1)
        {
            bread = 0;

            PeekNamedPipe(h, buf, 1, &bread, &avail, NULL);
            if (bread == 0) return 0;

            if (ReadFile(h, buf, sizeof(buf), &bread, NULL)) { //read the stdout pipe
                if (bread) {
                    if (check_stdout) back->send(backhandle, buf, bread);
                    else from_backend(term, 1, buf, bread);

                    continue;
                }
            }

            // EOF/ERROR
            xyz_Done(term);
            return 1;
        }

        return 1;
    }

    GetExitCodeProcess(term->xyz_Internals->pi.hProcess, &exitcode);

    // if process exited, or no transfer in 4 sec, then terminate the transmission.
    if (exitcode != STILL_ACTIVE || (time(NULL) - last_stdout_works) > SZ_STALL_TIME) {
        xyz_Done(term);
        return 1;
    }

    return 0;
}

void xyz_ReceiveInit(Terminal *term)
{
    char rz_path[MAX_PATH] = "rz.exe"; //FIXME: read from conf
    if (xyz_SpawnProcess(term, rz_path, "") == 0) {
        term->xyz_transfering = 1;
    }
}

#define TIMELIMITED_LOOP_BEGIN(timeout) { \
                                            time_t _tend = time(NULL) + (timeout); \
                                            while (time(NULL) < _tend) {    \
                                                // add your loop stuff here. remember to call break;
#define TIMELIMITED_LOOP_TIMEOUT()          } \
                                            if (_tend < time(NULL)) {  \
                                                // add timeout handler here
#define TIMELIMITED_LOOP_END()              } \
                                        }

// send file with unix xxd command
// assuming term->inbuf2 is enabled and ready
static void xyz_sendFileWithXxd(Terminal *term, char* fn)
{
#define twrite(buf, len) subthd_back_write(buf, len)
#define tputs(str)  twrite(str, strlen(str))
#define failed(message) do { fail_info = message; goto final_step; } while(0)

    char *fail_info = NULL;
    char *filename_base = strrchr(fn, PATH_SEPARATOR) + 1;
    const int block_size = (1024 * 16) * 3; //FIXME: Too big blocks may cause SSH problem (ssh.c:1728)
    const int line_break = 256;  // *4 chars per line. bash cuts long lines :(

    size_t len;
    long fsize, linecnt;
    float _div_fsize_percent;  // = 100.0f / fsize . used to calculate progress
    char *buf;
    char sendtmp[64];
    FILE* fp = fopen(fn, "rb");
    
    time_t next_report_time = 0;
    time_t transfer_since = time(NULL);
    long last_report_fpos = 0;
    const int report_interval = 1;

    subthd_back_read_empty_buf();

    buf = snmalloc(1, block_size);

    fseek(fp, 0, SEEK_END); fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET); _div_fsize_percent = 100.0f / fsize;

    tputs(" stty -echo;\n");
    subthd_back_flush();

	subthd_back_wait(2, 15);

    tputs(" AZps1=$PS1; AZhc1=$HISTCONTROL; PS1='&'; HISTCONTROL=ignorespace; \n");
    tputs(" read AZfn1 && rm -rf \"$AZfn1\" \n");
    subthd_back_read_empty_buf();
    subthd_back_flush();

    while (1) {
        if (!subthd_back_wait(1, 10)) failed("Timeout before FILENAME");
        subthd_back_read(buf, 1); if (*buf == '&') break;
    }
    tputs(filename_base); tputs("\n");
    subthd_back_flush();

    while (1) {
        if (!subthd_back_wait(1, 10)) failed("Timeout after FILENAME");
        subthd_back_read(buf, 1); if (*buf == '&') break;
    }
    tputs("\n AZupl () { printf '\\r\\x8\\r@@@'; base64 -d >>\"$AZfn1\" ; } \n");
	subthd_back_read_empty_buf();
	subthd_back_flush();

    while (!feof(fp)) {
        unsigned char *readtmp = (unsigned char*)buf;

        // start a transfer

		while (1) {
			if (!subthd_back_wait(1, 30)) failed("Timeout before starting a block");
			subthd_back_read(buf, 1); if (*buf == '&') break;
		}
        
        tputs(" AZupl \n");
        subthd_back_read_empty_buf();
        subthd_back_flush();
        
        // wait for continous 3 '@'

        int at_striking = 0;
        while (1) {
            if (!subthd_back_wait(1, 30)) failed("Timeout before block");
            subthd_back_read(buf, 1);
            if (*buf == '@') { if (++at_striking == 3) break; }
            else at_striking = 0;
        }

        // send file part

        len = fread(buf, 1, block_size, fp);
        linecnt = 0;

        while (1) {
            base64_encode_atom(readtmp, (len > 3 ? 3 : len), sendtmp);

            if (++linecnt >= line_break) {
                linecnt = 0;
                twrite("\n", 1);
            }
            twrite(sendtmp, 4);

            if (next_report_time <= time(NULL)) {
                long fpos = (ftell(fp) - len);
                int slen = sprintf(sendtmp,
                    " %.2fkB/s %3d%%",
                    (float)(fpos - last_report_fpos) / 1000.f / (time(NULL) + report_interval - next_report_time),
                    (int)(fpos * _div_fsize_percent)
                );
                memcpy(sendtmp + slen, sendtmp, slen + 1);
                memset(sendtmp, 8, slen);
                from_backend(term, 1, sendtmp, slen * 2);
                next_report_time = time(NULL) + report_interval;
                last_report_fpos = fpos;
            }

            readtmp += 3;
            if (len > 3) len -= 3; else break;
        }

        tputs("\n\x4");
        subthd_back_flush();
    }

    int slen;

final_step:

    if (!fail_info) {
        slen = sprintf(sendtmp,
            " %.2fkB/s (OK)",
            (float)fsize / 1000.f / (time(NULL) - transfer_since)
        );
    } else {
        slen = sprintf(sendtmp, " %s (ERR)", fail_info);
    }
    memcpy(sendtmp + slen * 2, "\r\n", 3);
    memcpy(sendtmp + slen, sendtmp, slen);
    memset(sendtmp, 8, slen);
    from_backend(term, 1, sendtmp, slen * 2);

    tputs("\x3\x3");
    tputs(" PS1=$AZps1; HISTCONTROL=$AZhc1; printf \"\\r\"; stty echo; \n");
    subthd_back_flush();
    fclose(fp);
    sfree(buf);

#undef twrite
#undef tputs

}

static void xyz_StartSending_sz(Terminal *term, char* fns);
static int xyz_sending_thread(void* userdata);

// accept: GTK file list style. see term_drop()
void xyz_StartSending(Terminal *term, char* fns)
{
    int method = conf_get_int(term->conf, CONF_zm_drop_send_method);

    if (method == SEND_WITH_LRZSZ) {
        xyz_StartSending_sz(term, fns);
    } 

    if (method == SEND_WITH_SHELL) {
        // filename list will be freed, hence we need a copy.
        int fns_len = strlen(fns) + 1;
        void *fns_copy = malloc(fns_len);
        memcpy(fns_copy, fns, fns_len);

        subthd_start(xyz_sending_thread, fns_copy);
    }
}

int xyz_sending_thread(void* userdata)
{
    char* fns = (char*)userdata;
    char* fnprev = fns; // the beginning of current filename
    int fnlen;

    term->inbuf2_enabled = 1;
    subthd_sleep(200);

    while (1) {
        if ((fns = strchr(fnprev, '\n')) == NULL) break; // "\n\0" or 
        if (fns == fnprev) break;           // "\n\n" as the end of list

        if (memcmp(fnprev, "file://", 7) == 0) fnprev += 7; // skip prefix
        *fns = '\0';
        fnlen = fns - fnprev;

        from_backend(term, 1, "\r\nSending ", 10);
        from_backend(term, 1, fnprev, strlen(fnprev));
        from_backend(term, 1, "\r\n\x8\x8\x8\x8", 6);

        xyz_sendFileWithXxd(term, fnprev);

        *fns = '\n';
        fnprev = ++fns;
    }

    subthd_sleep(200);
    term->inbuf2_enabled = 0;

    subthd_back_write("\n", 1);
    subthd_back_flush();

    free(userdata);

    return 0;
}

void xyz_StartSending_sz(Terminal *term, char* fns)
{
    char *sz_path = conf_get_str(term->conf, CONF_zm_sz);
    char sz_full_params[32767], *param_ptr = sz_full_params;

    char* fnprev = fns; // the beginning of current filename
    int fnlen;

    while (1) { // construct a string like:   "1.txt" "2.bin"
        if ((fns = strchr(fnprev, '\n')) == NULL) fns = strchr(fnprev, '\0');
        else if (fns == fnprev) break; // "\n\n" as the end

        if (memcmp(fnprev, "file://", 7) == 0) fnprev += 7; // skip prefix
        fnlen = fns - fnprev;

        *param_ptr++ = '"';
        memcpy(param_ptr, fnprev, fnlen);
        param_ptr += fnlen;
        *param_ptr++ = '"';
        *param_ptr++ = ' ';

        if (*fns == '\0') break;
        fnprev = ++fns;
    }
    *param_ptr = '\0';

    if (xyz_SpawnProcess(term, sz_path, sz_full_params) == 0) {
        term->xyz_transfering = 1;
    }
}

void xyz_Cancel(Terminal *term)
{
    xyz_Done(term);
}

static int xyz_SpawnProcess(Terminal *term, const char *incommand, const char *inparams)
{
    STARTUPINFO si;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;               //security information for pipes

    HANDLE 
        read_stdout = NULL,
        read_stderr = NULL,
        write_stdin = NULL,
        newstdin = NULL,
        newstdout = NULL,
        newstderr = NULL
        ; //pipe handles

    term->xyz_Internals = (struct zModemInternals *)smalloc(sizeof(struct zModemInternals));
    memset(term->xyz_Internals, 0, sizeof(struct zModemInternals));

    if (IsWinNT())        //initialize security descriptor (Windows NT)
    {
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        sa.lpSecurityDescriptor = &sd;
    }

    else sa.lpSecurityDescriptor = NULL;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;         //allow inheritable handles

    if (
        !CreatePipe(&newstdin, &write_stdin, &sa, PIPE_SIZE)  ||    //create stdin pipe
        !CreatePipe(&read_stdout, &newstdout, &sa, PIPE_SIZE) ||    //create stdout pipe
        !CreatePipe(&read_stderr, &newstderr, &sa, PIPE_SIZE)       //create stdout pipe
       )
    {
        CloseHandle(newstdin);
        CloseHandle(write_stdin);
        CloseHandle(newstdout);
        CloseHandle(read_stdout);
        return 1;
    }

    GetStartupInfo(&si);      //set startupinfo for the spawned process
                              /*
                              The dwFlags member tells CreateProcess how to make the process.
                              STARTF_USESTDHANDLES validates the hStd* members. STARTF_USESHOWWINDOW
                              validates the wShowWindow member.
                              */

    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = newstdout;
    si.hStdError = newstderr;     //set the new handles for the child process
    si.hStdInput = newstdin;


    //system
    if (!DuplicateHandle(GetCurrentProcess(), read_stdout, GetCurrentProcess(), &term->xyz_Internals->read_stdout, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(newstdin);
        CloseHandle(write_stdin);
        CloseHandle(newstdout);
        CloseHandle(read_stdout);
        CloseHandle(newstderr);
        CloseHandle(read_stderr);
        return 1;
    }

    CloseHandle(read_stdout);

    if (!DuplicateHandle(GetCurrentProcess(), read_stderr, GetCurrentProcess(), &term->xyz_Internals->read_stderr, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(newstdin);
        CloseHandle(newstdout);
        CloseHandle(read_stdout);
        CloseHandle(write_stdin);
        CloseHandle(newstderr);
        CloseHandle(read_stderr);
        return 1;
    }

    CloseHandle(read_stderr);

    if (!DuplicateHandle(GetCurrentProcess(), write_stdin, GetCurrentProcess(), &term->xyz_Internals->write_stdin, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(newstdin);
        CloseHandle(write_stdin);
        CloseHandle(newstdout);
        CloseHandle(term->xyz_Internals->read_stdout);
        CloseHandle(newstderr);
        CloseHandle(term->xyz_Internals->read_stderr);
        return 1;
    }

    CloseHandle(write_stdin);

    //spawn the child process
    {
        const char download_dir[] = "C:/"; //FIXME:

        char command[10240];
        sprintf(command, "%s %s", incommand, inparams);

        if (!CreateProcess(
            NULL, command, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL,
            download_dir, &si, &term->xyz_Internals->pi))
        {
            //DWORD err = GetLastError();
            //		ErrorMessage("CreateProcess");
            CloseHandle(newstdin);
            CloseHandle(term->xyz_Internals->write_stdin);
            CloseHandle(newstdout);
            CloseHandle(term->xyz_Internals->read_stdout);
            CloseHandle(newstderr);
            CloseHandle(term->xyz_Internals->read_stderr);
            return 1;
        }
    }

    CloseHandle(newstdin);
    CloseHandle(newstdout);
    CloseHandle(newstderr);

    return 0;
}

int xyz_ReceiveData(Terminal *term, const char *buffer, int len)
{
    DWORD written;
    WriteFile(term->xyz_Internals->write_stdin, buffer, len, &written, NULL);

    return 0;
}

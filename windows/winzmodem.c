/*
 * winzmodem.c
 * 
 * This module create a thread handling ZModem.
 */

#include "putty.h"
#include "terminal.h"
#include "ldisc.h"

#define SZ_STALL_TIME 5

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
#define PIPE_SIZE (64*1024)

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

                const char errmsg[] = "\r[PuTTY] stalling zmodem detected. ";
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

    static time_t last_stdout_works = NULL;

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

// send file with unix xxd command
static void xyz_termSend(Terminal *term, char* fn)
{
    Ldisc ldisc = ((Ldisc)term->ldisc);
    Backend *back = ldisc->back;
    void *backhandle = ldisc->backhandle;

#define twrite(buf, len) back->send(backhandle, buf, len)
#define tputs(str)  twrite(str, strlen(str))
    
    char *filename_base = strrchr(fn, PATH_SEPARATOR) + 1;
    
    size_t len;
    char buf[1024];
    char sendtmp[3];
    FILE* fp = fopen(fn, "rb");

    tputs("xxd -r -ps >");
    tputs(filename_base);
    tputs(" <<EOF\n");

    while (!feof(fp)) {
        char *readtmp = buf;

        len = fread(buf, 1, sizeof buf, fp);
        while (len--) {
            sprintf(sendtmp, "%.2x", *readtmp++);
            tputs(sendtmp);
        }
    }

    tputs("\nEOF\n");

#undef twrite
#undef tputs

}

void xyz_StartSending(Terminal *term, char* fns)
{
#if 0
    char* fnprev = fns; // the beginning of current filename
    int fnlen;

    while (1) {
        if ((fns = strchr(fnprev, '\n')) == NULL) fns = strchr(fnprev, '\0');
        else if (fns == fnprev) break; // "\n\n" as the end

        if (memcmp(fnprev, "file://", 7) == 0) fnprev += 7; // skip prefix
        *fns = '\0';
        fnlen = fns - fnprev;

        from_backend(term, 1, "\rSending ", 9);
        from_backend(term, 1, fnprev, strlen(fnprev));
        from_backend(term, 1, "   ", 3);

        xyz_termSend(term, fnprev);

        if (*fns == '\0') break;
        fnprev = ++fns;
    }

#else

    char sz_path[MAX_PATH] = "sz.exe -v"; //FIXME: read from conf
    char sz_full_params[32767], *param_ptr = sz_full_params;

    filename_to_str(conf_get_filename(term->conf, CONF_zm_rz));

    char* fnprev = fns; // the beginning of current filename
    int fnlen;

    while (1) {
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

#endif
}

void xyz_Cancel(Terminal *term)
{
    xyz_Done(term);
}

static int xyz_SpawnProcess(Terminal *term, const char *incommand, const char *inparams)
{
#if 0
    // see https://msdn.microsoft.com/en-us/library/windows/desktop/ms682499(v=vs.85).aspx

    SECURITY_ATTRIBUTES saAttr;

    struct zModemInternals *xyz;
    xyz = (struct zModemInternals *)smalloc(sizeof(struct zModemInternals));
    memset(xyz, 0, sizeof(struct zModemInternals));
    term->xyz_Internals = xyz;

    // Set the bInheritHandle flag so pipe handles are inherited. 

    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_IN_Wr = NULL;   // for host
    HANDLE g_hChildStd_OUT_Rd = NULL;  // for host
    HANDLE g_hChildStd_OUT_Wr = NULL;
    HANDLE g_hChildStd_ERR_Rd = NULL;  // for host
    HANDLE g_hChildStd_ERR_Wr = NULL;

    // Create a pipe for the child process's STDOUT. 
    // then Ensure the read handle to the pipe for STDOUT is not inherited.

    if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0)) return -1;
    if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) return -2;

    if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0)) return -3;
    if (!SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) return -4;

    if (!CreatePipe(&g_hChildStd_ERR_Rd, &g_hChildStd_ERR_Wr, &saAttr, 0)) return -5;
    if (!SetHandleInformation(g_hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0)) return -6;

    // Spawn thread

    PROCESS_INFORMATION piProcInfo;
    STARTUPINFO siStartInfo;
    BOOL bSuccess = FALSE;

    // Set up members of the PROCESS_INFORMATION structure. 

    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    // Set up members of the STARTUPINFO structure. 
    // This structure specifies the STDIN and STDOUT handles for redirection.

    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
    siStartInfo.cb = sizeof(STARTUPINFO);
    siStartInfo.hStdError = g_hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
    siStartInfo.hStdInput = g_hChildStd_IN_Rd;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    // Create the child process. 

    char command[4096];
    sprintf(command, "%s %s", incommand, inparams);

    bSuccess = CreateProcess(NULL,
        command,       // command line 
        NULL,          // process security attributes 
        NULL,          // primary thread security attributes 
        TRUE,          // handles are inherited 
        0,             // creation flags 
        NULL,          // use parent's environment 
        NULL,          // use parent's current directory 
        &siStartInfo,  // STARTUPINFO pointer 
        &piProcInfo);  // receives PROCESS_INFORMATION 

    if (!bSuccess)  // If an error occurs, exit the application. 
        return -7;
    else
    {
        // Close handles to the child process and its primary thread.
        // Some applications might keep these handles to monitor the status
        // of the child process, for example. 

        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
    }
#endif
#if 1
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
#endif
}

int xyz_ReceiveData(Terminal *term, const char *buffer, int len)
{
    DWORD written;
    WriteFile(term->xyz_Internals->write_stdin, buffer, len, &written, NULL);

    return 0;
}

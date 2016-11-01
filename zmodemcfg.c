/*
 * zmodemcfg.c - the ZModem and Drag'n'drop specific parts of the 
 * PuTTY configuration box. Centralised as cross-platform code because
 * more than one platform will want to use it, but not part of the
 * main configuration. The expectation is that each platform's
 * local config function will call out to ser_setup_config_box() if
 * it needs to set up the standard serial stuff. (Of course, it can
 * then apply local tweaks after ser_setup_config_box() returns, if
 * it needs to.)
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"

#ifdef _WIN32
#define EXEC_SUFFIX ".exe"
#else
#define EXEC_SUFFIX ""
#endif

#define EXEC_FILE_FILTER(name) (\
    name "\0" name "\0" \
    "Any Executable (*" EXEC_SUFFIX ")\0*" EXEC_SUFFIX "\0" \
	"All Files (*.*)\0*\0\0\0")

void zm_setup_config_box(struct controlbox *b, int midsession)
{
    struct controlset *s;
    
    /*
     * Entirely new Terminal/File Transfer panel
     */
    const char panel_name[] = "Terminal/File Transfer";
    ctrl_settitle(b, panel_name, "Transfer files by zmodem or drag'n'drop");

    s = ctrl_getset(b, panel_name, "zmdrag", "Drag and drop");
    ctrl_radiobuttons(s, "Send files with", 'm', 1,
        HELPCTX(logging_flush),
        conf_radiobutton_handler,
        I(CONF_zm_drop_send_method),
        "lrzsz", I(SEND_WITH_LRZSZ),
        "Send as base64, then decode remotely.(*)", I(SEND_WITH_SHELL),
        NULL);
    ctrl_text(s, "(*) Slow. Supports most Unix. Tested on sh/bash.", P(NULL));

    s = ctrl_getset(b, panel_name, "zmxyz", "X/Y/ZModem");
    ctrl_filesel(s, "Local rz" EXEC_SUFFIX "  (from lrzsz)", 'r',
        EXEC_FILE_FILTER("rz" EXEC_SUFFIX), FALSE, "Select rz binary",
        P(NULL),
        conf_filesel_handler, I(CONF_zm_rz));
    ctrl_text(s, " \"sz" EXEC_SUFFIX "\" shall be under the same directory.", P(NULL));
    ctrl_checkbox(s, "Automatically start receiving", 's',
        HELPCTX(logging_flush),
        conf_checkbox_handler, I(CONF_zm_autorecv));

    s = ctrl_getset(b, panel_name, "zmremote", "Remote execution");
    ctrl_editbox(s, "Before a transmission", 'e', 100,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_sendcmd), I(1));
    ctrl_editbox(s, "After a transmission", 'x', 100,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_sendcmd_post), I(1));
    ctrl_text(s, "(Use &N for file name)", P(NULL));
}

/*
 * zmodemcfg.c - the ZModem and Drag'n'drop specific parts of the 
 * PuTTY configuration box. 
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"

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

    s = ctrl_getset(b, panel_name, "zmxyz", "Local X/Y/ZModem command");
    ctrl_editbox(s, "Send", 's', 85,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_sz), I(1));
    ctrl_editbox(s, "Recv", 'r', 85,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_rz), I(1));
    ctrl_checkbox(s, "Automatically start receiving from lrzsz", 't',
        HELPCTX(logging_flush),
        conf_checkbox_handler, I(CONF_zm_autorecv));

    s = ctrl_getset(b, panel_name, "zmremote", "Remote command");
    ctrl_editbox(s, "Before receiving", 'e', 100,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_sendcmd), I(1));
    ctrl_text(s, "(Hint: You may want to execute \"rz\" remotely)", P(NULL));
    ctrl_editbox(s, "After receiving", 'x', 100,
        HELPCTX(logging_flush),
        conf_editbox_handler, I(CONF_zm_sendcmd_post), I(1));
    ctrl_text(s, "(Use &N for file name)", P(NULL));
}

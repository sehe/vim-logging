#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "vim.h"

#define CMDLOG_VERSION_STR "vim72-logging 0.03"

static FILE *cmdlogfile;

const char *ord_to_str[] = {
    "<NUL>", "<C-A>", "<C-B>", "<C-C>", "<C-D>", "<C-E>", "<C-F>", "<C-G>",
    "<C-H>", "<C-I>", " ", "<C-K>", "<C-L>", "<C-M>", "<C-N>", "<C-O>",
    "<C-P>", "<C-Q>", "<C-R>", "<C-S>", "<C-T>", "<NL>", "<C-V>", "<C-W>",
    "<C-X>", "<C-Y>", "<C-Z>", "<ESC>", "<C-\\>", "<C-]>", "<C-^>", "<C-_>",
    " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",", "-",
    ".", "/", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<",
    "=", ">", "?", "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
    "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "[", "\\", "]", "^", "_", "`", "a", "b", "c", "d", "e", "f", "g", "h", "i",
    "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "{", "|", "}", "~"
};

const char *cmdlog_get_state_str(int state) {
    switch (state) {
        case NORMAL: return "NORMAL";
        case VISUAL: return "VISUAL";
        case OP_PENDING: return "OP_PENDING";
        case CMDLINE: return "CMDLINE";
        case INSERT: return "INSERT";
        case LANGMAP: return "LANGMAP";
        case REPLACE_FLAG: return "REPLACE_FLAG";
        case REPLACE: return "REPLACE";
        case VREPLACE_FLAG: return "VREPLACE_FLAG";
        case VREPLACE: return "VREPLACE";
        case LREPLACE: return "LREPLACE";
        case NORMAL_BUSY: return "NORMAL_BUSY";
        case HITRETURN: return "HITRETURN";
        case ASKMORE: return "ASKMORE";
        case SETWSIZE: return "SETWSIZE";
        case ABBREV: return "ABBREV";
        case EXTERNCMD: return "EXTERNCMD";
        case SHOWMATCH: return "SHOWMATCH";
        case CONFIRM: return "CONFIRM";
        case SELECTMODE: return "SELECTMODE";
    }
    return "UNKNOWN";
}

// Push one character into cmdlog buf, performing all the appropriate checks
// and converting the character to its escape sequence if nessicary (ie,
// convert '\1' to "C-A").
// This will never result in an overflow.
// _cmdlog_buf will always be null-terminated.
#define _CMDLOG_BUF_SIZE 512
char _cmdlog_buf[_CMDLOG_BUF_SIZE];
int _cmdlog_buf_pos = 0;
void _cmdlog_buf_putch(int c) {
    const char *chstr;
    char _chstr_helper[16];

    if (c >= 0 && c < 127) {
        chstr = ord_to_str[c];
    } else if (c == 0xFFFF9D95) { // Backspace?
        chstr = "<BS>";
    } else if (c == 0xFFFF9395) { // Delete?
        chstr = "<DEL>";
    } else {
        vim_snprintf(_chstr_helper, sizeof(_chstr_helper), "<0x%X>", c);
        chstr = _chstr_helper;
    }

    while (*chstr != '\0' && _cmdlog_buf_pos < _CMDLOG_BUF_SIZE-1) {
        _cmdlog_buf[_cmdlog_buf_pos++] = *chstr++;
    }

    _cmdlog_buf[_cmdlog_buf_pos] = '\0';
}

int _cmdlog_last_state = -1;
void cmdlog_flush_buf() {
    // If we're on the first state change, just do nothing
    if (_cmdlog_last_state == -1)
        goto done;

    // Beacuse normal.c and ex_docmd.c can force us to flush, occasionally we
    // are asked to flush when the cmdlog buf is empty. In this case it would
    // be silly to write a line... So we don't.
    if (_cmdlog_buf[0] == '\0')
        goto done;

    if (!ensure_cmdlog_file_is_open())
        return;

    // Flush the current buffer
    char_u *filetype = curbuf->b_p_ft;
    if (filetype[0] == '\0')
        filetype = (char_u *)"unknown";

    struct timeval tv;
    gettimeofday(&tv, NULL);

    char msg[_CMDLOG_BUF_SIZE+64];
    vim_snprintf(msg, sizeof(msg), "%d.%d %s %s %s\n", tv.tv_sec, tv.tv_usec,
                 filetype, cmdlog_get_state_str(_cmdlog_last_state),
                 _cmdlog_buf);
    fputs(msg, cmdlogfile);

    done:
    // Reset the internal state of everything
    _cmdlog_last_state = State;
    _cmdlog_buf_pos = 0;
    _cmdlog_buf[0] = '\0';
}

void cmdlog_gotchar(int c) {
    // If our state has changed, flush the previous state
    if (State != _cmdlog_last_state) {
        cmdlog_flush_buf();
    }

    if (Exec_reg == TRUE)
        return;

    if (!p_cli && State == INSERT)
        return;

    _cmdlog_buf_putch(c);
}

// Read a uid/gid from the environment, returning the id or -1
int id_from_env(const char *name) {
    int id;
    char *idstr;

    idstr = getenv(name);
    if (idstr == NULL)
        return -1;

    id = strtol(idstr, NULL, 10);
    if (errno != 0)
        return -1;

    return id;
}

int ensure_cmdlog_file_is_open() {
    if (p_cld == NULL || p_cld[0] == '\0')
        return 0;

    if (cmdlogfile != NULL)
        return 1;

    size_t logpath_size = strlen((char *)p_cld) + 32;
    char logpath[logpath_size];
    vim_snprintf((char *)logpath, logpath_size, "%s/cmd%d.log", p_cld, getpid());
    cmdlogfile = fopen(logpath, "a");
    //strcpy(logpath, "/tmp/cmdlog"); // For testing
    //cmdlogfile = fopen(logpath, "w"); // For testing

    // Did everything break?
    static int errcount = 0;
    if (cmdlogfile == NULL) {
        if (errcount < 3) {
            char err[logpath_size+32];
            vim_snprintf(err, logpath_size+32, "Cannot open cmdlog %s", logpath);
            emsg((char_u *)err);
            errcount += 1;
            if (errcount == 3)
                emsg((char_u *)"Surpressing further cmdlog messages.");
        }
        return 0;
    }

    // If vim is being run as root using sudo, chown the logfile so it is owned
    // by the "real" user (ie, not root).
    if (geteuid() == 0) {
        int sudo_uid = id_from_env("SUDO_UID");
        int sudo_gid = id_from_env("SUDO_GID");
        fchown(fileno(cmdlogfile), sudo_uid, sudo_gid);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Print a bit of information about vim-logging
    fprintf(cmdlogfile, "%d.%d none VIM-LOGGING %s\n", tv.tv_sec, tv.tv_usec,
            CMDLOG_VERSION_STR);

    return 1;
}


#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <wchar.h>


#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif
#ifndef NCURSES_WIDECHAR
#define NCURSES_WIDECHAR 1
#endif
#if defined(__has_include)
# if __has_include(<ncursesw/curses.h>)
#  include <ncursesw/curses.h>
# else
#  include <ncurses.h>
# endif
#else
# include <ncurses.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <pty.h>
#include <time.h>
#include <unistd.h>

#define APP_VERSION "1.40"
#define PATHBUF 512
#define MAXEXTRA 8
#define MAXARGV 96

/* 退出码，和 bash 版保持一致 */
#define EXIT_OK 0
#define EXIT_USER_CANCEL 1
#define EXIT_MISS_DEPEND 2
#define EXIT_FILE_MISS 3
#define EXIT_FILE_CORRUPT 4
#define EXIT_QEMU_FAIL 5
#define EXIT_INVALID_PARAM 6

typedef struct {
    char name[64];
    char arch[16];          /* x86_64 / i386 / aarch64 / ppc */
    char machine[32];
    char cpu[32];
    int  cores, threads;
    int  mem;               /* MB */
    int  mem_auto;
    char gpu[32];           /* -device 名，空表示不加 */
    char audio[24];         /* none / hda-duplex / ac97 / ... */
    char if_type[16];       /* virtio / ide / scsi / ... */
    int  nvirt;
    char firmware[16];      /* default / uefi / none */
    char bios_file[PATHBUF];
    char code_file[PATHBUF];
    char vars_file[PATHBUF];
    char hda[PATHBUF];
    char hda2[PATHBUF];
    char cdrom[PATHBUF];
    char cdrom2[PATHBUF];
    char fda[PATHBUF];
    char fdb[PATHBUF];
    int  shared;
    char shared_path[PATHBUF];
    char nic[40];
    int  net_user;
    int  ssh_port;
    char dns1[32];
    char dns2[32];
    char boot[8];           /* c / d / a / n，空表示默认 */
    int  boot_menu;
    char display[16];       /* vnc / spice / sdl / gtk / none */
    int  vnc_ext;           /* 0 = unix socket，1 = TCP */
    char vnc_addr[32];
    int  vnc_port;
    char vnc_pass[32];
    int  spice_port;
    int  localtime;
    int  overcommit;
    char args[512];
} VM;

static char HOME_DIR[PATHBUF];
static char ROOT_DIR[PATHBUF];   /* ~/.vvm：现在只放共用的固件，按需创建 */
static char CONF_DIR[PATHBUF];  /* ~/start */
static struct { char name[64]; char dir[PATHBUF]; } g_rdir[64];
static int g_rdir_n;
static char PREFIX_DIR[PATHBUF];
static VM vm;

/* ------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------ */
static void trim(char *s) {
    char *p = s;
    size_t n;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int file_exists(const char *p) {
    return p && *p && access(p, R_OK) == 0;
}

static int dir_exists(const char *p) {
    struct stat st;
    if (!p || !*p) return 0;
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static void ensure_dir(const char *p) {
    char tmp[PATHBUF];
    size_t i, n;
    snprintf(tmp, sizeof tmp, "%s", p);
    n = strlen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void fmt_bytes(char *out, size_t n, long long bytes) {
    if (bytes < 1024) snprintf(out, n, "%lld B", bytes);
    else if (bytes < 1024LL * 1024) snprintf(out, n, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) snprintf(out, n, "%.1f MB", bytes / 1048576.0);
    else snprintf(out, n, "%.1f GB", bytes / 1073741824.0);
}

/* ------------------------------------------------------------------
 * ncurses 基础组件
 * ------------------------------------------------------------------ */
static int dwidth(const char *s);
static void put_right(WINDOW *w, int y, int right, const char *s, int attr);

static unsigned g_grey = A_DIM; /* 浅灰：有 16 色时用 bright black，否则退化成 dim */
                                /* 用 unsigned 存属性位，和 COLOR_PAIR() 的类型对齐 */

static void force_palette(void) {
    /* 自己把 16 色调成固定值，换主题也不会变样；终端不支持改色就静默跳过 */
    static const int pal[16][3] = {
        {0x12,0x12,0x16}, {0xff,0x55,0x55}, {0x4e,0xc9,0x73}, {0xf2,0xc0,0x47},
        {0x42,0x85,0xf4}, {0xc7,0x61,0xd7}, {0x4c,0xd0,0xc0}, {0xd6,0xd9,0xe3},
        {0x62,0x66,0x72}, {0xcb,0x4b,0x16}, {0x66,0xdd,0x88}, {0xff,0xd0,0x60},
        {0x63,0x9f,0xff}, {0xd4,0x7d,0xe2}, {0x60,0xe0,0xd0}, {0xff,0xff,0xff}
    };
    int i;
    if (!can_change_color()) return;
    for (i = 0; i < 16; i++)
        init_color((short)i,
                   (short)(pal[i][0] * 1000 / 255),
                   (short)(pal[i][1] * 1000 / 255),
                   (short)(pal[i][2] * 1000 / 255));
}

static void on_term_signal(int sig);

static void ui_init(void) {
    const char *t = getenv("TERM");
    /* 没终端/终端太简陋（dumb），curses 画不出来也读不到键，直接说明白再退，
       不然会空转一屏乱码 */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "需要在终端里运行（不是管道/后台）\n");
        exit(EXIT_INVALID_PARAM);
    }
    if (!t || !*t || strstr(t, "dumb")) {
        fprintf(stderr, "终端类型不对（TERM=%s），试试：export TERM=xterm-256color\n", t ? t : "未设置");
        exit(EXIT_INVALID_PARAM);
    }
    setlocale(LC_ALL, "");          /* 必须，否则中文按字节算宽度会错位 */
    initscr();
    set_escdelay(80);   /* Esc 别等 1 秒，但也要够慢终端把方向键整串送完 */
    if (has_colors()) {
        int f, b;
        start_color();
        use_default_colors();
        for (f = -1; f < 8; f++)
            for (b = -1; b < 8; b++)
                init_pair(8 + (f + 1) * 9 + (b + 1), f, b);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    /* 有些终端/输入法发的是「普通模式」方向键（CSI 形式），
       terminfo 里只认应用模式（SS3），不补上就会被当成一个孤立的 Esc 直接退页 */
    define_key("\033[A", KEY_UP);
    define_key("\033[B", KEY_DOWN);
    define_key("\033[C", KEY_RIGHT);
    define_key("\033[D", KEY_LEFT);
    define_key("\033[H", KEY_HOME);
    define_key("\033[F", KEY_END);
    define_key("\033[1~", KEY_HOME);
    define_key("\033[2~", KEY_IC);
    define_key("\033[3~", KEY_DC);
    define_key("\033[4~", KEY_END);
    define_key("\033[5~", KEY_PPAGE);
    define_key("\033[6~", KEY_NPAGE);
    curs_set(0);
    signal(SIGINT, on_term_signal);
    signal(SIGTERM, on_term_signal);
    signal(SIGHUP, on_term_signal);
    if (has_colors()) {
        start_color();
        use_default_colors();
        force_palette();
        init_pair(1, COLOR_BLUE, -1);    /* 标题 */
        init_pair(2, COLOR_YELLOW, -1);  /* 提示 */
        init_pair(3, COLOR_RED, -1);     /* 错误 */
        init_pair(4, COLOR_GREEN, -1);   /* 成功 */
        init_pair(5, COLOR_CYAN, -1);    /* 选中行 */
        init_pair(6, COLOR_BLACK, COLOR_WHITE); /* 选中整行 */
        init_pair(7, COLOR_WHITE, COLOR_RED);   /* 错误状态条 */
        if (COLORS >= 16) {
            init_pair(90, 8, -1);               /* 8 = bright black，就是浅灰 */
            init_pair(95, 15, -1);              /* 15 = 纯白，给 [INFO] 这类标签 */
            g_grey = COLOR_PAIR(90);
        }
    }
}

static void ui_done(void) {
    endwin();
}

/* 按显示宽度（不是字节）输出 UTF-8；maxw<=0 表示不限制 */
static int put_u8(WINDOW *w, int y, int x, const char *s, int maxw, int attr) {
    wchar_t buf[4096];
    size_t n, i;
    int used = 0;
    if (!s) return 0;
    n = mbstowcs(buf, s, sizeof buf / sizeof buf[0] - 1);
    if (n == (size_t)-1) {          /* 转换失败就按字节画，至少不崩 */
        mvwaddnstr(w, y, x, s, maxw > 0 ? maxw : getmaxx(w) - x - 1);
        return 0;
    }
    buf[n] = L'\0';
    wmove(w, y, x);
    if (attr) wattron(w, attr);
    for (i = 0; i < n; i++) {
        int cw = wcwidth(buf[i]);
        if (cw < 0) cw = 1;
        if (maxw > 0 && used + cw > maxw) break;
        waddnwstr(w, &buf[i], 1);
        used += cw;
    }
    if (attr) wattroff(w, attr);
    return used;
}

static WINDOW *g_top;      /* 当前这一屏用的窗口，确认框直接画上去 */
static void close_win(WINDOW *w) { if (g_top == w) g_top = NULL; delwin(w); }

static WINDOW *mkwin(const char *title, int h, int w) {
    WINDOW *win;
    int y, x;
    if (h > LINES - 1) h = LINES - 1;
    if (w > COLS - 1) w = COLS - 1;
    if (h < 5) h = 5;
    if (w < 20) w = 20;
    y = (LINES - h) / 2;
    x = (COLS - w) / 2;
    werase(stdscr);
    wrefresh(stdscr);
    win = newwin(h, w, y, x);
    if (!win) return NULL;
    g_top = win;
    /* 和主界面一个样式：不画框，标题下面一条细横线 */
    if (title && *title) {
        put_u8(win, 0, 1, title, w - 2, A_BOLD);
        wattron(win, g_grey);
        mvwhline(win, 1, 0, ACS_HLINE, w);
        wattroff(win, g_grey);
    }
    return win;
}

static void win_destroy(WINDOW *w) {
    if (!w) return;
    wborder(w, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ');
    wrefresh(w);
    close_win(w);
    touchwin(stdscr);
    refresh();
}


/* ---- 底部状态条：不弹窗，错误整行红 ---- */
static char g_status[512];
static time_t g_status_at;
static int g_status_err;
static int g_keep_menu;          /* 1 = 这层菜单先留着窗口，调用方自己关（删除要问话时用） */
static WINDOW *g_menu_w;

static void status_set(int err, const char *txt) {
    snprintf(g_status, sizeof g_status, "%s", txt ? txt : "");
    if (txt && (strstr(txt, "删除") || strstr(txt, "删掉") || strstr(txt, "清空")))
        err = 2;                                  /* 2 = 危险提示，红字 */
    else if (!err && txt) {
        if (strstr(txt, "已") || strstr(txt, "成功") || strstr(txt, "完成") ||
            strstr(txt, "就绪") || strstr(txt, "继续"))
            err = 4;                              /* 4 = 成功，绿字 */
        else if (strstr(txt, "失败") || strstr(txt, "错误") || strstr(txt, "不能") ||
                 strstr(txt, "无法") || strstr(txt, "不存在") || strstr(txt, "缺少") ||
                 strstr(txt, "拒绝"))
            err = 1;                              /* 1 = 错误，红底白字 */
    }
    { size_t l = strlen(g_status); while (l && g_status[l - 1] == '\n') g_status[--l] = '\0'; }
    { size_t i; for (i = 0; g_status[i]; i++) if (g_status[i] == '\n') g_status[i] = ' '; }
    g_status_at = time(NULL);
    g_status_err = err;
}

static void status_clear(void) { g_status[0] = '\0'; g_status_err = 0; }

static int status_active(void) {
    return g_status[0] && time(NULL) - g_status_at <= 4;
}

/* 画状态条，没状态就画普通提示 */
static void status_or_hint_at(WINDOW *w, int y, const char *hint) {
    int wid = getmaxx(w);
    int pad = (getbegx(w) >= 0 && y == getmaxy(w) - 2 && getmaxx(w) < COLS) ? 1 : 0;
    if (status_active()) {
        mvwhline(w, y, pad, ' ', wid - pad * 2);
        if (g_status_err == 2) {                  /* 危险动作：红字，不铺红底 */
            put_u8(w, y, 1 + pad, g_status, wid - 2 - pad * 2, COLOR_PAIR(3) | A_BOLD);
        } else if (g_status_err == 4) {            /* 成功：绿字 */
            put_u8(w, y, 1 + pad, g_status, wid - 2 - pad * 2, COLOR_PAIR(4) | A_BOLD);
        } else if (g_status_err == 5) {            /* 停止类提示：黄字 */
            put_u8(w, y, 1 + pad, g_status, wid - 2 - pad * 2, COLOR_PAIR(2) | A_BOLD);
        } else if (g_status_err) {
            wattron(w, COLOR_PAIR(7) | A_BOLD);
            mvwhline(w, y, pad, ' ', wid - pad * 2);
            put_u8(w, y, 1 + pad, g_status, wid - 2 - pad * 2, COLOR_PAIR(7) | A_BOLD);
            wattroff(w, COLOR_PAIR(7) | A_BOLD);
        } else {
            put_u8(w, y, 1 + pad, g_status, wid - 2 - pad * 2, A_BOLD);
        }
    } else if (hint && *hint) {
        mvwhline(w, y, pad, ' ', wid - pad * 2);      /* 状态消失后要把旧字擦掉 */
        put_u8(w, y, 1 + pad, hint, wid - 2 - pad * 2, g_grey);
    } else {
        mvwhline(w, y, pad, ' ', wid - pad * 2);
    }
}

static void ui_msg(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    int err;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    err = strstr(buf, "失败") || strstr(buf, "错误") || strstr(buf, "缺少") ||
          strstr(buf, "不存在") || strstr(buf, "无法") || strstr(buf, "不能") ||
          strstr(buf, "不是") || strstr(buf, "异常") || strstr(buf, "拒绝") ||
          strstr(buf, "Error") || strstr(buf, "error");
    if (strstr(buf, "已停止")) { status_set(5, "[已停止]"); return; }
    status_set(err ? 1 : 0, buf);
}

static int ui_confirm(const char *fmt, ...) {
    char buf[1024], line[1024];
    va_list ap;
    int ch;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    { size_t i; for (i = 0; buf[i]; i++) if (buf[i] == '\n') buf[i] = ' '; }
    snprintf(line, sizeof line, "%s [y] 确认 [n] 取消", buf);
    {
        /* 超过一行会被终端折到下一行，底行就糊了：截断加省略号 */
        int lim = COLS > 20 ? COLS : 80;
        if ((int)strlen(line) > lim - 1) {
            size_t cut = (size_t)(lim - 2);
            while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;   /* 别把 UTF-8 切一半 */
            line[cut] = 0;
            snprintf(line + strlen(line), sizeof line - strlen(line), "…");
        }
    }

    /* 直接写终端：curses 的 refresh() 会拿空白的 stdscr 把整屏擦掉，
       确认框就变成"另一个空界面"了。这里只动最底下一行，页面原样留着 */
    {
        FILE *tty = fopen("/dev/tty", "w");
        int yes = 0;
        if (tty) {
            fprintf(tty, "\033[%d;1H\033[0m\033[K\033[7m%s\033[0m", LINES, line);
            fflush(tty);
        }
        /* 必须是 wgetch 到"这一屏"的窗口上：
           对 stdscr 调 getch 时，ncurses 会顺手把空白的 stdscr 刷一遍，
           整屏就被擦掉了 */
        if (g_top) wtimeout(g_top, -1);
        while ((ch = wgetch(g_top ? g_top : stdscr)) != ERR) {
            if (ch == 'y' || ch == 'Y') { yes = 1; break; }
            /* 回车/空格按“不改”处理，误碰也不会把东西删掉 */
            if (ch == 'n' || ch == 'N' || ch == 27 || ch == 'q' ||
                ch == ' ' || ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
        }
        if (tty) { fclose(tty); }
        /* 那一行是绕过 curses 直接写的，它自己不知道；
           强制整窗重画一次，页面和底行提示就都回来了 */
        if (g_top) { clearok(g_top, TRUE); wrefresh(g_top); }
        status_clear();
        return yes;
    }
}

/* 输入框：默认值可编辑（Backspace 删），回车确认，Esc 取消 */
static int ui_input(const char *title, const char *prompt, const char *def,
                    char *out, int outlen, int secret) {
    char buf[PATHBUF];
    char line[PATHBUF * 2];
    int len, ok = 0;
    wint_t ch;
    FILE *tty;
    WINDOW *tw = g_top ? g_top : stdscr;

    snprintf(buf, sizeof buf, "%s", def ? def : "");
    len = (int)strlen(buf);
    curs_set(1);
    keypad(tw, TRUE);
    wtimeout(tw, -1);          /* 别继承主页那 300ms 的轮询超时，否则输入时一直被 ERR 打断 */
    tty = fopen("/dev/tty", "w");

    while (1) {
        const char *vis = buf;
        char show[PATHBUF], ptext[200];
        int pcol, vcol, avail;
        size_t skip = 0;

        if (secret) {
            int i;
            for (i = 0; i < len && i < PATHBUF - 1; i++) show[i] = '*';
            show[len] = '\0';
            vis = show;
        }
        /* 提示太长就只留「标题：」，宽度全给正在输入的字 */
        if ((int)dwidth(prompt) > COLS - 10) {
            if (title && *title) snprintf(ptext, sizeof ptext, "%s：", title);
            else snprintf(ptext, sizeof ptext, "输入：");
        } else {
            snprintf(ptext, sizeof ptext, "%s", prompt);
        }
        pcol = (int)dwidth(ptext);
        avail = COLS - 2 - pcol;
        if (avail < 4) avail = 4;
        while ((int)dwidth(vis + skip) > avail) {           /* 值太长只显示尾部 */
            skip++;
            while (vis[skip] && (vis[skip] & 0xC0) == 0x80) skip++;
        }
        snprintf(line, sizeof line, "%s", "");
        if (tty) {
            /* 只动最后一行：提示黄、内容亮白，页面一个像素都不碰 */
            fprintf(tty, "\033[%d;1H\033[0m\033[K\033[33m%s\033[0m\033[1m", LINES, ptext);
            fflush(tty);
        }
        vcol = pcol + (secret ? len : (int)dwidth(vis + skip));
        if (vcol > COLS - 2) vcol = COLS - 2;
        if (vcol < 0) vcol = 0;
        if (tty) {
            /* 值用亮白写出来 */
            fprintf(tty, "%s\033[0m\033[%d;%dH", secret ? show : (vis + skip), LINES, vcol + 1);
            fflush(tty);
        }

        if (wget_wch(tw, &ch) == ERR) continue;
        if (ch == 27) { ok = 0; break; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) { ok = 1; break; }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                while (len > 0 && (buf[len] & 0xC0) == 0x80) len--;
                buf[len] = '\0';
            }
            continue;
        }
        if (ch == KEY_RESIZE) continue;
        if (ch >= KEY_MIN && ch <= KEY_MAX) continue;   /* 方向键等特殊键 */
        if (ch < 32) continue;
        {
            char mb[8];
            size_t mlen = wcrtomb(mb, (wchar_t)ch, NULL);
            if (mlen == (size_t)-1 || mlen == 0) continue;
            if (len + (int)mlen < outlen - 1 && len + (int)mlen < PATHBUF - 1) {
                memcpy(buf + len, mb, mlen);
                len += (int)mlen;
                buf[len] = '\0';
            }
        }
    }
    curs_set(0);
    if (tty) {
        fprintf(tty, "\033[%d;1H\033[K", LINES);
        fflush(tty);
        fclose(tty);
    }
    /* 最后一行是绕过 curses 写的，强制把整页重画回来 */
    if (g_top) { clearok(g_top, TRUE); wrefresh(g_top); }
    if (ok) { snprintf(out, outlen, "%s", buf); trim(out); }
    return ok;
}

/* 就地编辑：在列表某一行上直接输入，不跳新页面。Enter 提交，Esc 取消 */
static int inline_text(WINDOW *w, int y, int x, const char *init,
                       char *out, int outlen, int secret, int digits) {
    char buf[PATHBUF];
    int len, maxw = getmaxx(w) - x - 2;
    wint_t wc;
    int fresh = 1;          /* 直接敲字就替换旧值，按退格才改旧值 */

    snprintf(buf, sizeof buf, "%s", init ? init : "");
    len = (int)strlen(buf);
    curs_set(1);
    while (1) {
        char show[PATHBUF];
        int i;
        if (secret) {
            for (i = 0; i < len && i < PATHBUF - 1; i++) show[i] = '*';
            show[len > 0 ? len : 0] = '\0';
        } else snprintf(show, sizeof show, "%s", buf);
        wmove(w, y, x);
        wclrtoeol(w);
        if (maxw > 0) put_u8(w, y, x, show, maxw, A_BOLD);
        {
            int cx = x + (int)dwidth(show);
            if (cx > getmaxx(w) - 2) cx = getmaxx(w) - 2;
            wmove(w, y, cx);
        }
        wrefresh(w);

        if (wget_wch(w, &wc) == ERR) { if (getmaxx(w) != COLS) { } continue; }
        if (wc == 27) { curs_set(0); return 0; }
        if (wc == '\n' || wc == '\r' || wc == KEY_ENTER) {
            curs_set(0);
            snprintf(out, outlen, "%s", buf);
            trim(out);
            return 1;
        }
        if (wc == 0x15) { len = 0; buf[0] = '\0'; fresh = 1; continue; }   /* Ctrl+U 清空 */
        if (wc == KEY_BACKSPACE || wc == 127 || wc == 8) {
            fresh = 0;
            if (len > 0) {
                len--;
                while (len > 0 && (buf[len] & 0xC0) == 0x80) len--;
                buf[len] = '\0';
            }
            continue;
        }
        if (wc >= KEY_MIN && wc <= KEY_MAX) continue;
        if (wc < 32) continue;
        if (digits && !(wc >= L'0' && wc <= L'9')) continue;   /* 数字项就别让它打进去 */
        {
            char mb[8];
            size_t mlen = wcrtomb(mb, (wchar_t)wc, NULL);
            if (mlen == (size_t)-1 || mlen == 0) continue;
            if (fresh) { len = 0; buf[0] = '\0'; fresh = 0; }
            if (len + (int)mlen < outlen - 1 && len + (int)mlen < PATHBUF - 1) {
                memcpy(buf + len, mb, mlen);
                len += (int)mlen;
                buf[len] = '\0';
            }
        }
    }
}

/* 通用列表菜单，返回下标，Esc/q 返回 -1 */
/* 带"删除/强制/覆盖"字样的菜单项一律标红，手别抖 */
static int item_danger(const char *s) {
    return s && (strstr(s, "删除") || strstr(s, "删掉") || strstr(s, "清空") ||
                 strstr(s, "覆盖") || strstr(s, "强制") || strstr(s, "丢弃"));
}
static unsigned item_attr(const char *s) { return item_danger(s) ? COLOR_PAIR(3) | A_BOLD : 0; }

static int ui_menu_hdr(const char *title, const char *header,
                      const char *const *items, int n) {
    WINDOW *w;
    int top = 0, sel = 0, ch, i;
    int h = LINES - 4, wd = COLS > 82 ? 82 : COLS - 2;
    int rows, hdr_lines = 0, list_y;

    /* 先算头部行数：按换行切 */
    if (header && *header) {
        const char *p = header;
        hdr_lines = 1;
        while ((p = strchr(p, '\n')) != NULL) { hdr_lines++; p++; }
    }
    if (h > n + hdr_lines + 6) h = n + hdr_lines + 6;
    if (h < 7 + hdr_lines) h = 7 + hdr_lines;
    if (h > LINES - 2) h = LINES - 2;
    w = mkwin(title, h, wd);

    if (header && *header) {
        wattron(w, g_grey);
        const char *p = header;
        int ln = 0;
        char linebuf[PATHBUF * 2];
        int maxlen = getmaxx(w) - 3;
        if (maxlen < 4) maxlen = 4;              /* 窄终端下别把负数当 size_t 用 */
        while (ln < hdr_lines) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len > (size_t)maxlen) len = (size_t)maxlen;
            memcpy(linebuf, p, len);
            linebuf[len] = '\0';
            put_u8(w, 2 + ln, 2, linebuf, getmaxx(w) - 4, 0);
            ln++;
            if (!nl) break;
            p = nl + 1;
        }
        wattroff(w, g_grey);
    }
    list_y = 2 + hdr_lines + 1;
    rows = getmaxy(w) - list_y - 2;
    keypad(w, TRUE);

    {
        int need = 1, last_act = -1;
        while (1) {
            if (need) {
                for (i = 0; i < rows; i++) {
                    int idx = top + i;
                    mvwhline(w, list_y + i, 1, ' ', getmaxx(w) - 2);
                    if (idx >= n) continue;
                    if (idx == sel) {
                        wattron(w, COLOR_PAIR(6));
                        mvwhline(w, list_y + i, 1, ' ', getmaxx(w) - 2);
                        put_u8(w, list_y + i, 2, items[idx], getmaxx(w) - 5, COLOR_PAIR(6) | A_BOLD);
                        wattroff(w, COLOR_PAIR(6));
                    } else {
                        put_u8(w, list_y + i, 2, items[idx], getmaxx(w) - 5, item_attr(items[idx]));
                    }
                }
                {
                    const char *hint = "↑↓ [选择]   Enter [确认]   Esc [返回]";
                    if (dwidth(hint) > getmaxx(w) - 18) hint = "↑↓ Enter Esc 返回";
                    status_or_hint_at(w, getmaxy(w) - 2, hint);
                }
                if (n > rows && !status_active()) {
                    char hint[32];
                    snprintf(hint, sizeof hint, "%d/%d", sel + 1, n);
                    put_right(w, getmaxy(w) - 2, getmaxx(w) - 2, hint, 0);
                }
                wrefresh(w);
                need = 0;
                last_act = status_active();
            }

            wtimeout(w, 350);
            ch = wgetch(w);
            if (ch == ERR) {
                int act = status_active();
                if (act != last_act) { need = 1; last_act = act; }
                continue;
            }
            if (ch == 27 || ch == 'q') { sel = -1; break; }
            if (ch == KEY_UP || ch == 'k' || ch == 'K') {
                int old = sel;
                if (sel > 0) sel--;
                if (sel < top) { top = sel; need = 1; }
                else if (old != sel) {
                    mvwhline(w, list_y + old - top, 1, ' ', getmaxx(w) - 2);
                    put_u8(w, list_y + old - top, 2, items[old], getmaxx(w) - 5, item_attr(items[old]));
                    wattron(w, COLOR_PAIR(6));
                    mvwhline(w, list_y + sel - top, 1, ' ', getmaxx(w) - 2);
                    put_u8(w, list_y + sel - top, 2, items[sel], getmaxx(w) - 5, COLOR_PAIR(6) | A_BOLD);
                    wattroff(w, COLOR_PAIR(6));
                    wrefresh(w);
                }
            } else if (ch == KEY_DOWN || ch == 'j' || ch == 'J') {
                int old = sel;
                if (sel < n - 1) sel++;
                if (sel >= top + rows) { top = sel - rows + 1; need = 1; }
                else if (old != sel) {
                    mvwhline(w, list_y + old - top, 1, ' ', getmaxx(w) - 2);
                    put_u8(w, list_y + old - top, 2, items[old], getmaxx(w) - 5, item_attr(items[old]));
                    wattron(w, COLOR_PAIR(6));
                    mvwhline(w, list_y + sel - top, 1, ' ', getmaxx(w) - 2);
                    put_u8(w, list_y + sel - top, 2, items[sel], getmaxx(w) - 5, COLOR_PAIR(6) | A_BOLD);
                    wattroff(w, COLOR_PAIR(6));
                    wrefresh(w);
                }
            } else if (ch == 'g' || ch == KEY_HOME) {
                sel = 0; top = 0; need = 1;
            } else if (ch == 'G' || ch == KEY_END) {
                sel = n - 1; top = sel - rows + 1; if (top < 0) top = 0; need = 1;
            } else if (ch == KEY_PPAGE) {
                sel -= rows; if (sel < 0) sel = 0;
                top = sel;
                need = 1;
            } else if (ch == KEY_NPAGE) {
                sel += rows; if (sel > n - 1) sel = n - 1;
                if (sel >= top + rows) top = sel - rows + 1;
                need = 1;
            } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                break;
            }
        }
    }
    if (g_keep_menu) { g_keep_menu = 0; g_menu_w = w; return sel; }
    win_destroy(w);
    return sel;
}

static int ui_menu(const char *title, const char *const *items, int n) {
    return ui_menu_hdr(title, NULL, items, n);
}

/* 文本翻页查看 */
static void ui_pager(const char *title, char *const *lines, int n) {
    WINDOW *w;
    int top = 0, ch, rows, i;
    w = mkwin(title, LINES - 2, COLS - 2);
    rows = getmaxy(w) - 4;
    keypad(w, TRUE);
    while (1) {
        for (i = 0; i < rows; i++) {
            int idx = top + i;
            mvwhline(w, 2 + i, 1, ' ', getmaxx(w) - 2);
            if (idx < n) put_u8(w, 2 + i, 1, lines[idx], getmaxx(w) - 3, 0);
        }
        put_u8(w, getmaxy(w) - 2, 2, "↑↓/PgUp/PgDn [滚动]   q/Esc [返回]", 0, 0);
        wrefresh(w);
        ch = wgetch(w);
        if (ch == 27 || ch == 'q') break;
        if (ch == KEY_UP && top > 0) top--;
        if (ch == KEY_DOWN && top < n - rows) top++;
        if (ch == KEY_PPAGE) { top -= rows; if (top < 0) top = 0; }
        if (ch == KEY_NPAGE) { top += rows; if (top > n - rows) top = n - rows > 0 ? n - rows : 0; }
    }
    win_destroy(w);
}

/* ------------------------------------------------------------------
 * 文件选择器
 * ------------------------------------------------------------------ */
typedef struct {
    char name[256];
    int isdir;
    long long size;
} FEnt;

static int ent_cmp(const void *a, const void *b) {
    const FEnt *x = a, *y = b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* 目录优先 */
    return strcasecmp(x->name, y->name);
}

static int ext_match(const char *name, const char *ext) {
    size_t nl, el;
    if (!ext || !*ext) return 1;
    nl = strlen(name);
    el = strlen(ext);
    if (nl < el) return 0;
    return strcasecmp(name + nl - el, ext) == 0;
}

static int scan_dir(const char *dir, const char *ext, FEnt **out) {
    DIR *d;
    struct dirent *e;
    FEnt *list = NULL;
    int n = 0, cap = 0;

    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char full[PATHBUF];
        struct stat st;
        if (!strcmp(e->d_name, ".")) continue;
        if (!strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !ext_match(e->d_name, ext)) continue;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (n + 1 >= cap) {
                int ncap = cap ? cap * 2 : 64;
                FEnt *nl = realloc(list, ncap * sizeof(FEnt));
                if (!nl) break;                 /* 扩不动就先把已读到的交出去，别丢指针 */
                list = nl;
                cap = ncap;
            }
            snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
            list[n].isdir = 1;
            list[n].size = 0;
            n++;
        } else if (S_ISREG(st.st_mode) && ext_match(e->d_name, ext)) {
            if (n + 1 >= cap) {
                int ncap = cap ? cap * 2 : 64;
                FEnt *nl = realloc(list, ncap * sizeof(FEnt));
                if (!nl) break;
                list = nl;
                cap = ncap;
            }
            snprintf(list[n].name, sizeof list[n].name, "%s", e->d_name);
            list[n].isdir = 0;
            list[n].size = (long long)st.st_size;
            n++;
        }
    }
    closedir(d);
    if (list) qsort(list, n, sizeof(FEnt), ent_cmp);
    *out = list;
    return n;
}


/* 底行常驻提示：不切屏、不阻塞，用来显示“正在保存内存… 已 N 秒” */
static void tty_line_msg(const char *msg) {
    FILE *tty = fopen("/dev/tty", "w");
    char line[520];
    snprintf(line, sizeof line, " %s", msg);
    if (tty) {
        fprintf(tty, "\033[%d;1H\033[0m\033[K\033[33m%.*s\033[0m", LINES,
                (int)(COLS > 2 ? COLS - 1 : COLS), line);
        fflush(tty);
        fclose(tty);
    }
}

static void tty_line_clear(void) {
    if (g_top) { clearok(g_top, TRUE); wrefresh(g_top); }
}

static void flash_err(const char *msg) {
    FILE *tty = fopen("/dev/tty", "w");
    char line[520];
    snprintf(line, sizeof line, " %s", msg);
    if (tty) {
        /* 红底白字，只占最后一行；不触碰页面，页面保持原样 */
        fprintf(tty, "\033[%d;1H\033[0m\033[K\033[7m%.*s\033[0m", LINES,
                (int)(COLS > 2 ? COLS - 1 : COLS), line);
        fflush(tty);
        fclose(tty);
    }
    napms(700);
    if (g_top) { clearok(g_top, TRUE); wrefresh(g_top); }
}

static int ui_input_port(const char *what, int *val) {
    char t[32];
    snprintf(t, sizeof t, "%d", *val > 0 ? *val : 2222);
    while (1) {
        if (!ui_input(what, "端口（1024-65535，0 关闭）：", t, t, sizeof t, 0)) return 0;
        if (!t[0] || !strcmp(t, "0")) { *val = 0; return 1; }
        {
            const char *p;
            int ok = 1, v;
            for (p = t; *p; p++) if (*p < '0' || *p > '9') { ok = 0; break; }
            v = atoi(t);
            if (!ok) { flash_err("端口只能填数字"); continue; }
            if (v < 1024 || v > 65535) { flash_err("端口要在 1024-65535"); continue; }
            *val = v;
            return 1;
        }
    }
}
static int ui_browse(const char *title, const char *start, const char *ext,
                     char *out, int outlen, int dir_mode) {
    char cur[PATHBUF];
    int ret = 0;
    snprintf(cur, sizeof cur, "%s", (start && dir_exists(start)) ? start : HOME_DIR);

    while (1) {
        FEnt *list = NULL;
        int n, i, sel = 0, ch, top = 0;
        char **items;
        char status[PATHBUF * 2];
        WINDOW *w;
        int rows;

        int base;
        n = scan_dir(cur, ext, &list);
        items = calloc(n + 3, sizeof(char *));
        base = dir_mode ? 2 : 1;
        items[0] = strdup("..");
        if (dir_mode) {
            char sel_cur[PATHBUF * 2];
            snprintf(sel_cur, sizeof sel_cur, "就用当前目录：%s", cur);
            items[1] = strdup(sel_cur);
        }
        for (i = 0; i < n; i++) {
            char line[400];
            char sz[32];
            if (list[i].isdir)
                snprintf(line, sizeof line, "  %s/", list[i].name);
            else {
                fmt_bytes(sz, sizeof sz, list[i].size);
                snprintf(line, sizeof line, "  %s    (%s)", list[i].name, sz);
            }
            items[i + base] = strdup(line);
        }
        snprintf(status, sizeof status, "当前目录：%s%s", cur,
                 dir_mode ? "    （选目录：Enter 进下一层；选「就用当前目录」即确定；p 手输路径）" : "");

        w = mkwin(title, LINES - 2, COLS - 2);
        put_u8(w, 1, 2, status, getmaxx(w) - 4, 0);
        rows = getmaxy(w) - 5;
        keypad(w, TRUE);

        while (1) {
            for (i = 0; i < rows; i++) {
                int idx = top + i;
                mvwhline(w, 3 + i, 1, ' ', getmaxx(w) - 2);
                if (idx >= n + base) continue;
                if (idx == sel) {
                    wattron(w, A_REVERSE);
                    mvwhline(w, 3 + i, 1, ' ', getmaxx(w) - 2);
                    put_u8(w, 3 + i, 1, items[idx], getmaxx(w) - 3, 0);
                    wattroff(w, A_REVERSE);
                } else {
                    put_u8(w, 3 + i, 1, items[idx], getmaxx(w) - 3, 0);
                }
            }
            put_u8(w, getmaxy(w) - 2, 2, "↑↓ 选择  Enter 进入/选中  p 手输路径  Esc 取消", 0, 0);
            wrefresh(w);

            ch = wgetch(w);
            if (ch == 27 || ch == 'q') goto done;
            if (ch == KEY_UP && sel > 0) { sel--; if (sel < top) top = sel; }
            else if (ch == KEY_DOWN && sel < n + base - 1) { sel++; if (sel >= top + rows) top = sel - rows + 1; }
            else if (ch == 'p' || ch == 'P') {
                char tmp[PATHBUF];
                snprintf(tmp, sizeof tmp, "%s", cur);
                if (ui_input("手动输入路径", "直接输入完整路径（可留空取消）", tmp, tmp, sizeof tmp, 0)) {
                    if (dir_exists(tmp)) snprintf(cur, sizeof cur, "%s", tmp);
                    else if (file_exists(tmp)) {
                        snprintf(out, outlen, "%s", tmp);
                        ret = 1;
                        goto done;
                    }
                }
            } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                if (sel == 0) {
                    char *slash = strrchr(cur, '/');
                    if (slash && slash != cur) *slash = '\0';
                    else if (slash == cur) cur[1] = '\0';
                    top = 0; sel = 0;
                    break;
                } else if (dir_mode && sel == 1) {
                    snprintf(out, outlen, "%s", cur);
                    ret = 1;
                    goto done;
                } else {
                    char full[PATHBUF];
                    int li = sel - base;
                    snprintf(full, sizeof full, "%s/%s", cur, list[li].name);
                    if (list[li].isdir) {
                        snprintf(cur, sizeof cur, "%s", full);
                        top = 0; sel = 0;
                        break;
                    } else {
                        snprintf(out, outlen, "%s", full);
                        ret = 1;
                        goto done;
                    }
                }
            }
        }
        win_destroy(w);
        for (i = 0; i < n + 3; i++) free(items[i]);
        free(items);
        free(list);
        continue;
done:
        win_destroy(w);
        for (i = 0; i < n + 3; i++) free(items[i]);
        free(items);
        free(list);
        return ret;
    }
}

/* ------------------------------------------------------------------
 * 配置读写
 * ------------------------------------------------------------------ */
static void vm_defaults(VM *v) {
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof v->name, "default");
    snprintf(v->arch, sizeof v->arch, "x86_64");
    snprintf(v->machine, sizeof v->machine, "pc");
    snprintf(v->cpu, sizeof v->cpu, "qemu64");
    v->cores = 2;
    v->threads = 1;
    v->mem = 2048;
    v->mem_auto = 0;
    snprintf(v->gpu, sizeof v->gpu, "std");
    snprintf(v->audio, sizeof v->audio, "none");
    snprintf(v->if_type, sizeof v->if_type, "virtio");
    snprintf(v->firmware, sizeof v->firmware, "default");
    snprintf(v->nic, sizeof v->nic, "virtio-net-pci");
    v->net_user = 1;
    v->ssh_port = 2222;
    snprintf(v->dns1, sizeof v->dns1, "223.5.5.5");
    snprintf(v->dns2, sizeof v->dns2, "8.8.8.8");
    v->boot[0] = '\0';
    v->boot_menu = 0;
    snprintf(v->display, sizeof v->display, "vnc");
    v->vnc_ext = 1;
    snprintf(v->vnc_addr, sizeof v->vnc_addr, "127.0.0.1");
    v->vnc_port = 5901;
    v->spice_port = 6999;
    v->localtime = 1;
}

/* 把旧的 sh / vm 配置文件挪进 config/ */
static void migrate_configs(void) {
    DIR *d;
    struct dirent *e;
    d = opendir(ROOT_DIR);            /* ~/.vvm 可能压根不存在，别为此跳过下面的 ~/config */
    while (d && (e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        char from[PATHBUF], to[PATHBUF];
        if (l < 4) continue;
        if (strcmp(e->d_name + l - 3, ".sh") && strcmp(e->d_name + l - 3, ".vm")) continue;
        snprintf(from, sizeof from, "%s/%s", ROOT_DIR, e->d_name);
        snprintf(to, sizeof to, "%s/%s", CONF_DIR, e->d_name);
        if (access(to, F_OK) != 0) rename(from, to);
    }
    if (d) closedir(d);

    /* 老版本的 ~/config/ 搬到 ~/start */
    {
        char old[PATHBUF];
        snprintf(old, sizeof old, "%s/config", HOME_DIR);
        d = opendir(old);
        if (!d) return;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            char from[PATHBUF], to[PATHBUF];
            if (l < 4) continue;
            if (strcmp(e->d_name + l - 3, ".sh") && strcmp(e->d_name + l - 3, ".vm")) continue;
            snprintf(from, sizeof from, "%s/%s", old, e->d_name);
            snprintf(to, sizeof to, "%s/%s", CONF_DIR, e->d_name);
            if (access(to, F_OK) != 0) rename(from, to);
        }
        closedir(d);
    }
}

static int vm_load(const char *name);

/* 运行文件跟着主磁盘走：日志、pid、挂起/暂停标记都放 qcow2 同目录 */
/* 运行目录 = <磁盘同目录>/<磁盘名去后缀>/：磁盘文件搬到哪，pid/日志/socket 就跟到哪。
   没有磁盘、或磁盘目录写不进去时，退回 ~/start/<VM名>/，不再往 ~/.vvm 塞东西。 */
static void run_dir_of_disk(const char *disk, const char *name, char *out, int n) {
    char d[PATHBUF], stem[PATHBUF], safe[64];
    const char *b;
    char *dot;
    int i;

    out[0] = '\0';
    if (disk && disk[0]) {
        snprintf(d, sizeof d, "%s", disk);
        { char *sl = strrchr(d, '/'); if (sl) *sl = '\0'; }
        b = strrchr(disk, '/');
        b = b ? b + 1 : disk;
        snprintf(stem, sizeof stem, "%s", b);
        dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = '\0';
        if (!stem[0]) snprintf(stem, sizeof stem, "disk");
        if (d[0] && access(d, W_OK) == 0) {
            {   const char *db = strrchr(d, '/');
                db = db ? db + 1 : d;
                if (!strcmp(db, stem)) {          /* 磁盘已经躺在同名文件夹里，就用这个文件夹 */
                    snprintf(out, n, "%s", d);
                    if (access(out, W_OK) == 0) return;
                }
            }
            snprintf(out, n, "%s/%s", d, stem);
            ensure_dir(out);
            if (access(out, W_OK) == 0) return;
        }
    }
    if (!name || !name[0]) { snprintf(out, n, "%s", CONF_DIR); return; }   /* 名字都没定，别乱建目录 */
    snprintf(safe, sizeof safe, "%s", name);
    for (i = 0; safe[i]; i++) if (safe[i] == '/' || safe[i] == '\\') safe[i] = '_';
    snprintf(out, n, "%s/%s", CONF_DIR, safe);
    ensure_dir(out);
}

static void runtime_dir_for(const char *name, char *out, int n) {
    VM bak = vm;
    char d[PATHBUF], nm[64];        /* name 常常是 &vm.name 本身，vm_load 会把那块内存清空，先拷出来 */
    int i;
    if (!name || !name[0]) { snprintf(out, n, "%s", CONF_DIR); return; }
    snprintf(nm, sizeof nm, "%s", name);
    for (i = 0; i < g_rdir_n; i++) {
        if (strcmp(g_rdir[i].name, nm) == 0) { snprintf(out, n, "%s", g_rdir[i].dir); return; }
    }
    vm_load(nm);
    run_dir_of_disk(vm.hda, nm, d, sizeof d);
    vm = bak;
    if (!d[0]) snprintf(d, sizeof d, "%s/%s", CONF_DIR, nm);
    snprintf(out, n, "%s", d);
}

static void conf_path(const char *name, char *out, int n) {
    snprintf(out, n, "%s/%s.sh", CONF_DIR, name);
}

static void conf_path_legacy(const char *name, char *out, int n) {
    snprintf(out, n, "%s/%s.vm", CONF_DIR, name);
}

static void copyfield(char *dst, size_t n, const char *val) {
    snprintf(dst, n, "%s", val ? val : "");
}

static void sh_q(FILE *f, const char *s) {
    fputc('\'', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\'') fputs("'\\''", f);
        else if (c < 32 || c == 127) fputc(' ', f);   /* 换行/制表/控制字符写进去会把配置写坏 */
        else fputc(*s, f);
    }
    fputc('\'', f);
}

/* sh_q 的反操作：剥外层引号，再把 '\'' 还原成 ' */
static void sh_unquote(char *s) {
    size_t l = strlen(s);
    char *p;

    if (l >= 2 && ((s[0] == '\'' && s[l - 1] == '\'') || (s[0] == '"' && s[l - 1] == '"'))) {
        memmove(s, s + 1, l - 2);
        s[l - 2] = '\0';
    }
    while ((p = strstr(s, "'\\''")) != NULL) {
        memmove(p + 1, p + 4, strlen(p + 4) + 1);
        *p = '\'';
    }
}

/* 注释行里的 #KEY=VALUE 只有当键名是纯 ASCII 标识符时才当老配置读，
   否则 "# 完整启动命令（一行字符串）" 这种注释会被误认成配置项 */
static int key_plain(const char *k) {
    int i;
    if (!k || !k[0] || strlen(k) > 24) return 0;
    for (i = 0; k[i]; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= 'A' && k[i] <= 'Z') ||
              (k[i] >= '0' && k[i] <= '9') || k[i] == '_')) return 0;
    return 1;
}

static void serial_sock_path(const char *name, char *out, int n);

static int disk_has_suspend(const char *hda);

static int vm_build_argv(const VM *v, char **argv, int maxargv,
                         char pool[][PATHBUF], int pooln, int background,
                         const char *serial_dev);

static int vm_save(VM *v) {
    char path[PATHBUF];
    char pool[MAXARGV][PATHBUF];
    char *argv[MAXARGV];
    int argc, i;
    FILE *f;

    ensure_dir(CONF_DIR);
    conf_path(v->name, path, sizeof path);
    argc = vm_build_argv(v, argv, MAXARGV, pool, MAXARGV, 0, NULL);

    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "#!/usr/bin/env sh\n");
    fprintf(f, "# vvm 生成：直接执行 = 启动这台虚拟机；source 它 = 只取配置和命令行\n");
    {
        char safe[PATHBUF];
        int i;
        snprintf(safe, sizeof safe, "%s", path);
        for (i = 0; safe[i]; i++) if ((unsigned char)safe[i] < 32) safe[i] = ' ';
        fprintf(f, "# 其他程序用法： . \"%s\" ; vvm_start &   （vvm_start 会 exec，放进后台自己加 &）\n", safe);
    }
    fprintf(f, "\nVM_NAME="); sh_q(f, v->name); fputc('\n', f);
    fprintf(f, "VM_ARCH="); sh_q(f, v->arch); fputc('\n', f);
    fprintf(f, "VM_MACHINE="); sh_q(f, v->machine); fputc('\n', f);
    fprintf(f, "VM_CPU="); sh_q(f, v->cpu); fputc('\n', f);
    fprintf(f, "VM_CORES=%d\n", v->cores);
    fprintf(f, "VM_THREADS=%d\n", v->threads);
    fprintf(f, "VM_MEM=%d\n", v->mem);
    fprintf(f, "VM_MEM_AUTO=%d\n", v->mem_auto);
    fprintf(f, "VM_GPU="); sh_q(f, v->gpu); fputc('\n', f);
    fprintf(f, "VM_AUDIO="); sh_q(f, v->audio); fputc('\n', f);
    fprintf(f, "VM_IF_TYPE="); sh_q(f, v->if_type); fputc('\n', f);
    fprintf(f, "VM_NVIRT=%d\n", v->nvirt);
    fprintf(f, "VM_FIRMWARE="); sh_q(f, v->firmware); fputc('\n', f);
    fprintf(f, "VM_BIOS_FILE="); sh_q(f, v->bios_file); fputc('\n', f);
    fprintf(f, "VM_CODE_FILE="); sh_q(f, v->code_file); fputc('\n', f);
    fprintf(f, "VM_VARS_FILE="); sh_q(f, v->vars_file); fputc('\n', f);
    fprintf(f, "VM_HDA="); sh_q(f, v->hda); fputc('\n', f);
    fprintf(f, "VM_HDA2="); sh_q(f, v->hda2); fputc('\n', f);
    fprintf(f, "VM_CDROM="); sh_q(f, v->cdrom); fputc('\n', f);
    fprintf(f, "VM_CDROM2="); sh_q(f, v->cdrom2); fputc('\n', f);
    fprintf(f, "VM_FDA="); sh_q(f, v->fda); fputc('\n', f);
    fprintf(f, "VM_FDB="); sh_q(f, v->fdb); fputc('\n', f);
    fprintf(f, "VM_SHARED=%d\n", v->shared);
    fprintf(f, "VM_SHARED_PATH="); sh_q(f, v->shared_path); fputc('\n', f);
    fprintf(f, "VM_NIC="); sh_q(f, v->nic); fputc('\n', f);
    fprintf(f, "VM_NET_USER=%d\n", v->net_user);
    fprintf(f, "VM_SSH_PORT=%d\n", v->ssh_port);
    fprintf(f, "VM_DNS1="); sh_q(f, v->dns1); fputc('\n', f);
    fprintf(f, "VM_DNS2="); sh_q(f, v->dns2); fputc('\n', f);
    fprintf(f, "VM_BOOT="); sh_q(f, v->boot); fputc('\n', f);
    fprintf(f, "VM_BOOT_MENU=%d\n", v->boot_menu);
    fprintf(f, "VM_DISPLAY="); sh_q(f, v->display); fputc('\n', f);
    fprintf(f, "VM_VNC_EXT=%d\n", v->vnc_ext);
    fprintf(f, "VM_VNC_ADDR="); sh_q(f, v->vnc_addr); fputc('\n', f);
    fprintf(f, "VM_VNC_PORT=%d\n", v->vnc_port);
    fprintf(f, "VM_VNC_PASS="); sh_q(f, v->vnc_pass); fputc('\n', f);
    fprintf(f, "VM_SPICE_PORT=%d\n", v->spice_port);
    fprintf(f, "VM_LOCALTIME=%d\n", v->localtime);
    fprintf(f, "VM_OVERCOMMIT=%d\n", v->overcommit);
    fprintf(f, "VM_ARGS="); sh_q(f, v->args); fputc('\n', f);

    fprintf(f, "\n# 完整启动命令（一条便于查看/复制的字符串；真启动请用下面的 vvm_start，\n# 它是逐参数的，路径带空格也不会被拆开）\nqemu_cmd=");
    {
        static char joined[MAXARGV * PATHBUF];
        size_t off = 0;

        joined[0] = '\0';
        for (i = 0; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (i) {
                if (off + 1 >= sizeof joined) break;
                joined[off++] = ' ';
            }
            if (off + l >= sizeof joined) { joined[off] = '\0'; break; }
            memcpy(joined + off, argv[i], l);
            off += l;
            joined[off] = '\0';
        }
        sh_q(f, joined);
    }
    fputc('\n', f);

    fprintf(f, "\n# 启动函数：vvm_start 额外参数会追加到命令末尾\nvvm_start() {\n  exec");
    for (i = 0; i < argc; i++) {
        fprintf(f, " \\\n    ");
        sh_q(f, argv[i]);
    }
    fprintf(f, " \"$@\"\n}\n\n");
    fprintf(f, "# 直接执行才启动；被 source 时只加载变量\n");
    fprintf(f, "case \"$0\" in *%s.sh) vvm_start \"$@\" ;; esac\n", v->name);

    fclose(f);
    chmod(path, 0755);
    return 0;
}

static void remember_rundir(const char *name, const char *disk) {
    char d[PATHBUF];
    int i;
    run_dir_of_disk(disk, name, d, sizeof d);
    for (i = 0; i < g_rdir_n; i++) {
        if (strcmp(g_rdir[i].name, name) == 0) { snprintf(g_rdir[i].dir, PATHBUF, "%s", d); return; }
    }
    if (g_rdir_n < 64) {
        snprintf(g_rdir[g_rdir_n].name, sizeof g_rdir[0].name, "%s", name);
        snprintf(g_rdir[g_rdir_n].dir, PATHBUF, "%s", d);
        g_rdir_n++;
    }
}

/* 配置文件名和里面的 VM_NAME 不一致时（手改名/手拷文件），按 VM_NAME 找回真正的文件。
   返回 1 表示找到了，并把文件名（不含 .sh）写回 name_norm。 */
static int conf_find_by_vmname(const char *want, char *path_out, int pn,
                               char *name_norm, int nn) {
    DIR *d = opendir(CONF_DIR);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char full[PATHBUF], line[256], vn[128];
        FILE *f;
        size_t l = strlen(e->d_name);
        if (l < 4 || strcmp(e->d_name + l - 3, ".sh") != 0) continue;
        snprintf(full, sizeof full, "%s/%s", CONF_DIR, e->d_name);
        f = fopen(full, "r");
        if (!f) continue;
        vn[0] = '\0';
        while (fgets(line, sizeof line, f)) {
            char *p2 = line;
            while (*p2 == ' ' || *p2 == '#') p2++;
            if (!strncmp(p2, "VM_NAME=", 8)) {
                char *v = p2 + 8, *end;
                size_t n;
                while (*v == '\'' || *v == '"') v++;
                end = v + strlen(v);
                while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == '\'' || end[-1] == '"')) end--;
                *end = '\0';
                n = strlen(v);
                if (n >= sizeof vn) n = sizeof vn - 1;
                memcpy(vn, v, n); vn[n] = '\0';
                break;
            }
        }
        fclose(f);
        if (vn[0] && strcmp(vn, want) == 0) {
            snprintf(path_out, pn, "%s", full);
            if (name_norm && nn > 0) {
                snprintf(name_norm, nn, "%.*s", (int)(l - 3), e->d_name);
            }
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int vm_load(const char *name) {
    char path[PATHBUF], line[PATHBUF * 2];
    FILE *f;
    vm_defaults(&vm);
    snprintf(vm.name, sizeof vm.name, "%s", name);
    conf_path(name, path, sizeof path);
    f = fopen(path, "r");
    if (!f) {
        conf_path_legacy(name, path, sizeof path);
        f = fopen(path, "r");
    }
    if (!f) {
        char fn[64];
        if (conf_find_by_vmname(name, path, sizeof path, fn, sizeof fn) &&
            (f = fopen(path, "r")) != NULL) {
            /* 文件名才是这台机器的“键”：pid/日志/删除都按它来，显示名改成文件名 */
            snprintf(vm.name, sizeof vm.name, "%s", fn);
        }
    }
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val, *nl;
        char *p = line;
        int legacy = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            legacy = 1;
        }
        if (!strchr(p, '=')) continue;
        memmove(line, p, strlen(p) + 1);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        trim(key);
        trim(val);
        if (strncmp(key, "VM_", 3) == 0) key += 3;
        for (char *p = key; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        /* 注释行里的键必须长得像键（纯 ASCII 标识符），
           否则本文件自己那些中文注释一旦带个 '=' 就会改配置 */
        if (legacy && !key_plain(key)) continue;
        /* 老脚本键名 */
        {
            const char *alias = NULL;
            if (!strcmp(key, "target_arch")) alias = "arch";
            else if (!strcmp(key, "smp")) alias = "cores";
            else if (!strcmp(key, "sound_dev")) alias = "audio";
            else if (!strcmp(key, "vnc_passwd")) alias = "vnc_pass";
            else if (!strcmp(key, "vnc_listen_addr")) alias = "vnc_addr";
            else if (!strcmp(key, "vnc_display")) alias = "vnc_disp";
            else if (!strcmp(key, "dns_main")) alias = "dns1";
            else if (!strcmp(key, "dns_alt")) alias = "dns2";
            else if (!strcmp(key, "ssh_forward_port")) alias = "ssh_port";
            else if (!strcmp(key, "custom_args")) alias = "args";
            if (alias) key = (char *)alias;
        }
        sh_unquote(val);
        if (!strcmp(key, "vnc_disp")) {
            vm.vnc_ext = 1;
            vm.vnc_port = 5900 + atoi(val);
        }
        else if (!strcmp(key, "uefi_code")) {
            copyfield(vm.code_file, sizeof vm.code_file, val);
        }
        else if (!strcmp(key, "uefi_vars")) {
            copyfield(vm.vars_file, sizeof vm.vars_file, val);
        }
        else if (!strcmp(key, "x86_ovmf_code")) {
            copyfield(vm.code_file, sizeof vm.code_file, val);
            if (val[0]) copyfield(vm.firmware, sizeof vm.firmware, "uefi");
        }
        else if (!strcmp(key, "x86_ovmf_vars")) copyfield(vm.vars_file, sizeof vm.vars_file, val);
        else if (!strcmp(key, "extra_disks")) {
            char *sp = strchr(val, ' ');
            if (sp) *sp = '\0';
            while (*val == '(' || *val == '\'' || *val == '"') val++;
            { size_t l = strlen(val); while (l && (val[l-1] == ')' || val[l-1] == '\'' || val[l-1] == '"')) val[--l] = 0; }
            if (val[0]) copyfield(vm.hda2, sizeof vm.hda2, val);
        }
        else if (!strcmp(key, "name")) copyfield(vm.name, sizeof vm.name, val);
        else if (!strcmp(key, "arch")) copyfield(vm.arch, sizeof vm.arch, val);
        else if (!strcmp(key, "machine")) copyfield(vm.machine, sizeof vm.machine, val);
        else if (!strcmp(key, "cpu")) copyfield(vm.cpu, sizeof vm.cpu, val);
        else if (!strcmp(key, "cores")) vm.cores = atoi(val);
        else if (!strcmp(key, "threads")) vm.threads = atoi(val);
        else if (!strcmp(key, "mem")) vm.mem = atoi(val);
        else if (!strcmp(key, "mem_auto")) vm.mem_auto = atoi(val);
        else if (!strcmp(key, "gpu")) copyfield(vm.gpu, sizeof vm.gpu, val);
        else if (!strcmp(key, "audio")) copyfield(vm.audio, sizeof vm.audio, val);
        else if (!strcmp(key, "if_type")) copyfield(vm.if_type, sizeof vm.if_type, val);
        else if (!strcmp(key, "nvirt")) vm.nvirt = atoi(val);
        else if (!strcmp(key, "firmware")) copyfield(vm.firmware, sizeof vm.firmware, val);
        else if (!strcmp(key, "bios_file")) copyfield(vm.bios_file, sizeof vm.bios_file, val);
        else if (!strcmp(key, "code_file")) copyfield(vm.code_file, sizeof vm.code_file, val);
        else if (!strcmp(key, "vars_file")) copyfield(vm.vars_file, sizeof vm.vars_file, val);
        else if (!strcmp(key, "hda")) copyfield(vm.hda, sizeof vm.hda, val);
        else if (!strcmp(key, "hda2")) copyfield(vm.hda2, sizeof vm.hda2, val);
        else if (!strcmp(key, "cdrom")) copyfield(vm.cdrom, sizeof vm.cdrom, val);
        else if (!strcmp(key, "cdrom2")) copyfield(vm.cdrom2, sizeof vm.cdrom2, val);
        else if (!strcmp(key, "fda")) copyfield(vm.fda, sizeof vm.fda, val);
        else if (!strcmp(key, "fdb")) copyfield(vm.fdb, sizeof vm.fdb, val);
        else if (!strcmp(key, "shared")) vm.shared = atoi(val);
        else if (!strcmp(key, "shared_path")) copyfield(vm.shared_path, sizeof vm.shared_path, val);
        else if (!strcmp(key, "nic")) copyfield(vm.nic, sizeof vm.nic, val);
        else if (!strcmp(key, "net_user")) vm.net_user = atoi(val);
        else if (!strcmp(key, "ssh_port")) vm.ssh_port = atoi(val);
        else if (!strcmp(key, "dns1")) copyfield(vm.dns1, sizeof vm.dns1, val);
        else if (!strcmp(key, "dns2")) copyfield(vm.dns2, sizeof vm.dns2, val);
        else if (!strcmp(key, "boot")) copyfield(vm.boot, sizeof vm.boot, val);
        else if (!strcmp(key, "boot_menu")) vm.boot_menu = atoi(val);
        else if (!strcmp(key, "display")) copyfield(vm.display, sizeof vm.display, val);
        else if (!strcmp(key, "vnc_ext")) vm.vnc_ext = atoi(val);
        else if (!strcmp(key, "vnc_addr")) copyfield(vm.vnc_addr, sizeof vm.vnc_addr, val);
        else if (!strcmp(key, "vnc_port")) vm.vnc_port = atoi(val);
        else if (!strcmp(key, "vnc_pass")) copyfield(vm.vnc_pass, sizeof vm.vnc_pass, val);
        else if (!strcmp(key, "spice_port")) vm.spice_port = atoi(val);
        else if (!strcmp(key, "localtime")) vm.localtime = atoi(val);
        else if (!strcmp(key, "overcommit")) vm.overcommit = atoi(val);
        else if (!strcmp(key, "args")) copyfield(vm.args, sizeof vm.args, val);
    }
    fclose(f);
    if (vm.cores < 1) vm.cores = 1;
    if (vm.threads < 1) vm.threads = 1;
    if (vm.mem < 64) vm.mem = 64;
    {   /* 统一以文件名（去掉 .sh）为键：显示名、pid、日志、删除都靠它，避免两边对不上 */
        char base[64];
        const char *sl = strrchr(path, '/');
        size_t n;
        snprintf(base, sizeof base, "%s", sl ? sl + 1 : path);
        n = strlen(base);
        if (n > 3 && !strcmp(base + n - 3, ".sh")) base[n - 3] = '\0';
        if (base[0] && strcmp(base, vm.name) != 0) snprintf(vm.name, sizeof vm.name, "%s", base);
    }
    remember_rundir(vm.name, vm.hda);
    return 0;
}

static int list_configs(char names[][64], int maxn) {
    DIR *d;
    struct dirent *e;
    int n = 0;
    d = opendir(CONF_DIR);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL && n < maxn) {
        size_t l = strlen(e->d_name);
        char base[64];
        int dup = 0, i;
        if (l < 4) continue;
        if (strcmp(e->d_name + l - 3, ".sh") && strcmp(e->d_name + l - 3, ".vm")) continue;
        snprintf(base, sizeof base, "%.*s", (int)(l - 3), e->d_name);
        for (i = 0; i < n; i++) if (!strcmp(names[i], base)) { dup = 1; break; }
        if (dup) continue;
        snprintf(names[n], 64, "%s", base);
        n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------
 * 路径 / 依赖
 * ------------------------------------------------------------------ */
static const char *qemu_bin(const VM *v) {
    if (!strcmp(v->arch, "aarch64")) return "qemu-system-aarch64";
    if (!strcmp(v->arch, "i386")) return "qemu-system-i386";
    if (!strcmp(v->arch, "ppc")) return "qemu-system-ppc";
    return "qemu-system-x86_64";
}

/* 固件路径：先找 ~/start/firmware/（新家），再兼容老的 ~/.vvm/firmware/，最后找系统里现成的 */
static void fw_path(char *out, int n, const char *name) {
    static const char *alt[][2] = {
        { "QEMU_EFI.img",    "share/qemu/edk2-aarch64-code.fd" },
        { "QEMU_EFI.img",    "share/AAVMF/AAVMF_CODE.fd" },      /* 完整 64MB 的那份，优先 */
        { "QEMU_EFI.img",    "share/qemu-efi-aarch64/QEMU_EFI.fd" },
        { "QEMU_VARS.img",   "share/qemu-efi-aarch64/QEMU_VARS.fd" },
        { "QEMU_VARS.img",   "share/AAVMF/AAVMF_VARS.fd" },
        { "RELEASEX64_OVMF.fd",      "share/qemu/edk2-x86_64-code.fd" },
        { "RELEASEX64_OVMF.fd",      "share/OVMF/OVMF_CODE.fd" },
        { "RELEASEX64_OVMF_VARS.fd", "share/qemu/edk2-x86_64-vars.fd" },
        { "RELEASEX64_OVMF_VARS.fd", "share/OVMF/OVMF_VARS.fd" },
    };
    char p[PATHBUF];
    int i;

    snprintf(out, n, "%s/firmware/%s", CONF_DIR, name);
    if (file_exists(out)) return;
    snprintf(out, n, "%s/firmware/%s", ROOT_DIR, name);
    if (file_exists(out)) return;

    for (i = 0; i < (int)(sizeof alt / sizeof alt[0]); i++) {
        if (strcmp(alt[i][0], name)) continue;
        snprintf(p, sizeof p, "%s/%s", PREFIX_DIR, alt[i][1]);
        if (file_exists(p)) { snprintf(out, n, "%s", p); return; }
        snprintf(p, sizeof p, "/usr/%s", alt[i][1]);
        if (file_exists(p)) { snprintf(out, n, "%s", p); return; }
    }
}

/* 每台 VM 自己的 UEFI 变量盘（vars.fd）：
   原版脚本就是每台盘旁边放一个 64MB 的 raw，而不是所有人共用一个。
   共用会出两个问题：两台 ARM 同时跑会抢同一把 pflash 写锁；一台改了启动项
   另一台跟着变。文件名跟着磁盘走，和运行时目录放一起。 */
static void vars_fd_path(const VM *v, char *out, int n) {
    char d[PATHBUF];
    runtime_dir_for(v->name, d, sizeof d);
    snprintf(out, n, "%s/%s.vars.fd", d, v->name);
}

/* 变量盘大小必须跟 CODE 匹配：ARM 的 edk2 是 64MB，x86 的 OVMF 只有 2–4MB。
   以前一律 ftruncate 64MB，x86 pflash 两块加起来超过 8MB 上限，
   qemu 会直接报 "combined size of system firmware exceeds 8388608 bytes"。 */
static int ensure_vars_fd(const char *path, long want, char *err, int errn) {
    struct stat st;
    char d[PATHBUF];
    int fd;
    snprintf(d, sizeof d, "%s", path);
    { char *sl = strrchr(d, '/'); if (sl) *sl = '\0'; else snprintf(d, sizeof d, "."); }
    ensure_dir(d);
    if (want < 65536) want = 65536;
    if (stat(path, &st) == 0 && st.st_size == want) return 1;
    fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) { snprintf(err, errn, "无法创建 UEFI 变量盘：%s", path); return 0; }
    if (ftruncate(fd, want) != 0) {
        close(fd);
        snprintf(err, errn, "UEFI 变量盘大小调整失败：%s", path);
        return 0;
    }
    close(fd);
    return 1;
}

static void suspend_path_for(const char *name, char *out, int n) {
    char d[PATHBUF];
    runtime_dir_for(name, d, sizeof d);
    snprintf(out, n, "%s/%s.suspend", d, name);
}

static void paused_path_for(const char *name, char *out, int n) {
    char d[PATHBUF];
    runtime_dir_for(name, d, sizeof d);
    snprintf(out, n, "%s/%s.paused", d, name);
}

/* 目录能不能真的放 unix socket：路径够短 + bind 成功。
   /sdcard 是 FUSE 挂载，AF_UNIX 在那里 bind 会直接失败，所以必须探一下再决定 */
#define SOCKPATH_MAX 104        /* sockaddr_un.sun_path 是 108，留一点余量 */
static int unix_sock_ok(const char *dir) {
    char p[PATHBUF];
    struct sockaddr_un sa;
    int fd, ok;

    if (!dir || !dir[0]) return 0;
    snprintf(p, sizeof p, "%s/.vvm-probe-%d.sock", dir, (int)getpid());
    if (strlen(p) >= SOCKPATH_MAX) return 0;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", p);
    unlink(p);
    ok = (bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    close(fd);
    if (ok) unlink(p);
    return ok;
}

/* socket 放哪：优先主磁盘同目录（跟磁盘一起搬走），
   放不进去（/sdcard 不支持 AF_UNIX、或路径太长）才退回 TMPDIR / /tmp */
/* socket 文件名里不能有逗号（-qmp/-chardev 的路径是按逗号解析的） */
static void sock_name(const char *name, char *out, int n) {
    int i;
    snprintf(out, n, "%s", name ? name : "");
    for (i = 0; out[i]; i++)
        if (out[i] == ',') out[i] = '_';
}

static void sock_dir_for(const char *name, const char *prefix, char *out, int n) {
    char d[PATHBUF], p[PATHBUF], sn[PATHBUF];
    const char *tmp;
    sock_name(name, sn, sizeof sn);

    runtime_dir_for(name, d, sizeof d);
    snprintf(p, sizeof p, "%s/%s%s.sock", d, prefix, sn);
    if (d[0] && strlen(p) < SOCKPATH_MAX && unix_sock_ok(d) && !strchr(p, ',')) {
        snprintf(out, n, "%s", d);
        return;
    }

    tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(p, sizeof p, "%s/%s%s.sock", tmp, prefix, sn);
    if (strlen(p) >= SOCKPATH_MAX || !unix_sock_ok(tmp)) {
        if (unix_sock_ok("/tmp")) tmp = "/tmp";
        else if (unix_sock_ok(ROOT_DIR)) tmp = ROOT_DIR;
        else if (unix_sock_ok(HOME_DIR)) tmp = HOME_DIR;
    }
    snprintf(out, n, "%s", tmp);
}

static void qmp_path_for(const char *name, char *out, int n) {
    char d[PATHBUF], sn[PATHBUF];
    sock_dir_for(name, "qmp-", d, sizeof d);
    sock_name(name, sn, sizeof sn);
    snprintf(out, n, "%s/qmp-%s.sock", d, sn);
}

static void vnc_path_for(const char *name, char *out, int n) {
    char d[PATHBUF], sn[PATHBUF];
    sock_dir_for(name, "vnc-", d, sizeof d);
    sock_name(name, sn, sizeof sn);
    snprintf(out, n, "%s/vnc-%s.sock", d, sn);
}

static void pid_path_for(const char *name, char *out, int n) {
    char d[PATHBUF];
    runtime_dir_for(name, d, sizeof d);
    snprintf(out, n, "%s/%s.pid", d, name);
}

static void log_path_for(const VM *v, char *out, int n) {
    char d[PATHBUF];
    runtime_dir_for(v->name, d, sizeof d);
    snprintf(out, n, "%s/%s.log", d, v->name);
}

static int read_pid(const char *name) {
    char path[PATHBUF], buf[64];
    FILE *f;
    int pid = 0;
    pid_path_for(name, path, sizeof path);
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(buf, sizeof buf, f)) pid = atoi(buf);
    fclose(f);
    return pid;
}

static int pid_alive(int pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

/* pid 文件里的号码可能是：僵尸、或者重启后又被系统分给了别的进程。
   光靠 kill(pid,0) 会说谎，得看看 /proc 里这个进程到底是啥。 */
static int qemu_alive(int pid) {
    char p[PATHBUF], buf[1024];
    FILE *f;
    size_t n, i;
    if (!pid_alive(pid)) return 0;
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    f = fopen(p, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) {
            char *rp = strrchr(buf, ')');
            if (rp && rp[1] == ' ' && rp[2] == 'Z') { fclose(f); return 0; }   /* 僵尸：其实已经死了 */
        }
        fclose(f);
    }
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    f = fopen(p, "r");
    if (!f) return 1;                             /* 读不到 /proc 就信 kill() */
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    for (i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
    return strstr(buf, "qemu") != NULL;           /* 不是 qemu 就是 pid 被复用了 */
}

/* 连一下 socket 就知道那边有没有人在听（pid 丢了也能认出来） */
static int sock_connectable(const char *path) {
    struct sockaddr_un a;
    int fd, ok;
    if (!path || !path[0] || access(path, F_OK) != 0) return 0;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static int vm_state_of(const char *name, int pid) {
    char p[PATHBUF];
    int live = qemu_alive(pid);
    if (!live) {                                  /* pid 文件没了/过期：用 QMP socket 兜底 */
        qmp_path_for(name, p, sizeof p);
        live = sock_connectable(p);
    }
    if (live) {
        paused_path_for(name, p, sizeof p);
        return access(p, F_OK) == 0 ? 2 : 3;      /* 2 已暂停  3 运行中 */
    }
    suspend_path_for(name, p, sizeof p);
    if (access(p, F_OK) == 0) return 1;           /* 1 已挂起 */
    return 0;                                     /* 0 未启动 */
}

/* 界面各处判断"这台是不是在跑"统一走这里 */
static int vm_running(const char *name) {
    return vm_state_of(name, read_pid(name)) >= 2;
}

/* 端口是否已被占用（bind 一下就知道，不用连） */
static int port_busy(int port) {
    int fd, r;
    struct sockaddr_in sa;
    if (port <= 0 || port > 65535) return 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
    return r != 0;
}

/* 有没有别的“正在跑”的虚拟机占着这个 SSH 端口 */
static int ssh_port_taken_by_other(int port, const char *self) {
    char names[64][64];
    int n = list_configs(names, 64), i, taken = 0;
    VM bak = vm;
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], self) == 0) continue;
        vm_load(names[i]);
        if (pid_alive(read_pid(vm.name)) && vm.ssh_port == port) { taken = 1; break; }
    }
    vm = bak;
    return taken;
}

/* 有没有别的“正在跑”的虚拟机占着这个 VNC 端口 */
static int vnc_port_taken_by_other(int port, const char *self) {
    char names[64][64];
    int n = list_configs(names, 64), i, taken = 0;
    VM bak = vm;
    for (i = 0; i < n; i++) {
        if (strcmp(names[i], self) == 0) continue;
        vm_load(names[i]);
        if (pid_alive(read_pid(vm.name)) && vm.vnc_ext &&
            strcmp(vm.display, "vnc") == 0 && vm.vnc_port == port) { taken = 1; break; }
    }
    vm = bak;
    return taken;
}

/* 往一段提示后面追加一句，别拿同一个缓冲区既当源又当目标（那是未定义行为） */
static void note_append(char *dst, int n, const char *msg) {
    char tmp[192];
    if (!dst || n <= 0) return;
    if (dst[0]) snprintf(tmp, sizeof tmp, "%s；%s", dst, msg);
    else snprintf(tmp, sizeof tmp, "%s", msg);
    snprintf(dst, n, "%s", tmp);
}

/* 换端口时的说明，启动完拿给状态条用 */
static char g_port_note[128];
/* 核数被本机上限压下来时的说明，和端口说明一起放进启动消息 */
static char g_cpu_note[128];

static void note_join(char *out, int n) {
    out[0] = '\0';
    if (g_port_note[0] && g_cpu_note[0]) snprintf(out, n, "%s；%s", g_port_note, g_cpu_note);
    else if (g_port_note[0]) snprintf(out, n, "%s", g_port_note);
    else if (g_cpu_note[0]) snprintf(out, n, "%s", g_cpu_note);
}

/* 端口被别的虚拟机占了就换一个，别让 qemu 因为 hostfwd 直接退出 */
/* 端口被占就重新从 2222（SSH）/5901（VNC）往上扫，不沿用上次换到的号，
   否则每启动一次数字就往后爬一段，越跑越大。
   扫的时候要跳过本机占用，也要跳过别的在跑的虚拟机已经占着的号。 */
static int scan_port(int base, int span, int self_old, int is_vnc, const char *self) {
    int p, n = 0, cand[300];
    for (p = base; p < 65535 && n < 300 && p < base + span; p++) {
        if (p == self_old) continue;
        if (port_busy(p)) continue;
        cand[n++] = p;
    }
    for (p = 0; p < n; p++)
        if (!(is_vnc ? vnc_port_taken_by_other(cand[p], self) : ssh_port_taken_by_other(cand[p], self)))
            return cand[p];
    return 0;
}

static void vm_fix_ports(VM *v) {
    int old;
    g_port_note[0] = '\0';
    if (v->ssh_port > 0 && (port_busy(v->ssh_port) || ssh_port_taken_by_other(v->ssh_port, v->name))) {
        int np;
        old = v->ssh_port;
        np = scan_port(2222, 300, old, 0, v->name);
        if (np > 0) {
            v->ssh_port = np;
            snprintf(g_port_note, sizeof g_port_note, "SSH 端口 %d 被占用，改用 %d", old, v->ssh_port);
        } else {
            snprintf(g_port_note, sizeof g_port_note, "SSH 端口 %d 被占用，2222 往后 300 个都满了", old);
        }
    }
    if (!strcmp(v->display, "vnc") && v->vnc_ext &&
        (port_busy(v->vnc_port) || vnc_port_taken_by_other(v->vnc_port, v->name))) {
        char note[96];
        int np;
        old = v->vnc_port;
        np = scan_port(5901, 99, old, 1, v->name);
        if (np > 0) {
            v->vnc_port = np;
            snprintf(note, sizeof note, "%sVNC 端口 %d 被占用，改用 %d",
                     g_port_note[0] ? "；" : "", old, v->vnc_port);
        } else {
            snprintf(note, sizeof note, "%sVNC 端口 %d 被占用，5900-5999 里没有空位",
                     g_port_note[0] ? "；" : "", old);
        }
        strncat(g_port_note, note, sizeof g_port_note - strlen(g_port_note) - 1);
    }
    if (g_port_note[0]) vm_save(v);
}

static int host_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static int host_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    int mb = 0;
    if (!f) return 2048;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "MemAvailable:", 13)) {
            long kb = atol(line + 13);
            mb = (int)(kb / 1024);
            break;
        }
        if (!strncmp(line, "MemTotal:", 9) && !mb) mb = (int)(atol(line + 9) / 1024);
    }
    fclose(f);
    return mb > 0 ? mb : 2048;
}

/* ------------------------------------------------------------------
 * 命令拼装（顺序照 StartVM.env）
 * ------------------------------------------------------------------ */
/* qemu 的 -drive/-chardev 选项用逗号分隔，路径里的逗号要写成两个逗号 */
/* 把附加参数按引号切词：支持 "..."、'...' 和反斜杠转义 */
static int split_args(char *s, char *out[], int max) {
    int n = 0;
    char *p = s;
    while (*p && n < max) {
        char *w;
        int q = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        w = p;
        out[n++] = w;
        while (*p) {
            if (*p == '\\' && p[1]) { *w++ = p[1]; p += 2; continue; }
            if (!q && (*p == '"' || *p == '\'')) { q = *p; p++; continue; }
            if (q && *p == q) { q = 0; p++; continue; }
            if (!q && (*p == ' ' || *p == '\t')) { p++; break; }
            *w++ = *p++;
        }
        *w = '\0';
    }
    return n;
}

static const char *qesc(const char *in) {
    static char buf[8][PATHBUF * 2];
    static int k = 0;
    char *o = buf[k = (k + 1) % 8];
    int j = 0;
    if (!in) return "";
    for (; *in && j < (int)(sizeof buf[0]) - 2; in++) {
        if (*in == ',') o[j++] = ',';
        o[j++] = *in;
    }
    o[j] = '\0';
    return o;
}

static int vm_build_argv(const VM *v, char **argv, int maxargv,
                         char pool[][PATHBUF], int pooln, int background,
                         const char *serial_dev) {
    int argc = 0, pi = 0;
    char sock[PATHBUF], bufsock[PATHBUF];
    char extra[512], *tok;
    int mem = v->mem, i;

    g_cpu_note[0] = '\0';

#define ADD(...) do { \
        if (pi < pooln - 1 && argc < maxargv - 3) { \
            snprintf(pool[pi], PATHBUF, __VA_ARGS__); \
            argv[argc++] = pool[pi++]; \
        } \
    } while (0)

    ADD("%s", qemu_bin(v));
    {
        /* 宿主机有 /dev/kvm 且架构一致才用 kvm；安卓上一般没有，就还是 tcg 多线程 */
        char harch[64] = "";
        FILE *uf = fopen("/proc/sys/kernel/osrelease", "r");
        int kvm_ok = (access("/dev/kvm", R_OK | W_OK) == 0);
        if (uf) fclose(uf);
        if (kvm_ok) {
            struct utsname un;
            if (uname(&un) == 0) {
                if (strstr(un.machine, "aarch64") || strstr(un.machine, "arm64")) snprintf(harch, sizeof harch, "aarch64");
                else if (strstr(un.machine, "x86_64")) snprintf(harch, sizeof harch, "x86_64");
            }
        }
        if (kvm_ok && harch[0] && !strcmp(v->arch, harch)) { ADD("-accel"); ADD("kvm"); }
        else { ADD("-accel"); ADD("tcg,thread=multi"); }
    }
    qmp_path_for(v->name, sock, sizeof sock);
    ADD("-qmp");
    ADD("unix:%s,server,nowait", qesc(sock));

    if (v->machine[0]) {
        char m[96];
        snprintf(m, sizeof m, "%s", v->machine);
        if (v->nvirt && !strcmp(v->arch, "aarch64")) strncat(m, ",virtualization=true", sizeof m - strlen(m) - 1);
        ADD("-M");
        ADD("%s", m);
    }
    if (v->cpu[0]) {
        char c[96];
        snprintf(c, sizeof c, "%s", v->cpu);
        if (v->nvirt && (!strcmp(v->arch, "x86_64") || !strcmp(v->arch, "i386")))
            strncat(c, ",+vmx,+svm", sizeof c - strlen(c) - 1);
        ADD("-cpu");
        ADD("%s", c);
    }
    {
        int cores = v->cores > 0 ? v->cores : 1;
        int threads = v->threads > 0 ? v->threads : 1;
        if (cores * threads > host_cores()) {
            int hc = host_cores();
            if (hc < 1) hc = 1;
            snprintf(g_cpu_note, sizeof g_cpu_note, "本机只有 %d 个核，已按 %d 核 1 线程启动", hc, hc);
            cores = hc;
            threads = 1;
        }
        ADD("-smp");
        ADD("sockets=1,cores=%d,threads=%d", cores, threads);
    }
    if (v->boot_menu) {
        ADD("-boot");
        if (v->boot[0]) ADD("%s,menu=on", v->boot);
        else ADD("menu=on");
    } else if (v->boot[0]) { ADD("-boot"); ADD("%s", v->boot); }

    if (v->nic[0]) ADD("-device");
    if (v->nic[0]) ADD("%s,netdev=net0", v->nic);
    if (v->net_user) {
        char net[320];
        snprintf(net, sizeof net, "user,id=net0");
        if (v->ssh_port > 0) {
            char tmp[128];
            snprintf(tmp, sizeof tmp, ",hostfwd=tcp::%d-:22", v->ssh_port);
            strncat(net, tmp, sizeof net - strlen(net) - 1);
        }
        if (v->dns1[0]) {
            char tmp[80];
            snprintf(tmp, sizeof tmp, ",dns=%s", v->dns1);
            strncat(net, tmp, sizeof net - strlen(net) - 1);
        }
        if (v->dns2[0]) {
            char tmp[80];
            snprintf(tmp, sizeof tmp, ",dns=%s", v->dns2);
            strncat(net, tmp, sizeof net - strlen(net) - 1);
        }
        ADD("-netdev");
        ADD("%s", net);
    }

    if (v->gpu[0]) {
        if (!strcmp(v->gpu, "std") || !strcmp(v->gpu, "qxl") || !strcmp(v->gpu, "virtio") ||
            !strcmp(v->gpu, "vmware") || !strcmp(v->gpu, "cirrus") || !strcmp(v->gpu, "none")) {
            ADD("-vga");
            ADD("%s", v->gpu);
        } else {
            ADD("-device");
            ADD("%s", v->gpu);
        }
    }

    if (v->args[0]) {
        char *toks[64];
        int nt;
        snprintf(extra, sizeof extra, "%s", v->args);
        nt = split_args(extra, toks, 64);
        for (tok = 0, i = 0; i < nt; i++) {
            if (pi >= pooln - 3 || argc >= maxargv - 3) {   /* 参数被截断了，让用户知道 */
                char m[96];
                snprintf(m, sizeof m, "附加参数太多，只用了前 %d 个", i);
                note_append(g_cpu_note, sizeof g_cpu_note, m);
                break;
            }
            ADD("%s", toks[i]);
        }
        (void)tok;
    }

    if (v->hda[0]) {
        if (!v->if_type[0]) { ADD("-hda"); ADD("%s", v->hda); }
        else if ((!strcmp(v->arch, "aarch64") && !strcmp(v->if_type, "ide")) || !strcmp(v->arch, "ppc")) {
            ADD("-drive"); ADD("media=disk,file=%s", qesc(v->hda));
        } else {
            ADD("-drive"); ADD("media=disk,if=%s,file=%s", v->if_type, qesc(v->hda));
        }
    }
    if (v->hda2[0]) {
        if (!v->if_type[0]) { ADD("-hdb"); ADD("%s", v->hda2); }
        else if ((!strcmp(v->arch, "aarch64") && !strcmp(v->if_type, "ide")) || !strcmp(v->arch, "ppc")) {
            ADD("-drive"); ADD("media=disk,file=%s", qesc(v->hda2));
        } else {
            ADD("-drive"); ADD("media=disk,if=%s,file=%s", v->if_type, qesc(v->hda2));
        }
    }

    if (v->cdrom[0]) {
        if (!strcmp(v->arch, "aarch64")) {
            ADD("-device"); ADD("nec-usb-xhci,id=usbopticaldiscreader0");
            ADD("-device"); ADD("usb-storage,bus=usbopticaldiscreader0.0,drive=cdromdrive0");
            ADD("-drive"); ADD("if=none,id=cdromdrive0,format=raw,media=cdrom,file=%s", qesc(v->cdrom));
        } else {
            ADD("-drive"); ADD("media=cdrom,file=%s", qesc(v->cdrom));
        }
    }
    if (v->cdrom2[0]) {
        const char *bus = v->cdrom[0] ? "usbopticaldiscreader1" : "usbopticaldiscreader0";
        if (!strcmp(v->arch, "aarch64")) {
            ADD("-device"); ADD("nec-usb-xhci,id=%s", bus);
            ADD("-device"); ADD("usb-storage,bus=%s.0,drive=cdromdrive1", bus);
            ADD("-drive"); ADD("if=none,id=cdromdrive1,format=raw,media=cdrom,file=%s", qesc(v->cdrom2));
        } else {
            ADD("-drive"); ADD("media=cdrom,file=%s", qesc(v->cdrom2));
        }
    }

    if (v->fda[0] && strcmp(v->arch, "aarch64")) { ADD("-fda"); ADD("%s", v->fda); }
    if (v->fdb[0] && strcmp(v->arch, "aarch64")) { ADD("-fdb"); ADD("%s", v->fdb); }

    if (v->shared && v->shared_path[0]) {
        ADD("-drive"); ADD("file=fat:rw:%s,format=raw", qesc(v->shared_path));
    }

    /* 固件 */
    if (strcmp(v->firmware, "none")) {
        char code[PATHBUF], varsf[PATHBUF], tpl[PATHBUF];
        if (v->bios_file[0]) {
            ADD("-bios"); ADD("%s", v->bios_file);
        } else if (!strcmp(v->arch, "aarch64")) {
            fw_path(code, sizeof code, "QEMU_EFI.img");
            if (v->code_file[0]) snprintf(code, sizeof code, "%s", v->code_file);
            if (v->vars_file[0]) snprintf(varsf, sizeof varsf, "%s", v->vars_file);
            else vars_fd_path(v, varsf, sizeof varsf);
            {   char ve[256];
                if (!ensure_vars_fd(varsf, 64L * 1024 * 1024, ve, sizeof ve)) snprintf(varsf, sizeof varsf, "");
            }
            if (file_exists(code)) { ADD("-drive"); ADD("file=%s,format=raw,readonly=on,if=pflash", qesc(code)); }
            if (file_exists(varsf)) { ADD("-drive"); ADD("file=%s,format=raw,if=pflash", qesc(varsf)); }
        } else if (!strcmp(v->arch, "ppc")) {
            ADD("-L"); ADD("pc-bios");
        } else if (!strcmp(v->firmware, "uefi")) {
            fw_path(code, sizeof code, "RELEASEX64_OVMF.fd");
            if (v->code_file[0]) snprintf(code, sizeof code, "%s", v->code_file);
            if (v->vars_file[0]) snprintf(varsf, sizeof varsf, "%s", v->vars_file);
            else vars_fd_path(v, varsf, sizeof varsf);
            {   char ve[256];
                struct stat cst, vst;
                long want = 4L * 1024 * 1024;
                int got = 0;
                fw_path(tpl, sizeof tpl, "RELEASEX64_OVMF_VARS.fd");
                if (stat(code, &cst) == 0 && cst.st_size > 0) want = (long)cst.st_size;
                if (stat(varsf, &vst) != 0 && file_exists(tpl) && strcmp(tpl, varsf) != 0) {
                    /* 第一次用：拷一份系统 OVMF vars 模板，格式才是 OVMF 认的 */
                    FILE *in = fopen(tpl, "rb"), *ou = fopen(varsf, "wb");
                    if (in && ou) {
                        char buf[65536];
                        size_t rd;
                        while ((rd = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, rd, ou);
                        got = 1;
                    }
                    if (in) fclose(in);
                    if (ou) fclose(ou);
                }
                if (!got && !ensure_vars_fd(varsf, want, ve, sizeof ve)) snprintf(varsf, sizeof varsf, "");
            }
            if (file_exists(code)) { ADD("-drive"); ADD("file=%s,format=raw,readonly=on,if=pflash", qesc(code)); }
            if (file_exists(varsf)) { ADD("-drive"); ADD("file=%s,format=raw,if=pflash", qesc(varsf)); }
        } else {
            fw_path(code, sizeof code, "bios.bin");
            if (file_exists(code)) { ADD("-bios"); ADD("%s", code); }
        }
    }

    {
        char sm[PATHBUF];
        suspend_path_for(v->name, sm, sizeof sm);
        if (file_exists(sm)) {
            if (disk_has_suspend(v->hda)) { ADD("-loadvm"); ADD("suspend"); }
            else unlink(sm);        /* 标记过期，别让 qemu 带着 -loadvm 秒退 */
        }
    }

    if (v->overcommit) { ADD("-overcommit"); ADD("mem-lock=off"); }
    if (v->localtime) { ADD("-rtc"); ADD("base=localtime"); }

    if (v->mem_auto) {
        mem = host_avail_mb() - 100;
        if (mem < 256) mem = 256;
    }
    if (mem < 64) mem = 64;
    if (mem > host_avail_mb()) {           /* 配置被手改成超大值：别让 qemu 直接分配失败 */
        char m[96];
        snprintf(m, sizeof m, "内存 %d MB 超过本机可用 %d MB，已降到 %d MB",
                 mem, host_avail_mb(), host_avail_mb() - 100 > 256 ? host_avail_mb() - 100 : 256);
        mem = host_avail_mb() - 100 > 256 ? host_avail_mb() - 100 : 256;
        note_append(g_cpu_note, sizeof g_cpu_note, m);
    }
    if (!strcmp(v->arch, "ppc") && mem > 2048) mem = 2048;
    ADD("-m");
    ADD("%d", mem);

    if (!strcmp(v->display, "vnc") || !strcmp(v->display, "spice")) {
        ADD("-device"); ADD("qemu-xhci");
        ADD("-device"); ADD("usb-kbd");
        ADD("-device"); ADD("usb-tablet");
        ADD("-k"); ADD("en-us");
    }

    if (v->audio[0] && strcmp(v->audio, "none")) {
        ADD("-device"); ADD("%s,audiodev=snd0", v->audio);
        ADD("-audiodev"); ADD("none,id=snd0");
    }

    if (!strcmp(v->display, "vnc")) {
        if (v->vnc_pass[0]) {
            ADD("-object"); ADD("secret,id=vncpass,data=%s", v->vnc_pass);
        }
        if (v->vnc_ext) {
            if (v->vnc_pass[0]) {
                ADD("-vnc"); ADD("%s:%d,password-secret=vncpass", v->vnc_addr, v->vnc_port - 5900);
            } else {
                ADD("-vnc"); ADD("%s:%d", v->vnc_addr, v->vnc_port - 5900);
            }
        } else {
            vnc_path_for(v->name, bufsock, sizeof bufsock);
            ADD("-vnc"); ADD("unix:%s,lossy=off", qesc(bufsock));
        }
        ADD("-monitor"); ADD("vc");
    } else if (!strcmp(v->display, "spice")) {
        ADD("-spice"); ADD("port=%d,disable-ticketing=on", v->spice_port);
    } else if (!strcmp(v->display, "sdl")) {
        ADD("-display"); ADD("sdl");
        ADD("-monitor"); ADD("vc");
    } else if (!strcmp(v->display, "gtk")) {
        ADD("-display"); ADD("gtk,gl=on");
        ADD("-monitor"); ADD("vc");
    } else {
        ADD("-display"); ADD("none");
    }

    if (serial_dev && *serial_dev) {
        char sp[PATHBUF], lp[PATHBUF];
        serial_sock_path(v->name, sp, sizeof sp);
        log_path_for(v, lp, sizeof lp);
        ADD("-chardev");
        ADD("socket,id=vser0,path=%s,server=on,wait=off,logfile=%s,logappend=on", qesc(sp), qesc(lp));
        ADD("-serial"); ADD("chardev:vser0");
    } else if (background) {
        char logp[PATHBUF];
        log_path_for(v, logp, sizeof logp);
        ADD("-serial"); ADD("file:%s", logp);
    } else {
        ADD("-serial"); ADD("stdio");
    }

    argv[argc] = NULL;
#undef ADD
    return argc;
}


/* 只接受：IPv4（四段 0-255）、IPv6（含冒号）、普通主机名；其余拒绝 */
static int valid_addr(const char *s, char *why, int n) {
    const char *p;
    if (!s || !*s) { snprintf(why, n, "地址不能为空（本机用 127.0.0.1）"); return 0; }
    if (strlen(s) > 31) { snprintf(why, n, "地址太长"); return 0; }
    if (strchr(s, ':')) {                      /* IPv6 */
        for (p = s; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                  (*p >= 'A' && *p <= 'F') || *p == ':' || *p == '.')) {
                snprintf(why, n, "IPv6 只能有 0-9 a-f : ."); return 0;
            }
        if (strlen(s) < 3) { snprintf(why, n, "IPv6 写法不完整"); return 0; }
        return 1;
    }
    {
        int allnum = 1, dots = 0, i;
        for (p = s; *p; p++) {
            if (*p == '.') { dots++; continue; }
            if (*p < '0' || *p > '9') { allnum = 0; break; }
        }
        if (allnum) {
            int a, b, c, d;
            char tail;
            if (dots != 3) { snprintf(why, n, "IPv4 必须是四段，形如 127.0.0.1"); return 0; }
            a = b = c = d = 0; tail = 0;
            i = sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail);
            if (i != 4 || a < 0 || a > 255 || b < 0 || b > 255 ||
                c < 0 || c > 255 || d < 0 || d > 255) {
                snprintf(why, n, "每段要在 0-255 之间"); return 0;
            }
            return 1;
        }
    }
    {                                          /* 主机名 */
        int len = 0;
        for (p = s; *p; p++, len++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) {
                snprintf(why, n, "只能包含字母、数字、. - _"); return 0;
            }
        }
        if (s[0] == '.' || s[0] == '-' || s[len - 1] == '.' || s[len - 1] == '-') {
            snprintf(why, n, "主机名不能以 . 或 - 开头结尾"); return 0;
        }
    }
    return 1;
}

static int path_is_dir(const char *p) {
    struct stat st;
    if (!p || !*p) return 0;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 原版脚本对 ARM UEFI 固件做大小校验：60–70MB 才是完整的一份。
   空文件、半截下载也能“存在”，但一启动就停在 UEFI Shell 或黑屏。 */
static int fw_arm_size_ok(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_size >= (off_t)62914560 && st.st_size <= (off_t)73400320;
}

static int vm_validate(const VM *v, char *err, int errn) {
    char a[PATHBUF];
    if (!err || errn <= 0) { err = NULL; errn = 0; }   /* 只想问"合不合法"时不传缓冲区 */
    else err[0] = '\0';
    if (v->cores < 1 || v->threads < 1) { snprintf(err, errn, "核心数/线程数必须 >= 1"); return EXIT_INVALID_PARAM; }
    /* 配置被手改坏时不至于悄悄用错二进制/错参数，先挡下来 */
    if (strcmp(v->arch, "x86_64") && strcmp(v->arch, "i386") &&
        strcmp(v->arch, "aarch64") && strcmp(v->arch, "ppc")) {
        snprintf(err, errn, "不认识的架构：%s（只支持 x86_64 / i386 / aarch64 / ppc）", v->arch);
        return EXIT_INVALID_PARAM;
    }
    {
        const char *okdisp[] = { "none", "vnc", "spice" };
        int i, hit = 0;
        for (i = 0; i < 3; i++) if (!strcmp(v->display, okdisp[i])) hit = 1;
        if (!hit) { snprintf(err, errn, "显示方式不认识：%s（none / vnc / spice）", v->display); return EXIT_INVALID_PARAM; }
    }
    {
        const char *okif[] = { "virtio", "ide", "scsi", "usb" };
        int i, hit = 0;
        for (i = 0; i < 4; i++) if (!strcmp(v->if_type, okif[i])) hit = 1;
        if (!hit) { snprintf(err, errn, "磁盘接口不认识：%s（virtio / ide / scsi / usb）", v->if_type); return EXIT_INVALID_PARAM; }
    }
    {
        const char *p2;
        for (p2 = v->machine; *p2; p2++)
            if (!isalnum((unsigned char)*p2) && *p2 != '-' && *p2 != '_' && *p2 != '.') {
                snprintf(err, errn, "主板型号里有不合法字符：%s", v->machine);
                return EXIT_INVALID_PARAM;
            }
        for (p2 = v->cpu; *p2; p2++)
            if (!isalnum((unsigned char)*p2) && *p2 != '-' && *p2 != '_' && *p2 != '.') {
                snprintf(err, errn, "CPU 型号里有不合法字符：%s", v->cpu);
                return EXIT_INVALID_PARAM;
            }
    }
    if (v->mem < 64) { snprintf(err, errn, "内存至少 64 MB"); return EXIT_INVALID_PARAM; }
    {
        char why[128];
        if (strcmp(v->display, "vnc") == 0 && !valid_addr(v->vnc_addr, why, sizeof why)) {
            snprintf(err, errn, "VNC 地址不对：%s", why);
            return EXIT_INVALID_PARAM;
        }
    }
    if (!v->hda[0] && !v->hda2[0] && !v->cdrom[0] && !v->cdrom2[0]) {
        snprintf(err, errn, "没有磁盘也没有光盘，开不了机：先加一块磁盘或者挂一个 ISO");
        return EXIT_INVALID_PARAM;
    }
    if (v->hda[0] && !file_exists(v->hda)) { snprintf(err, errn, "磁盘不存在：%s", v->hda); return EXIT_FILE_MISS; }
    if (v->hda[0] && path_is_dir(v->hda)) { snprintf(err, errn, "磁盘路径是个目录：%s", v->hda); return EXIT_FILE_CORRUPT; }
    if (v->hda2[0] && !file_exists(v->hda2)) { snprintf(err, errn, "第二块磁盘不存在：%s", v->hda2); return EXIT_FILE_MISS; }
    if (v->hda2[0] && path_is_dir(v->hda2)) { snprintf(err, errn, "第二块磁盘路径是个目录：%s", v->hda2); return EXIT_FILE_CORRUPT; }
    if (v->cdrom[0] && !file_exists(v->cdrom)) { snprintf(err, errn, "ISO 不存在：%s", v->cdrom); return EXIT_FILE_MISS; }
    if (v->cdrom[0] && path_is_dir(v->cdrom)) { snprintf(err, errn, "ISO 路径是个目录：%s", v->cdrom); return EXIT_FILE_CORRUPT; }
    if (v->cdrom2[0] && !file_exists(v->cdrom2)) { snprintf(err, errn, "第二张 ISO 不存在：%s", v->cdrom2); return EXIT_FILE_MISS; }
    if (v->cdrom2[0] && path_is_dir(v->cdrom2)) { snprintf(err, errn, "第二张 ISO 路径是个目录：%s", v->cdrom2); return EXIT_FILE_CORRUPT; }
    if (!strcmp(v->arch, "x86_64") && !strcmp(v->firmware, "uefi")) {
        fw_path(a, sizeof a, "RELEASEX64_OVMF.fd");
        if (!file_exists(a) && !v->code_file[0]) {
            snprintf(err, errn, "选了 UEFI，但缺 %s\n把 OVMF 文件放到 %s/firmware，或改回默认 BIOS", "RELEASEX64_OVMF.fd", CONF_DIR);
            return EXIT_MISS_DEPEND;
        }
    }
    if (v->bios_file[0] && !file_exists(v->bios_file)) {
        snprintf(err, errn, "自定义 BIOS 不存在：%s", v->bios_file);
        return EXIT_FILE_MISS;
    }
    if (!strcmp(v->arch, "aarch64")) {
        struct stat fst;
        fw_path(a, sizeof a, "QEMU_EFI.img");
        if (!file_exists(a)) {
            snprintf(err, errn, "缺少 ARM64 固件 %s\n主页 → 固件，Enter 自动从 APK 提取", strrchr(a, '/') ? strrchr(a, '/') + 1 : a);
            return EXIT_MISS_DEPEND;
        }
        /* 原版脚本会查大小：空文件、半截下载都会被当成固件“存在”，
           启动后停在 UEFI Shell 或者干脆黑屏，这里提前报损坏 */
        if (stat(a, &fst) != 0 || !fw_arm_size_ok(a)) {
            snprintf(err, errn, "ARM64 固件大小不对[%ld MB，正常应在 60–70 MB]，重装 qemu-system-aarch64-headless 或换一份 %s",
                     (long)((stat(a, &fst) == 0 ? fst.st_size : 0) / (1024 * 1024)),
                     strrchr(a, '/') ? strrchr(a, '/') + 1 : a);
            return EXIT_FILE_CORRUPT;
        }
    }
    if (!strcmp(v->display, "vnc") && v->vnc_ext &&
        (v->vnc_port < 5900 || v->vnc_port > 5999)) {
        snprintf(err, errn, "VNC 端口应在 5900-5999"); return EXIT_INVALID_PARAM;
    }
    if (v->ssh_port && (v->ssh_port < 1024 || v->ssh_port > 65535)) {
        snprintf(err, errn, "SSH 转发端口应在 1024-65535"); return EXIT_INVALID_PARAM;
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------
 * QMP
 * ------------------------------------------------------------------ */
static int qmp_connect(const char *name, char *err, int errn) {
    int fd, n;
    struct sockaddr_un addr;
    char sock[PATHBUF], buf[4096];
    qmp_path_for(name, sock, sizeof sock);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errn, "socket 创建失败"); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        snprintf(err, errn, "连不上 QMP [ 虚拟机未启动？]");
        close(fd);
        return -1;
    }
    n = (int)read(fd, buf, sizeof buf - 1);
    if (n <= 0) { snprintf(err, errn, "QMP 无响应"); close(fd); return -1; }
    buf[n] = '\0';
    if (!strstr(buf, "\"QMP\"")) { snprintf(err, errn, "QMP 握手失败"); close(fd); return -1; }
    if (write(fd, "{\"execute\":\"qmp_capabilities\"}\n", 31) < 0) {
        snprintf(err, errn, "QMP 写入失败"); close(fd); return -1;
    }
    n = (int)read(fd, buf, sizeof buf - 1);
    if (n <= 0 || !strstr(buf, "\"return\"")) {
        snprintf(err, errn, "QMP 未进入命令模式"); close(fd); return -1;
    }
    return fd;
}

static int qmp_exec(const char *name, const char *json, char *reply, int rn,
                    char *err, int errn) {
    int fd, n, total = 0;
    fd = qmp_connect(name, err, errn);
    if (fd < 0) return -1;
    if (write(fd, json, strlen(json)) < 0 || write(fd, "\n", 1) < 0) {
        snprintf(err, errn, "QMP 写入失败");
        close(fd);
        return -1;
    }
    while (total < rn - 1) {
        n = (int)read(fd, reply + total, rn - 1 - total);
        if (n <= 0) break;
        total += n;
        reply[total] = '\0';
        if (strstr(reply, "\"return\"") || strstr(reply, "\"error\"")) break;
    }
    close(fd);
    if (total <= 0) { snprintf(err, errn, "QMP 无回应"); return -1; }
    if (strstr(reply, "\"error\"")) { snprintf(err, errn, "QMP 报错：%s", reply); return -1; }
    return 0;
}


/* ------------------------------------------------------------------
 * qemu-img / 启动
 * ------------------------------------------------------------------ */
static int run_cmd(char *const argv[], char *out, int outn) {
    int pipefd[2], status = 0, total = 0;
    pid_t pid;
    ssize_t n;
    if (out && outn > 0) out[0] = '\0';
    if (pipe(pipefd) != 0) return -1;
    pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    while (out && outn > 0 && total < outn - 1 &&
           (n = read(pipefd[0], out + total, outn - 1 - total)) > 0) {
        total += (int)n;
        out[total] = '\0';
    }
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void qemu_err_line(const char *name, char *out, int n);
static void qemu_err_brief(const char *name, char *out, int n);

/* 这个 qcow2 里到底有没有名为 suspend 的内部快照。
   光看 <name>.suspend 标记文件是不够的：快照可能早被删掉、或者上次保存失败
   只留下了标记，这时还硬加 -loadvm，qemu 会立刻退出（“启动后立刻退出了”）。 */
static int disk_has_suspend(const char *hda) {
    char out[4096];
    char *a[] = { "qemu-img", "snapshot", "-l", (char *)hda, NULL };
    if (!hda || !hda[0]) return 0;
    out[0] = '\0';
    if (run_cmd(a, out, sizeof out) != 0) return 0;
    return strstr(out, "suspend") != NULL;
}

static int vm_start_background(VM *v, char *err, int errn) {
    char *argv[MAXARGV];
    char pool[MAXARGV][PATHBUF];
    char logp[PATHBUF], qlogp[PATHBUF], sock[PATHBUF], pidp[PATHBUF], sm[PATHBUF];
    pid_t mid, pid = -1;
    int fd, pfd[2], attempt, had_suspend = 0, launched = 0;

    vm_fix_ports(v);
    { char pp[PATHBUF]; paused_path_for(v->name, pp, sizeof pp); unlink(pp); }
    suspend_path_for(v->name, sm, sizeof sm);
    if (file_exists(sm) && !disk_has_suspend(v->hda)) unlink(sm);   /* 过期标记 */
    vm_build_argv(v, argv, MAXARGV, pool, MAXARGV, 1, "auto");
    log_path_for(v, logp, sizeof logp);
    {
        char d[PATHBUF];
        runtime_dir_for(v->name, d, sizeof d);
        snprintf(qlogp, sizeof qlogp, "%s/%s.qemu.log", d, v->name);
    }
    qmp_path_for(v->name, sock, sizeof sock);
    {
        int i;
        for (i = 0; argv[i]; i++) if (!strcmp(argv[i], "-loadvm")) had_suspend = 1;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        char got[32], eline[512];
        eline[0] = '\0';
        pfd[0] = pfd[1] = -1;
        unlink(sock);
        if (pipe(pfd) != 0) { snprintf(err, errn, "pipe 失败"); return -1; }

        /* 双 fork：qemu 交给 init 收养，vvm 不留僵尸 */
        mid = fork();
        if (mid == 0) {
            pid_t g;
            close(pfd[0]);
            g = fork();
            if (g == 0) {
                int devnull, logfd;
                setsid();
                close(pfd[1]);
                devnull = open("/dev/null", O_RDONLY);
                dup2(devnull, STDIN_FILENO);
                close(devnull);
                /* stdout 走 *.qemu.log，串口日志那个 *.log 留给 chardev 追加，
                   两份往同一个文件里写会把日志搅烂，还会被 O_TRUNC 清掉旧内容 */
                logfd = open(qlogp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); close(logfd); }
                execvp(argv[0], argv);
                _exit(127);
            }
            {
                char b[32];
                int n = snprintf(b, sizeof b, "%d", (int)g);
                if (write(pfd[1], b, n) < 0) { }
                close(pfd[1]);
            }
            _exit(0);
        }
        if (mid < 0) { snprintf(err, errn, "fork 失败"); close(pfd[0]); close(pfd[1]); return -1; }

        close(pfd[1]);
        got[0] = '\0';
        pid = -1;
        if (read(pfd[0], got, sizeof got - 1) > 0) pid = atoi(got);
        close(pfd[0]);
        waitpid(mid, NULL, 0);   /* 收掉中间的壳 */

        if (pid <= 0) {
            if (attempt == 0 && had_suspend) {
                /* 挂起存档还在，但设备图（比如新加的 UEFI 变量盘）和存档里的
                   设备对不上，qemu 会直接拒绝加载。别让用户卡在“启动失败”里：
                   丢掉过期存档，按正常启动再来一次。 */
                unlink(sm);
                had_suspend = 0;
                vm_build_argv(v, argv, MAXARGV, pool, MAXARGV, 1, "auto");
                snprintf(err, errn, "挂起存档和当前设备不匹配，已跳过挂起并重新启动");
                continue;
            }
            snprintf(err, errn, "启动失败");
            return -1;
        }

        /* qemu 因为参数/端口/存档问题秒退的话，立刻把原因带回去；
           但库存盘/慢存储上 qemu 初始化要一两秒，150ms 就判死会把
           正在启动的 qemu 当失败，重试时反被它自己占住磁盘 */
        {
            int w;
            for (w = 0; w < 20; w++) {
                struct timespec ts = { 0, 100000000L };
                if (pid_alive(pid)) break;
                nanosleep(&ts, NULL);
            }
        }
        if (pid_alive(pid)) { launched = 1; break; }
        qemu_err_brief(v->name, eline, sizeof eline);
        if (attempt == 0 && had_suspend) {
            unlink(sm);
            had_suspend = 0;
            vm_build_argv(v, argv, MAXARGV, pool, MAXARGV, 1, "auto");
            continue;
        }
        snprintf(err, errn, "%s", eline[0] ? eline : "Qemu启动后立刻退出了");
        return -1;
    }
    if (!launched) { snprintf(err, errn, "启动失败"); return -1; }

    unlink(sm);
    pid_path_for(v->name, pidp, sizeof pidp);
    fd = open(pidp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[32];
        snprintf(buf, sizeof buf, "%d\n", (int)pid);
        if (write(fd, buf, strlen(buf)) < 0) { }
        close(fd);
    }
    return (int)pid;
}

/* 扫 /proc 找 cmdline 里带着这块磁盘的 qemu 进程：
   同一块 qcow2 被两个 qemu 同时写，镜像基本就废了，启动前必须先挡一道 */
static int find_qemu_on_disk(const char *hda, int skip_pid, int *pid_out) {
    DIR *d;
    struct dirent *e;
    char p[PATHBUF], buf[4096];
    int found = 0;
    if (!hda || !hda[0]) return 0;
    d = opendir("/proc");
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        int pid = atoi(e->d_name);
        FILE *f;
        size_t n, k, a;
        char *args[96], *slash;
        int nargs = 0;
        if (pid <= 0 || pid == skip_pid || pid == (int)getpid() || pid == (int)getppid()) continue;
        snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
        f = fopen(p, "r");
        if (!f) continue;
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (n == 0) continue;
        buf[n] = '\0';
        /* 按 NUL 拆 argv：这样 "disk 路径正好等于某个参数" 才算占用，
           以前整串 strstr 会把 grep/cat/编辑器甚至 shell 命令都当成 qemu */
        a = 0;
        while (a < n && nargs < 96) {
            args[nargs++] = buf + a;
            a += strlen(buf + a) + 1;
            if (buf[a - 1] == '\0' && a >= n) break;
        }
        if (nargs < 2) continue;
        slash = strrchr(args[0], '/');
        if (!strstr(slash ? slash + 1 : args[0], "qemu-system") &&
            !strstr(slash ? slash + 1 : args[0], "qemu-kvm")) continue;
        for (k = 1; k < (size_t)nargs; k++) {
            const char *arg2 = args[k];
            const char *vp = arg2;
            if (strncmp(vp, "file=", 5) == 0) vp += 5;
            /* 真正的 -drive 参数常带后缀：file=/x.qcow2,if=virtio,format=qcow2。
               以前只认 "file=路径" 完全相等，带一个逗号就漏判，于是放行后
               qemu 自己报 "Is another process using the image"。 */
            {
                size_t hl = strlen(hda);
                int hit = 0;
                if (strncmp(vp, hda, hl) == 0 && (vp[hl] == '\0' || vp[hl] == ',')) hit = 1;
                /* qemu 选项里的逗号写法是 ",,"；比较前把路径也按同样规则展开 */
                if (!hit) {
                    char enc[PATHBUF * 2], i2;
                    size_t x, w = 0;
                    for (x = 0; hda[x] && w < sizeof enc - 3; x++) {
                        enc[w++] = hda[x];
                        if (hda[x] == ',') enc[w++] = ',';
                    }
                    enc[w] = '\0';
                    (void)i2;
                    if (strncmp(vp, enc, w) == 0 && (vp[w] == '\0' || vp[w] == ',')) hit = 1;
                }
                if (!hit) {
                    /* 软链接 / /storage 与 /mnt/media_rw 这类别名，按真实路径再比一次。
                       realpath 写固定缓冲区需要 PATH_MAX，传 512 字节栈缓冲会被它写爆，
                       所以传 NULL 让它自己 malloc。带逗号的参数不参与这项比较。 */
                    char tmp[PATHBUF];
                    size_t x;
                    for (x = 0; vp[x] && vp[x] != ',' && x < sizeof tmp - 1; x++) tmp[x] = vp[x];
                    tmp[x] = '\0';
                    if (!strchr(vp, ',')) {
                        char *r1 = realpath(hda, NULL);
                        char *r2 = realpath(tmp, NULL);
                        if (r1 && r2 && strcmp(r1, r2) == 0) hit = 1;
                        free(r1);
                        free(r2);
                    }
                }
                if (hit) { *pid_out = pid; found = 1; break; }
            }
        }
    }
    closedir(d);
    return found;
}

/* 进程是不是真的还活着（僵尸/已退出都不算） */
static int proc_really_alive(int pid) {
    char p[PATHBUF], buf[512];
    FILE *f;
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    f = fopen(p, "r");
    if (!f) return 0;
    if (fread(buf, 1, sizeof buf - 1, f) <= 0) { fclose(f); return 0; }
    fclose(f);
    buf[sizeof buf - 1] = '\0';
    {   /* stat: pid (comm) state ... */
        char *rp = strrchr(buf, ')');
        if (!rp || !rp[1]) return 0;
        if (rp[2] == 'Z' || rp[2] == 'X' || rp[2] == 'x') return 0;
    }
    return 1;
}

/* 把 /proc/<pid>/cmdline 拼成一行，确认框里给人看清楚是谁占着 */
static void proc_cmdline(int pid, char *out, int n) {
    char p[PATHBUF], buf[1024];
    FILE *f;
    size_t rd, i;
    int len = 0;
    out[0] = '\0';
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    f = fopen(p, "r");
    if (!f) return;
    rd = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[rd] = '\0';
    for (i = 0; i < rd && len < n - 2; i++) {
        char c = buf[i];
        out[len++] = (c == '\0') ? ' ' : c;
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
}

/* 返回 0 = 可以继续启动，1 = 别启动了 */
static int disk_conflict_guard(const VM *v) {
    char p[PATHBUF];
    int pid = 0, tries, alive = 0;
    if (!v->hda[0]) return 0;
    /* 刚 kill 完 / 刚退出还没被回收的进程，可能还挂在 /proc 里一会儿。
       先连看 1 秒，等它真消失，别一上来就弹“被占用”。 */
    for (tries = 0; tries < 5; tries++) {
        pid = 0;
        if (!find_qemu_on_disk(v->hda, read_pid(v->name), &pid)) return 0;
        if (!proc_really_alive(pid)) pid = 0;
        if (!pid) {   /* 已经没了或者只是残留：把旧 pid 文件清掉，直接开 */
            char pp[PATHBUF];
            pid_path_for(v->name, pp, sizeof pp);
            unlink(pp);
            return 0;
        }
        alive = pid;
        {
            struct timespec ts = { 0, 200000000L };
            nanosleep(&ts, NULL);          /* 给它一点时间自己退干净 */
        }
    }
    if (!alive) return 0;
    {
        int fp = read_pid(v->name);
        if (fp > 0 && fp == alive) {   /* 占着的是这台虚拟机自己：别杀，提示一句 */
            char m[160];
            snprintf(m, sizeof m, "虚拟机已经在运行 [PID %d]，主页按 b 打开控制台，或先正常关机", alive);
            status_set(2, m);
            return 1;
        }
    }
    {
        char cmd[200], q[768];
        proc_cmdline(alive, cmd, sizeof cmd);
        snprintf(q, sizeof q, "磁盘正被别的Qemu占用\nPID：%d\n%s\n%s\n关闭该进程再启动？[同时写会导致镜像损坏]",
                 alive, v->hda, cmd);
        if (!ui_confirm("%s", q)) return 1;
    }
    kill(alive, SIGKILL);
    /* 杀完最多等 3 秒，等它真的从 /proc 消失或在变僵尸，才算干净 */
    for (tries = 0; tries < 10; tries++) {
        int now = 0;
        struct timespec ts = { 0, 300000000L };
        if (!find_qemu_on_disk(v->hda, read_pid(v->name), &now) || !proc_really_alive(now)) break;
        nanosleep(&ts, NULL);
    }
    {
        int now = 0;
        if (find_qemu_on_disk(v->hda, read_pid(v->name), &now) && proc_really_alive(now)) {
            ui_msg("PID %d 未能关闭进程，手动处理", now);
            return 1;
        }
    }
    snprintf(p, sizeof p, "已强制关闭占着磁盘的进程，继续启动");
    status_set(0, p);
    return 0;
}

/* 读 qemu 自己报错的那份日志最后一行 */
static void qemu_err_line(const char *name, char *out, int n) {
    char p[PATHBUF], buf[4096], last[512];
    FILE *f;
    long sz;
    size_t len;
    char *line, *nl;

    out[0] = '\0';
    last[0] = '\0';
    { char d[PATHBUF]; runtime_dir_for(name, d, sizeof d); snprintf(p, sizeof p, "%s/%s.qemu.log", d, name); }
    f = fopen(p, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz > (long)sizeof(buf) - 1) fseek(f, sz - (long)sizeof(buf) + 1, SEEK_SET);
    else fseek(f, 0, SEEK_SET);
    len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    for (line = buf; line && *line; line = nl ? nl + 1 : NULL) {
        size_t l;
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (*line == '\r') line++;
        l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
        if (l) snprintf(last, sizeof last, "%s", line);
    }
    snprintf(out, n, "%s", last);
}

/* qemu 的报错行一般是 "qemu-system-x86_64: 真正的错在冒号后面"，
   这里统一剥一层，省得每个调用点各写一遍还各写歪一遍 */
static void qemu_err_brief(const char *name, char *out, int n) {
    char line[512];

    qemu_err_line(name, line, sizeof line);
    out[0] = '\0';
    if (!line[0]) return;
    {
        const char *c = strstr(line, ": ");
        snprintf(out, n, "%s", (c && c - line < 80) ? c + 2 : line);
    }
}

/* ---- 控制台：qemu 串口挂到 pty，ncurses 里显示并可输入；Ctrl+] 退回界面 ---- */
#define CGRID 400
#define CCOL  512
#define CAT_DEF 0x99                 /* fg=9/bg=9 表示“终端默认色” */
static char cgrid_main[CGRID][CCOL];
static char cgrid_alt[CGRID][CCOL];
static unsigned short catt_main[CGRID][CCOL];
static unsigned short catt_alt[CGRID][CCOL];
static char (*cgrid)[CCOL] = cgrid_main;
static unsigned short (*catt)[CCOL] = catt_main;
static int  c_alt, alt_cy, alt_cx;
static int  ccy, ccx;
static int  c_top;           /* 当前“一屏”的起始行，BIOS 里 ESC[H 回到这里 */
static int  c_disp_top;      /* 上一次 console_draw 实际画出来的首行 */
static int  c_rows = 24;     /* 控制台可见行数，绘制时更新 */
static int  c_save_y = -1, c_save_x;
static int  c_have_screen;   /* 这一屏是否已经定过起点 */
static unsigned short cur_attr = CAT_DEF;

static volatile sig_atomic_t c_winch;

static void sig_winch(int sig) { (void)sig; c_winch = 1; }

static unsigned short mkatt(int fg, int bg, int bold, int rev) {
    return (unsigned short)((fg & 0xF) | ((bg & 0xF) << 4) | (bold ? 0x100 : 0) | (rev ? 0x200 : 0));
}

/* 属性 -> ncurses：8 种前景 × 8 种背景，配对号 8 + fg*8 + bg */
static int nattr(unsigned short a) {
    int fg = a & 0xF, bg = (a >> 4) & 0xF, at = 0;
    if (a & 0x100) at |= A_BOLD;
    if (a & 0x200) at |= A_REVERSE;
    if (fg < 8 || bg < 8) {
        int fi = (fg == 9) ? 0 : fg + 1;      /* 0 = 默认色 */
        int bi = (bg == 9) ? 0 : bg + 1;
        at |= COLOR_PAIR(8 + fi * 9 + bi);
    }
    return at;
}

static void att_fill_row(unsigned short (*tab)[CCOL], int r, int from) {
    int i;
    for (i = from; i < CCOL; i++) tab[r][i] = CAT_DEF;
}

/* 当前显示的“这一屏”的顶行：正常情况下 ccy 不会跑出 c_top..c_top+rows-1。
   以前回话给客机（ESC[6n）时临时拿 ccy-(rows-1) 当顶，客机停在屏幕上方时
   它会以为光标在最底行，bash/readline 就每敲一个键往下挤一行——看着像“跳格”。 */
static void ctop_fix(void) {
    if (c_rows < 1) c_rows = 1;
    if (ccy < c_top) c_top = ccy;
    if (ccy - c_top > c_rows - 1) c_top = ccy - (c_rows - 1);
    if (c_top < 0) c_top = 0;
    if (c_top >= CGRID) c_top = CGRID - 1;
}

static void console_scroll(void) {
    if (c_top > 0) c_top--;
    if (c_save_y > 0) c_save_y--;
    memmove(cgrid[0], cgrid[1], sizeof(cgrid_main[0]) * (CGRID - 1));
    memmove(catt[0], catt[1], sizeof(catt_main[0]) * (CGRID - 1));
    memset(cgrid[CGRID - 1], 0, CCOL);
    att_fill_row(catt, CGRID - 1, 0);
    ccy = CGRID - 1;
}

static int  cesc;            /* 0 普通 1 收到 ESC 2 CSI 3 OSC */
static int  c_off;           /* 回看行数（0 = 贴着最新输出） */
static int  c_mfd = -1;      /* 串口主端，回复终端查询用 */
static int  c_show = 1;      /* 光标可见 */
static int  cnum, chas;
static int  csi_np, csi_p[16], csi_priv, csi_seen;

static void console_clr_eol(int mode) {
    int i;
    if (mode == 0) {
        for (i = ccx; i < CCOL; i++) { cgrid[ccy][i] = '\0'; catt[ccy][i] = CAT_DEF; }
        if (ccx < CCOL) cgrid[ccy][ccx] = '\0';
    } else if (mode == 1) {
        for (i = 0; i <= ccx && i < CCOL; i++) { cgrid[ccy][i] = ' '; catt[ccy][i] = CAT_DEF; }
    } else {
        memset(cgrid[ccy], 0, CCOL);
        att_fill_row(catt, ccy, 0);
    }
}

static void console_move(int dr, int dc) {
    int i;
    if (dr > 0) {
        for (i = 0; i < dr; i++) { if (++ccy >= CGRID) { console_scroll(); } }
    } else if (dr < 0) {
        ccy += dr; if (ccy < 0) ccy = 0;
    }
    if (dc > 0) { ccx += dc; if (ccx > CCOL - 2) ccx = CCOL - 2; }
    else if (dc < 0) { ccx += dc; if (ccx < 0) ccx = 0; }
    ctop_fix();
}

/* SGR：只认 8 色 + 粗体 + 反显，256 色按 38;5;n 跳过 */
static void console_sgr(void) {
    int i;
    if (!csi_seen && csi_np == 0) { cur_attr = CAT_DEF; return; }
    for (i = 0; i <= csi_np; i++) {
        int v = csi_p[i];
        int fg = cur_attr & 0xF, bg = (cur_attr >> 4) & 0xF;
        int b = (cur_attr & 0x100) != 0, r = (cur_attr & 0x200) != 0;
        if (v == 0) { cur_attr = CAT_DEF; continue; }
        if (v == 1) { cur_attr = mkatt(fg, bg, 1, r); continue; }
        if (v == 22) { cur_attr = mkatt(fg, bg, 0, r); continue; }
        if (v == 7) { cur_attr = mkatt(fg, bg, b, 1); continue; }
        if (v == 27) { cur_attr = mkatt(fg, bg, b, 0); continue; }
        if (v >= 30 && v <= 37) { cur_attr = mkatt(v - 30, bg, b, r); continue; }
        if (v == 39) { cur_attr = mkatt(9, bg, b, r); continue; }
        if (v >= 40 && v <= 47) { cur_attr = mkatt(fg, v - 40, b, r); continue; }
        if (v == 49) { cur_attr = mkatt(fg, 9, b, r); continue; }
        if (v >= 90 && v <= 97) { cur_attr = mkatt(v - 90, bg, 1, r); continue; }
        if (v >= 100 && v <= 107) { cur_attr = mkatt(fg, v - 100, b, r); continue; }
        if (v == 38 || v == 48) {
            int skip = (i + 1 <= csi_np && csi_p[i + 1] == 5) ? 3 : 1;
            i += skip - 1;
            continue;
        }
    }
}


/* 串口写入：非阻塞 socket 遇到 EAGAIN 就等一小会再写，不能把按键丢了 */
static int serial_write_all(int fd, const unsigned char *p, int n) {
    int off = 0;
    while (off < n) {
        ssize_t r = write(fd, p + off, (size_t)(n - off));
        if (r > 0) { off += (int)r; continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fd_set wf;
            struct timeval tv2;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            tv2.tv_sec = 0; tv2.tv_usec = 50000;
            if (select(fd + 1, NULL, &wf, NULL, &tv2) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
static void console_feed(const unsigned char *p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];

        if (cesc == 1) {                       /* ESC 之后 */
            if (c == '[') { cesc = 2; cnum = 0; chas = 0; csi_np = 0; csi_p[0] = 0; csi_priv = 0; csi_seen = 0; }
            else if (c == ']') { cesc = 3; }
            else if (c == '7') { c_save_y = ccy; c_save_x = ccx; }        /* DECSC */
            else if (c == '8') { if (c_save_y >= 0) { ccy = c_save_y; ccx = c_save_x; } }
            else if (c == '(' || c == ')' || c == '#') { cesc = 4; }      /* 字符集/对齐，吃一个参数 */
            else cesc = 0;
            continue;
        }
        if (cesc == 4) { cesc = 0; continue; }
        if (cesc == 2) {                       /* CSI 参数与终结符 */
            if (c >= '0' && c <= '9') {
                csi_p[csi_np] = csi_p[csi_np] * 10 + (c - '0');
                if (csi_p[csi_np] > 9999) csi_p[csi_np] = 9999;
                csi_seen = 1; chas = 1;
                cnum = csi_p[0];
                continue;
            }
            if (c == ';') { if (csi_np < 15) { csi_np++; csi_p[csi_np] = 0; } continue; }
            if (c == '?') { csi_priv = 1; continue; }
            if (c >= 0x20 && c <= 0x2f) { continue; }
            {
                int nn = chas ? csi_p[0] : 1;
                switch (c) {
                case 'K': console_clr_eol(chas ? csi_p[0] : 0); break;
                case 'm': console_sgr(); break;
                case 'h':
                case 'l':
                    if (!chas || csi_p[0] == 25) {
                        if (c == 'h') c_show = 1; else if (chas && csi_p[0] == 25) c_show = 0;
                        break;
                    }
                    if (chas && (csi_p[0] == 1049 || csi_p[0] == 47 || csi_p[0] == 1047)) {
                        if (c == 'h') {                 /* 进全屏程序：换备用屏，别毁掉开机日志 */
                            if (!c_alt) { alt_cy = ccy; alt_cx = ccx; }
                            c_alt = 1;
                            cgrid = cgrid_alt;
                            catt = catt_alt;
                            memset(cgrid_alt, 0, sizeof cgrid_alt);
                            {
                                int rr;
                                for (rr = 0; rr < CGRID; rr++) att_fill_row(catt_alt, rr, 0);
                            }
                            ccy = 0; ccx = 0; c_top = 0; c_have_screen = 0;
                        } else {                        /* 退出全屏：回主屏，历史还在 */
                            if (c_alt) { c_alt = 0; cgrid = cgrid_main; catt = catt_main; ccy = alt_cy; ccx = alt_cx; }
                        }
                    }
                    break;
                case 'J':
                    if (chas && csi_p[0] == 2) {
                        if (c_alt) {
                            int rr;
                            memset(cgrid_alt, 0, sizeof cgrid_alt);
                            for (rr = 0; rr < CGRID; rr++) att_fill_row(catt_alt, rr, 0);
                            ccy = 0; ccx = 0;
                        }
                        else {
                            /* 主屏清屏：只擦当前可见的这一屏，滚过去的开机日志保留。
                               起点每次按光标位置重算，不能只在第一次算——否则 GRUB
                               留下的菜单底纹会一直留在屏幕上，变成一块块白底。 */
                            int rr, topv, lim2;
                            topv = ccy - (c_rows - 1);
                            if (topv < 0) topv = 0;
                            c_top = topv;
                            c_have_screen = 1;
                            lim2 = topv + c_rows;
                            if (lim2 > CGRID) lim2 = CGRID;
                            for (rr = topv; rr < lim2; rr++) {
                                memset(cgrid[rr], 0, CCOL);
                                att_fill_row(catt, rr, 0);
                            }
                            ccx = 0;
                            ccy = topv;
                        }
                    }
                    else {
                        int r;
                        console_clr_eol(0);
                        for (r = ccy + 1; r < CGRID; r++) {
                            memset(cgrid[r], 0, CCOL);
                            att_fill_row(catt, r, 0);
                        }
                    }
                    break;
                case 'A': console_move(-nn, 0); break;
                case 'B': console_move(nn, 0); break;
                case 'C': console_move(0, nn); break;
                case 'D': console_move(0, -nn); break;
                case 'G': ccx = (chas ? csi_p[0] : 1) - 1; if (ccx < 0) ccx = 0; if (ccx > CCOL - 2) ccx = CCOL - 2; break;
                case 'n':                                  /* ESC[6n：你在哪 */
                    if (chas && csi_p[0] == 6 && c_mfd >= 0) {
                        char rep[32];
                        int vr, start;
                        /* 只能回答“屏幕上第几行”，不能把 400 行的内部网格泄漏给客机，
                           否则 UEFI Shell 会以为终端有几百行，光标定位全乱、画面被反复覆盖 */
                        start = c_disp_top;
                        if (start < 0) start = 0;
                        vr = ccy - start + 1;
                        if (vr < 1) vr = 1;
                        if (vr > c_rows) vr = c_rows;
                        snprintf(rep, sizeof rep, "\x1b[%d;%dR", vr, ccx + 1);
                        if (write(c_mfd, rep, strlen(rep)) < 0) { }
                    }
                    break;
                case 't':                                  /* ESC[18t：报一下窗口有多少行多少列 */
                    if (c_mfd >= 0 && chas && csi_p[0] == 18) {
                        char rep[32];
                        int cols = COLS - 1;
                        snprintf(rep, sizeof rep, "\x1b[8;%d;%dt", c_rows, cols > 0 ? cols : 80);
                        if (write(c_mfd, rep, strlen(rep)) < 0) { }
                    }
                    break;
                case 'c':                                  /* ESC[c：你是什么终端 */
                    if (c_mfd >= 0 && write(c_mfd, "\x1b[?6c", 6) < 0) { }
                    break;
                case 'H':
                case 'f': {
                    int row = (csi_seen && csi_p[0] > 0) ? csi_p[0] : 1;
                    int col = (csi_np >= 1 && csi_p[1] > 0) ? csi_p[1] : 1;
                    /* 回到"第 1 行"要落在当前看得见的这一屏顶上：
                       以前用早先 ESC[2J 记下的 c_top，输出滚过几屏后它就过期了，
                       UEFI Shell 一回家就回到屏幕上方几十行，提示符和列表互相压，字就花了 */
                    int base;
                    if (c_alt) base = 0;
                    else base = c_disp_top;
                    ccy = base + row - 1;
                    if (ccy >= CGRID) ccy = CGRID - 1;
                    if (ccy < 0) ccy = 0;
                    ccx = col - 1;
                    if (ccx < 0) ccx = 0;
                    if (ccx > CCOL - 2) ccx = CCOL - 2;
                    break;
                }
                default: break;
                }
            }
            cesc = 0;
            continue;
        }
        if (cesc == 3) { if (c == 7) cesc = 0; continue; }
        if (c == 0x1b) { cesc = 1; continue; }

        if (c == '\r') { ccx = 0; continue; }
        if (c == '\n') { ccy++; ccx = 0; if (ccy >= CGRID) console_scroll(); continue; }
        if (c == '\b') { if (ccx > 0) ccx--; continue; }   /* 退格只移光标，不擦字符 */
        if (c == '\t') {
            do { if (ccx < CCOL - 1) ccx++; } while ((ccx % 8) && ccx < CCOL - 1);   /* 制表符也只移光标 */
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        if (ccx >= CCOL - 1) { ccy++; ccx = 0; if (ccy >= CGRID) console_scroll(); }
        catt[ccy][ccx] = cur_attr;
        cgrid[ccy][ccx] = (char)c;
        ctop_fix();
        if (ccx < CCOL - 2) ccx++;      /* 只前进，不要在后面补 NUL，否则会擦掉右边已有的字 */
    }
}

/* 从 .log 里把开机那几屏灌回网格，接已有虚拟机时也能看到历史 */
/* 旧实现会把上次日志末段铺进控制台，重连时看到的全是过期内容，已废弃。 */

static void console_draw(WINDOW *w, const char *title) {
    int rows = getmaxy(w) - 1, i, start, wid = getmaxx(w);
    if (rows < 1) rows = 1;
    c_rows = rows;
    if (c_off > ccy) c_off = ccy;
    if (c_off < 0) c_off = 0;
    start = ccy - rows + 1 - c_off;
    if (start < 0) start = 0;
    c_disp_top = start;
    for (i = 0; i < rows; i++) {
        int r = start + i;
        wmove(w, i, 0);
        wclrtoeol(w);
        if (r > ccy || r >= CGRID || r < 0) continue;
        {
            int x = 0, lim = wid - 1, last = -1;   /* 留一列，写满会触发自动换行把行推乱 */
            for (x = 0; x < CCOL; x++) if (cgrid[r][x]) last = x;
            x = 0;
            while (x <= last && x < lim) {
                char tmp[CCOL];
                int tl = 0, j = x;
                unsigned short a = catt[r][x];
                while (j <= last && j < lim && catt[r][j] == a) {
                    tmp[tl++] = cgrid[r][j] ? cgrid[r][j] : ' ';  /* 中间空列补空格，别断行 */
                    j++;
                }
                wattrset(w, nattr(a));
                waddnstr(w, tmp, tl);
                x = j;
            }
            wattrset(w, A_NORMAL);
        }
    }
    {
        /* 串口一个字节都没收到时，别让用户对着一块黑屏猜 */
        int any = 0, r2, x2;
        for (r2 = 0; r2 <= ccy && r2 < CGRID && !any; r2++)
            for (x2 = 0; x2 < CCOL; x2++) if (cgrid[r2][x2]) { any = 1; break; }
        if (!any) {
            const char *lines[5] = {
                "如果你能看到这些文字，说明虚拟机没有向串口",
                "控制台输出任何字符。但是你不妨尝试按一下回车键，",
                "也许会看到有字符出现，因为它不会自动刷新，",
                "而且部分系统开机时日志无法发送到串口控制台，",
                "这里看不见不意味着 VNC 没有画面，你可以切换到VNC确认"
            };
            int y0, i2, maxw = dwidth("[INFO]"), bx;
            for (i2 = 0; i2 < 5; i2++) if (dwidth(lines[i2]) > maxw) maxw = dwidth(lines[i2]);
            y0 = rows / 2 - 3;
            if (y0 < 1) y0 = 1;
            bx = (wid - maxw) / 2;              /* 整块按最宽那行居中 */
            if (bx < 1) bx = 1;
            /* 整块居中，块内一律左对齐（[INFO] 也贴块的左边） */
            put_u8(w, y0, bx, "[INFO]", wid, COLOR_PAIR(95) | A_BOLD);
            for (i2 = 0; i2 < 5; i2++)
                put_u8(w, y0 + 1 + i2, bx, lines[i2], wid, g_grey);
        }
    }
    wattron(w, COLOR_PAIR(6));
    mvwhline(w, getmaxy(w) - 1, 0, ' ', wid);
    {
        char full[320];
        if (c_off > 0) snprintf(full, sizeof full, " ↑%d 行回看   Ctrl+ALT+Q 退出控制台 ", c_off);
        else snprintf(full, sizeof full, "%s", title);
        waddnstr(w, full, wid - 2);
    }
    wattroff(w, COLOR_PAIR(6));
    {
        int cx = ccx;
        int cy = ccy - start;              /* 光标跟文字走，不能钉在窗口底行 */
        if (cx > wid - 2) cx = wid - 2;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        if (c_off > 0) wmove(w, rows - 1, 0);
        else wmove(w, cy, cx);
    }
    {
        static int last_show = -1;
        if (c_show != last_show) { curs_set(c_show ? 1 : 0); last_show = c_show; }
    }
    wrefresh(w);
}

static void serial_sock_path(const char *name, char *out, int n) {
    char d[PATHBUF], sn[PATHBUF];
    sock_dir_for(name, "vvm-", d, sizeof d);
    sock_name(name, sn, sizeof sn);
    snprintf(out, n, "%s/vvm-%s.sock", d, sn);
}

static int sock_connect_retry(const char *path, int ms, pid_t watch) {
    int waited = 0;
    while (1) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sa;
        if (fd < 0) return -1;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) return fd;
        close(fd);
        if (watch > 0 && !pid_alive(watch)) return -1;   /* qemu 已经死了就别傻等 */
        if (waited >= ms) return -1;
        usleep(20000);
        waited += 20;
    }
}

static char c_last_vm[64];   /* 上一次进控制台的是哪台，换机器必须清屏 */
static int c_disp_top;       /* 上一次 console_draw 画的“屏幕第一行”在网格里的行号 */

/* 这台 VM 到底有没有在跑：先看 pid 文件，pid 文件丢了/过期就按磁盘扫一遍，
   找到真正的 qemu 再把 pid 文件补回去。以前只信 pid 文件，所以残留 qemu
   运行中却显示“未启动”，按 b 会再去起一个，然后撞上镜像锁进不了控制台。 */
static int vm_live_pid(VM *v) {
    int pid = read_pid(v->name), found = 0;
    char pp[PATHBUF];
    if (pid > 0 && pid_alive(pid)) return pid;
    if (v->hda[0] && find_qemu_on_disk(v->hda, 0, &found) && found > 0) {
        pid_path_for(v->name, pp, sizeof pp);
        { FILE *f = fopen(pp, "w"); if (f) { fprintf(f, "%d\n", found); fclose(f); } }
        return found;
    }
    return 0;
}

static void vm_start_foreground(VM *v) {
    char sockpath[PATHBUF], spec[PATHBUF];
    char *argv[MAXARGV];
    char pool[MAXARGV][PATHBUF];
    struct termios old_t, raw_t;
    WINDOW *w;
    fd_set fds;
    struct timeval tv;
    char esc_buf[8] = { 0 };
    int esc_len = 0;
    int master = -1, n;
    int dead = 0, knew = 1;
    char name_copy[64];
    pid_t pid;

    snprintf(name_copy, sizeof name_copy, "%s", v->name);
    serial_sock_path(v->name, sockpath, sizeof sockpath);
    snprintf(spec, sizeof spec, "auto");

    /* 只要串口 socket 能连上，就直接接控制台——不信 pid 文件，也不先起新 qemu。
       有些机器 pid 文件丢了、名字对应不上，但 qemu 明明在跑；先连 socket 最稳。 */
    pid = vm_live_pid(v);
    master = sock_connect_retry(sockpath, 0, 0);
    if (master >= 0) {
        if (pid <= 0) { int fp = 0; if (v->hda[0] && find_qemu_on_disk(v->hda, 0, &fp) && fp > 0) pid = fp; }
    } else if (pid > 0) {
        ui_msg("虚拟机运行[pid %d]，但串口没有连接\n这大概是旧版本启动的，退出后重新启动一次即可\n图形界面用 VNC:%s:%d", pid, v->vnc_addr, v->vnc_port);
        return;
    } else {
        int pfd[2] = { -1, -1 };
        char got[32];
        pid_t mid;
        vm_fix_ports(v);
        { char pp[PATHBUF]; paused_path_for(v->name, pp, sizeof pp); unlink(pp); }
        got[0] = '\0';
        unlink(sockpath);
        vm_build_argv(v, argv, MAXARGV, pool, MAXARGV, 2, spec);
        if (pipe(pfd) != 0) { ui_msg("启动失败：pipe"); return; }
        mid = fork();
        if (mid == 0) {
            pid_t g;
            close(pfd[0]);
            setsid();
            g = fork();
            if (g == 0) {
                char b[32];
                snprintf(b, sizeof b, "%d\n", (int)getpid());
                if (write(pfd[1], b, strlen(b)) < 0) { }
                close(pfd[1]);
                {
                    char qlog[PATHBUF];
                    int efd;
                    { char d[PATHBUF]; runtime_dir_for(name_copy, d, sizeof d); snprintf(qlog, sizeof qlog, "%s/%s.qemu.log", d, name_copy); }
                    efd = open(qlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (efd >= 0) { dup2(efd, STDERR_FILENO); close(efd); }
                }
                execvp(argv[0], argv);
                _exit(127);
            }
            _exit(0);
        }
        close(pfd[1]);
        if (mid < 0) { close(pfd[0]); ui_msg("启动失败：无法 fork"); return; }
        if (read(pfd[0], got, sizeof got - 1) <= 0) got[0] = '\0';
        close(pfd[0]);
        waitpid(mid, NULL, 0);
        pid = atoi(got);
        if (pid <= 0) { ui_msg("启动失败：qemu 没起来"); return; }

        /* pid 先落盘：串口连不上也得让人知道这台在跑，
           否则下次又会起第二台 qemu 来抢同一块磁盘 */
        {
            char pidp[PATHBUF], sm[PATHBUF];
            int fd;
            pid_path_for(v->name, pidp, sizeof pidp);
            fd = open(pidp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char buf[32];
                snprintf(buf, sizeof buf, "%d\n", (int)pid);
                if (write(fd, buf, strlen(buf)) < 0) { }
                close(fd);
            }
            suspend_path_for(v->name, sm, sizeof sm);
            unlink(sm);
        }

        master = sock_connect_retry(sockpath, 2500, pid);
        if (master < 0) {
            char eline[512];
            qemu_err_brief(name_copy, eline, sizeof eline);
            if (eline[0]) ui_msg("%s", eline);
            else ui_msg("控制台无法连接到串口，使用VNC控制");
            return;
        }
    }

    fcntl(master, F_SETFL, O_NONBLOCK);
    /* 同一台重连保留屏上历史，换了虚拟机才清，别一进去就把日志擦光 */
    if (strcmp(c_last_vm, name_copy) != 0) {
        int rr;
        ccy = 0; ccx = 0;
        memset(cgrid_main, 0, sizeof cgrid_main);
        memset(cgrid_alt, 0, sizeof cgrid_alt);
        for (rr = 0; rr < CGRID; rr++) { att_fill_row(catt_main, rr, 0); att_fill_row(catt_alt, rr, 0); }
        cgrid = cgrid_main; catt = catt_main;
        c_alt = 0;
        cur_attr = CAT_DEF;
        c_top = 0;
        c_have_screen = 0;
        c_save_y = -1;
    }

    snprintf(c_last_vm, sizeof c_last_vm, "%s", name_copy);
    c_off = 0;
    c_show = 1;
    c_mfd = master;
    c_winch = 0;
    signal(SIGWINCH, sig_winch);

    w = newwin(LINES, COLS, 0, 0);
    g_top = w;
    keypad(w, FALSE);
    curs_set(1);
    tcgetattr(STDIN_FILENO, &old_t);
    raw_t = old_t;
    cfmakeraw(&raw_t);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_t);

    {
        char title[256];
        snprintf(title, sizeof title, " %s - %s   Ctrl+ALT+Q / Ctrl+] 退出   VNC:%s:%d SSH:%d ",
                 v->name, v->arch, v->vnc_addr, v->vnc_port, v->ssh_port);
        while (!dead) {
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            FD_SET(master, &fds);
            tv.tv_sec = 0; tv.tv_usec = 60000;
            if (select((master > STDIN_FILENO ? master : STDIN_FILENO) + 1, &fds, NULL, NULL, &tv) > 0) {
                if (FD_ISSET(master, &fds)) {
                    unsigned char buf[4096];
                    n = (int)read(master, buf, sizeof buf);
                    if (n > 0) { console_feed(buf, n); c_off = 0; knew = 1; }
                    else if (n == 0) dead = 1;      /* socket 被对端关掉 = qemu 没了 */
                }
                if (FD_ISSET(STDIN_FILENO, &fds)) {
                    unsigned char k[64];
                    n = (int)read(STDIN_FILENO, k, sizeof k);
                    if (n > 0) {
                        int i, rows_now = getmaxy(w) - 1;
                        for (i = 0; i < n; i++) {
                            unsigned char b = k[i];
                            if (b == 0x1d) { dead = 2; break; }   /* Ctrl+] 退出；Ctrl+Q 留给客机软件 */
                            if (esc_len || b == 0x1b) {
                                if (esc_len < (int)sizeof esc_buf) esc_buf[esc_len++] = b;
                                else { esc_len = 0; }
                                esc_buf[esc_len] = 0;
                                /* Ctrl+Alt+Q：终端发 ESC + 0x11，这里认作退出，普通 Ctrl+Q 不受影响 */
                                if (esc_len == 2 && esc_buf[0] == 0x1b && esc_buf[1] == 0x11) {
                                    dead = 2; esc_len = 0; break;
                                }
                                /* PgUp / PgDn：回看历史，不发给 guest */
                                if (esc_len == 4 && esc_buf[0] == 0x1b && esc_buf[1] == '[' &&
                                    esc_buf[3] == '~' && (esc_buf[2] == '5' || esc_buf[2] == '6')) {
                                    if (esc_buf[2] == '5') {
                                        c_off += rows_now - 1;
                                        if (c_off > ccy) c_off = ccy;
                                    } else {
                                        c_off -= rows_now - 1;
                                        if (c_off < 0) c_off = 0;
                                    }
                                    esc_len = 0;
                                    knew = 1;
                                    continue;
                                }
                                /* 序列结束就整段转发：ESC O x 是 3 字节；CSI 以 @..~ 收尾 */
                                if (esc_len >= 2 && esc_buf[1] == 'O') {
                                    if (esc_len == 3) {
                                        if (serial_write_all(master, (const unsigned char *)esc_buf, esc_len) < 0) break;
                                        esc_len = 0;
                                    }
                                } else if (esc_len >= 2 && b >= 0x40 && b <= 0x7e) {
                                    if (serial_write_all(master, (const unsigned char *)esc_buf, esc_len) < 0) break;
                                    esc_len = 0;
                                }
                                if (esc_len > 7) { esc_len = 0; }
                                continue;
                            }
                            if (c_off) { c_off = 0; knew = 1; }   /* 一打字就回到最新输出 */
                            {
                                /* 手机键盘的退格是 DEL(0x7F)，串口那边（UEFI Shell、多数
                                   Linux 控制台）认的是 BS(0x08)，转一下，否则删不掉字 */
                                unsigned char sb = (b == 0x7f) ? 0x08 : b;
                                if (serial_write_all(master, &sb, 1) < 0) break;
                            }
                        }
                    }
                }
            }
            /* 单独一个 Esc 不要一直攥着：超时就发给客机，别让它显得「延迟」 */
            if (esc_len == 1 && !FD_ISSET(STDIN_FILENO, &fds)) {
                unsigned char e = 0x1b;
                if (serial_write_all(master, &e, 1) < 0) break;
                esc_len = 0;
            }
            if (c_winch) {
                struct winsize nw;
                c_winch = 0;
                if (ioctl(STDIN_FILENO, TIOCGWINSZ, &nw) == 0 && nw.ws_row > 0 && nw.ws_col > 0) {
                    resizeterm(nw.ws_row, nw.ws_col);
                    close_win(w);
                    w = newwin(LINES, COLS, 0, 0);
    g_top = w;
                    if (!w) w = stdscr;
                    keypad(w, FALSE);
                    clearok(w, TRUE);
                    knew = 1;
                }
            }
            if (pid > 0 && !pid_alive(pid)) dead = 1;   /* 别用 waitpid：SIGCHLD 回收器会先把它收掉 */
            /* pid 未知（纯靠 socket 接进来的）就不看 pid，交给 socket 的 EOF 判断 */
            if (knew || dead) { console_draw(w, title); knew = 0; }
        }
        if (dead == 1) {
            char pidp[PATHBUF];
            pid_path_for(v->name, pidp, sizeof pidp);
            if (pid > 0) unlink(pidp);
            {
                char eline[512];
                qemu_err_brief(name_copy, eline, sizeof eline);
                if (eline[0]) snprintf(title, sizeof title, " 已退出：%s ： 任意键返回 ", eline);
                else snprintf(title, sizeof title, " 虚拟机已退出，按任意键返回 ");
            }
            console_draw(w, title);
            {
                int k;
                for (k = 0; k < 120; k++) {          /* 最多等 6 秒，也可提前按键 */
                    unsigned char t;
                    FD_ZERO(&fds);
                    FD_SET(STDIN_FILENO, &fds);
                    tv.tv_sec = 0; tv.tv_usec = 50000;
                    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                        if (read(STDIN_FILENO, &t, 1) >= 0) break;
                    }
                }
            }
        }
    }

    signal(SIGWINCH, SIG_DFL);
    c_mfd = -1;
    curs_set(0);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
    close_win(w);
    close(master);                 /* 串口是 qemu 自己的 socket，断开不影响它 */
    clear();
    refresh();
    if (dead == 2 && pid > 0 && pid_alive(pid)) status_set(0, "已退出控制台，虚拟机仍在运行");
    else if (dead == 2) status_set(0, "已退出控制台");
}

/* ------------------------------------------------------------------
 * 菜单
 * ------------------------------------------------------------------ */
static const char *ARCH_ITEMS[] = {"x86_64（64 位 PC）", "i386（32 位 PC）", "aarch64（ARM64）", "ppc（PowerPC）"};
static const char *ARCH_VALS[] = {"x86_64", "i386", "aarch64", "ppc"};

static const char *MACHINE_X86[] = {"(默认)", "pc（i440fx）", "q35"};
static const char *MACHINE_X86_V[] = {"", "pc", "q35"};
static const char *MACHINE_ARM[] = {"virt"};
static const char *MACHINE_ARM_V[] = {"virt"};
static const char *MACHINE_PPC[] = {"(默认)", "mac99", "g3beige", "pegasos2", "amigaone", "sam460ex"};
static const char *MACHINE_PPC_V[] = {"", "mac99", "g3beige", "pegasos2", "amigaone", "sam460ex"};

static const char *CPU_X86[] = {"(默认)", "qemu64", "qemu32", "base", "max", "Snowridge", "Cooperlake",
    "Cascadelake-Server", "Skylake-Server", "Skylake-Client", "Broadwell", "Haswell",
    "IvyBridge", "SandyBridge", "Westmere", "Nehalem", "n270", "Penryn", "core2duo",
    "coreduo", "Conroe", "pentium3", "pentium2", "pentium", "486"};
static const char *CPU_X86_V[] = {"", "qemu64", "qemu32", "base", "max", "Snowridge", "Cooperlake",
    "Cascadelake-Server", "Skylake-Server", "Skylake-Client", "Broadwell", "Haswell",
    "IvyBridge", "SandyBridge", "Westmere", "Nehalem", "n270", "Penryn", "core2duo",
    "coreduo", "Conroe", "pentium3", "pentium2", "pentium", "486"};
static const char *CPU_ARM[] = {"max", "cortex-a76", "cortex-a72", "cortex-a57", "cortex-a55",
    "cortex-a53", "cortex-a35", "cortex-a15", "cortex-a9", "cortex-a8", "cortex-a7",
    "cortex-m55", "cortex-m33", "cortex-m7", "cortex-m4", "cortex-m3", "cortex-m0",
    "cortex-r52", "cortex-r5f", "cortex-r5"};
static const char *CPU_ARM_V[] = {"max", "cortex-a76", "cortex-a72", "cortex-a57", "cortex-a55",
    "cortex-a53", "cortex-a35", "cortex-a15", "cortex-a9", "cortex-a8", "cortex-a7",
    "cortex-m55", "cortex-m33", "cortex-m7", "cortex-m4", "cortex-m3", "cortex-m0",
    "cortex-r52", "cortex-r5f", "cortex-r5"};
static const char *CPU_PPC[] = {"g3", "460ex", "e500v2", "7457", "g4", "604e", "603e"};
static const char *CPU_PPC_V[] = {"g3", "460ex", "e500v2", "7457", "g4", "604e", "603e"};

static const char *NIC_ITEMS[] = {"(不接网卡)", "e1000e", "igb", "e1000", "e1000-82545em",
    "e1000-82544gc", "i82801", "i82562", "i82559er", "i82558b", "i82557c", "i82551",
    "i82550", "rtl8139", "pcnet", "ne2k_pci", "vmxnet3", "virtio-net-pci",
    "virtio-net-pci-transitional"};
static const char *NIC_VALS[] = {"", "e1000e", "igb", "e1000", "e1000-82545em",
    "e1000-82544gc", "i82801", "i82562", "i82559er", "i82558b", "i82557c", "i82551",
    "i82550", "rtl8139", "pcnet", "ne2k_pci", "vmxnet3", "virtio-net-pci",
    "virtio-net-pci-transitional"};

static const char *AUDIO_ITEMS[] = {"无声", "hda-duplex", "ac97", "sb16", "es1370", "cs4231a", "adlib"};
static const char *AUDIO_VALS[] = {"none", "hda-duplex", "ac97", "sb16", "es1370", "cs4231a", "adlib"};


static int find_index(const char *const *vals, int n, const char *val) {
    int i;
    for (i = 0; i < n; i++) if (!strcmp(vals[i], val)) return i;
    return 0;
}

/* 纯方向键选择：只改值，不让人手打 */
static int pick_value(const char *title, const char *cur, const char *const *items,
                      const char *const *vals, int n, char *dst, int dstn) {
    int i = find_index(vals, n, cur);
    int sel = ui_menu_hdr(title, cur[0] ? cur : "(空)", items, n);
    if (sel < 0) return 0;
    (void)i;
    snprintf(dst, dstn, "%s", vals[sel]);
    return 1;
}

/* 数字输入：直接在底行敲，Esc 取消；不是数字或越界就提示重来 */
static int ui_num(const char *title, const char *what, int min, int max, int step, int *val) {
    char t[32], prompt[160], msg[160];
    (void)step;
    snprintf(t, sizeof t, "%d", *val);
    snprintf(prompt, sizeof prompt, "%s（%d-%d）：", what, min, max);
    while (1) {
        const char *p;
        int ok = 1, v;
        if (!ui_input(title, prompt, t, t, sizeof t, 0)) return 0;
        if (!t[0]) continue;                       /* 什么都没输就再问一次 */
        for (p = t; *p; p++) if (*p < '0' || *p > '9') { ok = 0; break; }
        if (!ok) { flash_err("只能填数字"); continue; }
        v = atoi(t);
        if (v < min || v > max) {
            snprintf(msg, sizeof msg, "%s要在 %d-%d 之间", what, min, max);
            flash_err(msg);
            continue;
        }
        *val = v;
        return 1;
    }
}

static void menu_arch(void) {
    int sel = ui_menu_hdr("架构", vm.arch, ARCH_ITEMS, 4);
    if (sel < 0) return;
    snprintf(vm.arch, sizeof vm.arch, "%s", ARCH_VALS[sel]);
    if (!strcmp(vm.arch, "aarch64")) {
        snprintf(vm.machine, sizeof vm.machine, "virt");
        snprintf(vm.cpu, sizeof vm.cpu, "max");
        snprintf(vm.gpu, sizeof vm.gpu, "VGA");
    } else if (!strcmp(vm.arch, "ppc")) {
        snprintf(vm.machine, sizeof vm.machine, "mac99");
        snprintf(vm.cpu, sizeof vm.cpu, "g3");
        vm.gpu[0] = '\0';
    } else {
        snprintf(vm.machine, sizeof vm.machine, "pc");
        snprintf(vm.cpu, sizeof vm.cpu, "qemu64");
        snprintf(vm.gpu, sizeof vm.gpu, "std");
    }
}

static void menu_machine(void) {
    if (!strcmp(vm.arch, "aarch64")) pick_value("主板", vm.machine, MACHINE_ARM, MACHINE_ARM_V, 1, vm.machine, sizeof vm.machine);
    else if (!strcmp(vm.arch, "ppc")) pick_value("主板", vm.machine, MACHINE_PPC, MACHINE_PPC_V, 6, vm.machine, sizeof vm.machine);
    else pick_value("主板", vm.machine, MACHINE_X86, MACHINE_X86_V, 3, vm.machine, sizeof vm.machine);
}

static void menu_cpu(void) {
    int n = (int)(sizeof CPU_X86 / sizeof CPU_X86[0]);
    if (!strcmp(vm.arch, "aarch64")) pick_value("CPU 型号", vm.cpu, CPU_ARM, CPU_ARM_V, 20, vm.cpu, sizeof vm.cpu);
    else if (!strcmp(vm.arch, "ppc")) pick_value("CPU 型号", vm.cpu, CPU_PPC, CPU_PPC_V, 7, vm.cpu, sizeof vm.cpu);
    else pick_value("CPU 型号", vm.cpu, CPU_X86, CPU_X86_V, n, vm.cpu, sizeof vm.cpu);
}

static void menu_cores(void) {
    char items[16][48];
    const char *p[16];
    int i, n = 0, sel;
    for (i = 1; i <= host_cores() && i <= 8; i++) {
        snprintf(items[n], sizeof items[i], "%d 核 / 每核 %d 线程（共 %d vCPU）", i, vm.threads, i * vm.threads);
        p[n] = items[n];
        n++;
    }
    sel = ui_menu_hdr("核心数与线程", "上下选；想改线程数按 T", p, n);
    if (sel >= 0) vm.cores = sel + 1;
}

static void menu_threads(void) {
    int t = vm.threads;
    if (ui_num("每核线程数", "线程", 1, 4, 1, &t)) vm.threads = t;
}

static void menu_mem(void) {
    const char *items[] = {"自动（可用内存 - 100M）", "512 MB", "1024 MB", "2048 MB",
        "3072 MB", "4096 MB", "6144 MB", "8192 MB", "12288 MB", "16384 MB", "自定义（手动输入 MB）"};
    int sel = ui_menu_hdr("内存", vm.mem_auto ? "自动" : "固定", items, 11);
    if (sel < 0) return;
    if (sel == 0) { vm.mem_auto = 1; return; }
    vm.mem_auto = 0;
    if (sel == 10) {
        char t[32];
        snprintf(t, sizeof t, "%d", vm.mem > 0 ? vm.mem : 2048);
        while (1) {
            int m;
            if (!ui_input("内存大小", "直接输 MB 数，至少 256", t, t, sizeof t, 0)) return;
            m = atoi(t);
            if (m < 256) { flash_err("内存至少 256 MB"); continue; }
            if (m > host_avail_mb()) {
                char msg[96];
                snprintf(msg, sizeof msg, "超过本机可用内存 %d MB", host_avail_mb());
                flash_err(msg);
                continue;
            }
            vm.mem = m;
            return;
        }
    }
    {
        static const int table[] = {0, 512, 1024, 2048, 3072, 4096, 6144, 8192, 12288, 16384};
        if (sel > 0 && sel < (int)(sizeof table / sizeof table[0])) vm.mem = table[sel];
        else vm.mem = 2048;
    }
}

static void menu_gpu(void) {
    const char *items[] = {"-vga std（默认）", "-device VGA", "-device cirrus-vga", "-device qxl-vga",
        "-device virtio-vga", "-device virtio-gpu", "不设显卡"};
    const char *vals[] = {"std", "VGA", "cirrus-vga", "qxl-vga", "virtio-vga", "virtio-gpu", ""};
    pick_value("显卡", vm.gpu, items, vals, 7, vm.gpu, sizeof vm.gpu);
}

static void menu_audio(void) {
    pick_value("声卡", vm.audio, AUDIO_ITEMS, AUDIO_VALS, 7, vm.audio, sizeof vm.audio);
}

static void menu_nic(void) {
    pick_value("网卡", vm.nic, NIC_ITEMS, NIC_VALS, 19, vm.nic, sizeof vm.nic);
}

/* 把 QEMU 的 c/d/a/n 翻译成用户看得懂的设备名 */
static void boot_value(char *out, int n) {
    const char *p;
    char tmp[256];
    out[0] = '\0';
    if (!vm.boot[0]) { snprintf(out, n, "[默认]"); return; }
    for (p = vm.boot; *p; p++) {
        const char *name = NULL;
        if (*p == 'c') name = vm.hda[0] ? "[HardDisk A]" : (vm.hda2[0] ? "[HardDisk B]" : "[硬盘]");
        else if (*p == 'd') name = vm.cdrom[0] ? "[CDROM A]" : (vm.cdrom2[0] ? "[CDROM B]" : "[光盘]");
        else if (*p == 'a') name = vm.fda[0] ? "[Floppy A]" : (vm.fdb[0] ? "[Floppy B]" : "[软盘]");
        else if (*p == 'n') name = "[网络]";
        if (!name) continue;
        if (out[0]) { snprintf(tmp, sizeof tmp, "%s %s", out, name); snprintf(out, n, "%s", tmp); }
        else snprintf(out, n, "%s", name);
    }
    if (!out[0]) snprintf(out, n, "[默认]");
}

static void menu_boot(void) {
    const char *items[] = {"[默认]", "[HardDisk A]", "[HardDisk B]", "[CDROM A]",
                           "[CDROM B]", "[Floppy A]", "[Floppy B]", "[网络]"};
    const char *vals[] = {"", "cd", "cd", "dc", "dc", "ac", "ac", "nc"};
    char cur[256];
    int sel;
    boot_value(cur, sizeof cur);
    sel = ui_menu_hdr("启动顺序", cur, items, 8);
    if (sel < 0) return;
    snprintf(vm.boot, sizeof vm.boot, "%s", vals[sel]);
}

static void menu_firmware(void) {
    const char *items[] = {"默认 BIOS（bios.bin）", "UEFI / OVMF（x86）",
        "不用固件参数", "选择自定义 BIOS 文件", "选择自定义 OVMF CODE", "选择自定义 OVMF VARS"};
    int sel = ui_menu_hdr("固件", vm.firmware, items, 6);
    if (sel < 0) return;
    if (sel == 0) snprintf(vm.firmware, sizeof vm.firmware, "default");
    else if (sel == 1) snprintf(vm.firmware, sizeof vm.firmware, "uefi");
    else if (sel == 2) snprintf(vm.firmware, sizeof vm.firmware, "none");
    else if (sel == 3) ui_browse("选择 BIOS 文件", "/storage/emulated/0", ".bin", vm.bios_file, sizeof vm.bios_file, 0);
    else if (sel == 4) ui_browse("选择 OVMF CODE", "/storage/emulated/0", ".fd", vm.code_file, sizeof vm.code_file, 0);
    else if (sel == 5) ui_browse("选择 OVMF VARS", "/storage/emulated/0", ".fd", vm.vars_file, sizeof vm.vars_file, 0);
}

static void menu_display(void) {
    const char *items[] = {"VNC（本地 unix socket）", "VNC（TCP，可外部连）", "SPICE",
        "SDL 窗口", "GTK 窗口", "无显示", "VNC 密码", "VNC/IP 与端口", "SPICE 端口"};
    int sel = ui_menu_hdr("显示", vm.display, items, 9);
    if (sel < 0) return;
    switch (sel) {
    case 0: snprintf(vm.display, sizeof vm.display, "vnc"); vm.vnc_ext = 0; break;
    case 1: snprintf(vm.display, sizeof vm.display, "vnc"); vm.vnc_ext = 1; break;
    case 2: snprintf(vm.display, sizeof vm.display, "spice"); break;
    case 3: snprintf(vm.display, sizeof vm.display, "sdl"); break;
    case 4: snprintf(vm.display, sizeof vm.display, "gtk"); break;
    case 5: snprintf(vm.display, sizeof vm.display, "none"); break;
    case 6: {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%s", vm.vnc_pass);
        if (ui_input("VNC 密码", "直接输入，留空表示不要密码", tmp, tmp, sizeof tmp, 0))
            snprintf(vm.vnc_pass, sizeof vm.vnc_pass, "%s", tmp);
        break;
    }
    case 7: {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%s", vm.vnc_addr);
        while (ui_input("VNC 监听地址", "比如 127.0.0.1 或 0.0.0.0", tmp, tmp, sizeof tmp, 0)) {
            char why[128];
            if (valid_addr(tmp, why, sizeof why)) {
                snprintf(vm.vnc_addr, sizeof vm.vnc_addr, "%s", tmp);
                break;
            }
            flash_err(why);
        }
        {
            int p = vm.vnc_port;
            if (ui_num("VNC 端口", "端口", 5900, 5999, 1, &p)) vm.vnc_port = p;
        }
        break;
    }
    case 8: {
        int p = vm.spice_port;
        if (ui_num("SPICE 端口", "端口", 1024, 65535, 100, &p)) vm.spice_port = p;
        break;
    }
    }
}

static void menu_args(void) {
    char t[512];
    snprintf(t, sizeof t, "%s", vm.args);
    if (ui_input("自定义 QEMU 参数", "会原样追加在网卡参数之后，注意别和已有参数冲突", t, t, sizeof t, 0))
        snprintf(vm.args, sizeof vm.args, "%s", t);
}

/* 显示界面真正会执行的那条命令：串口走 socket，控制台才接得上。
   前台/后台只是进程托管方式不同（双 fork 还是前台挂着），argv 一模一样，
   所以这里只出一份，别再拿 -serial stdio/file 糊弄人。 */
static void show_command(void) {
    char *argv[MAXARGV];
    char pool[MAXARGV][PATHBUF];
    char *lines[MAXARGV];
    int argc, i;
    argc = vm_build_argv(&vm, argv, MAXARGV, pool, MAXARGV, 1, "auto");
    for (i = 0; i < argc; i++) lines[i] = argv[i];
    ui_pager("启动命令（界面实际执行）", lines, argc);
}

static void menu_img(void) {
    const char *items[] = {"新建 qcow2 虚拟磁盘", "检查磁盘错误", "查看磁盘信息",
        "离线快照列表", "建立离线快照", "恢复离线快照"};
    char path[PATHBUF];
    int sel = ui_menu_hdr("虚拟磁盘工具[Qemu-img]", vm.hda[0] ? vm.hda : "[未选择]", items, 6);
    if (sel < 0) return;
    snprintf(path, sizeof path, "%s", vm.hda);
    if (sel == 0) {
        char name[64] = "disk0.qcow2";
        char out[2048], base[PATHBUF], msg[PATHBUF + 64];
        int gb = 8;
        runtime_dir_for(vm.name, base, sizeof base);   /* 和磁盘、日志放一处 */
        snprintf(msg, sizeof msg, "文件名（落在 %s 下）", base);
        if (!ui_input("新建磁盘", msg, name, name, sizeof name, 0)) return;
        if (!ui_num("磁盘大小 GB", "大小", 1, 512, 1, &gb)) return;
        snprintf(path, sizeof path, "%s/%s", base, name);
        {
            char *argv2[6] = {"qemu-img", "create", "-f", "qcow2", path, NULL};
            char size[16];
            snprintf(size, sizeof size, "%dG", gb);
            argv2[5] = size;
            run_cmd(argv2, out, sizeof out);
        }
        snprintf(vm.hda, sizeof vm.hda, "%s", path);
        ui_msg("已创建：%s\n已设为当前主磁盘", path);
        return;
    }
    if (!path[0]) { ui_msg("先在磁盘菜单里选一块盘"); return; }
    if (sel == 1) { char *a[] = {"qemu-img", "check", path, NULL}; char out[4096]; run_cmd(a, out, sizeof out); ui_msg("%s", out); }
    else if (sel == 2) { char *a[] = {"qemu-img", "info", path, NULL}; char out[4096]; run_cmd(a, out, sizeof out); ui_msg("%s", out); }
    else if (sel == 3) { char *a[] = {"qemu-img", "snapshot", "-l", path, NULL}; char out[4096]; run_cmd(a, out, sizeof out); ui_msg("%s", out); }
    else if (sel == 4) {
        char sn[64] = "snap1";
        if (!ui_input("建立快照", "快照名", sn, sn, sizeof sn, 0)) return;
        { char *a[] = {"qemu-img", "snapshot", "-c", sn, path, NULL}; char out[4096]; run_cmd(a, out, sizeof out); ui_msg("%s", out); }
    } else {
        char sn[64] = "snap1";
        if (!ui_input("恢复快照", "快照名", sn, sn, sizeof sn, 0)) return;
        { char *a[] = {"qemu-img", "snapshot", "-a", sn, path, NULL}; char out[4096]; run_cmd(a, out, sizeof out); ui_msg("%s", out); }
    }
}

/* ------------------------------------------------------------------
 * 界面：主页列表 / 单机控制页 / 分组配置页
 * ------------------------------------------------------------------ */
static const char *which_path(const char *name) {
    static char out[PATHBUF];
    const char *p = getenv("PATH");
    char *dup, *tok, *save = NULL;
    if (!p || !*p) return "";
    dup = strdup(p);
    if (!dup) return "";
    for (tok = strtok_r(dup, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        snprintf(out, sizeof out, "%s/%s", tok, name);
        if (access(out, X_OK) == 0) { free(dup); return out; }
    }
    free(dup);
    out[0] = '\0';
    return out;
}

static void vnc_text(const VM *v, char *out, int n) {
    if (!strcmp(v->display, "vnc")) {
        if (v->vnc_ext) snprintf(out, n, "%s:%d", v->vnc_addr, v->vnc_port);
        else {
            char sp[PATHBUF];                       /* 跟着磁盘目录走，别写死 ~/.vvm */
            vnc_path_for(v->name, sp, sizeof sp);
            snprintf(out, n, "unix:%s", sp);
        }
    } else if (!strcmp(v->display, "spice")) {
        snprintf(out, n, "spice://:%d", v->spice_port);
    } else {
        snprintf(out, n, "%s", v->display);
    }
}

/* 文件多大，按 MB 说；读不到就返回 -1 */
static long file_mb(const char *path) {
    struct stat st;
    if (!path || !path[0]) return -1;
    if (stat(path, &st) != 0) return -1;
    return (long)((st.st_size + 1048575) / 1048576);
}

/* 环境检查：一项一项列出来，能滚着看（原来是塞进一条状态栏，长一点就被截掉） */
static void short_path(const char *in, char *out, int n, int maxw);

static void menu_env(void) {
    static char *lines[28];
    char a[PATHBUF], sp[PATHBUF], out[2048];
    int n = 0, bad = 0, i;

    for (i = 0; i < 28; i++) lines[i] = NULL;
    {
        char *l = malloc(PATHBUF + 128);
        if (!l) return;
        snprintf(l, PATHBUF + 128, "配置目录　%s", CONF_DIR);
        lines[n++] = l;
    }
    {
        char *l = malloc(160);
        snprintf(l, 160, "系统资源　可用内存 %d MB　处理器 %d 核", host_avail_mb(), host_cores());
        lines[n++] = l;
    }
    {
        char *l = malloc(160);
        snprintf(l, 160, "虚拟机监控程序");
        lines[n++] = l;
    }
    /* 可执行文件：不光看有没有，还看能不能跑起来 */
    {
        const char *bins[] = { "qemu-system-x86_64", "qemu-system-aarch64", "qemu-img" };
        const char *labels[] = { "x86_64 模拟器", "ARM64 模拟器", "磁盘工具" };
        int k;
        for (k = 0; k < 3 && n < 22; k++) {
            char *l = malloc(PATHBUF + 200);
            const char *wp = which_path(bins[k]);
            if (!wp[0]) {
                snprintf(l, PATHBUF + 200, "　%s　未安装（pkg install %s）", labels[k], k == 1 ? "qemu-system-aarch64-headless" : bins[k]);
                bad++;
            } else {
                char *av[3] = { (char *)bins[k], "--version", NULL };
                out[0] = '\0';
                snprintf(l, PATHBUF + 200, "　%s　%s", labels[k], wp);
                if (run_cmd(av, out, sizeof out) != 0) {
                    snprintf(l + strlen(l), PATHBUF + 200 - strlen(l), "　（无法运行，安装包可能已损坏）");
                    bad++;
                }
            }
            lines[n++] = l;
        }
    }
    {
        char *l = malloc(160);
        snprintf(l, 160, "固件");
        lines[n++] = l;
    }
    /* ARM UEFI 代码：按原版脚本的 60–70MB 判完整；x86 OVMF 本身就是几 MB，不按这个标准 */
    {
        long mb;
        char *l;
        fw_path(a, sizeof a, "QEMU_EFI.img");
        mb = file_mb(a);
        l = malloc(PATHBUF + 200);
        if (mb < 0) {
            snprintf(l, PATHBUF + 200, "　ARM UEFI 代码　未安装（pkg install qemu-system-aarch64-headless）");
            bad++;
        } else if (!fw_arm_size_ok(a)) {
            short_path(a, sp, sizeof sp, COLS - 6);
            snprintf(l, PATHBUF + 200, "　ARM UEFI 代码　大小异常（%ld MB，完整应为 60–70 MB）　%s", mb, sp);
            bad++;
        } else {
            short_path(a, sp, sizeof sp, COLS - 6);
            snprintf(l, PATHBUF + 200, "　ARM UEFI 代码　完整（%ld MB）　%s", mb, sp);
        }
        lines[n++] = l;

        fw_path(a, sizeof a, "RELEASEX64_OVMF.fd");
        mb = file_mb(a);
        l = malloc(PATHBUF + 200);
        if (mb < 0)
            snprintf(l, PATHBUF + 200, "　x86 UEFI 代码　未安装（只在选 UEFI 时使用）");
        else {
            short_path(a, sp, sizeof sp, COLS - 6);
            snprintf(l, PATHBUF + 200, "　x86 UEFI 代码　%ld MB　%s", mb, sp);
        }
        lines[n++] = l;

        fw_path(a, sizeof a, "bios.bin");
        l = malloc(PATHBUF + 200);
        if (file_exists(a)) {
            short_path(a, sp, sizeof sp, COLS - 6);
            snprintf(l, PATHBUF + 200, "　x86 BIOS　自定义　%s", sp);
        } else
            snprintf(l, PATHBUF + 200, "　x86 BIOS　使用 QEMU 内置 SeaBIOS");
        lines[n++] = l;
    }
    /* 软件包更新：只报告，不替你升级 */
    {
        const char *wpkg = which_path("pkg");
        if (wpkg[0] && n < 22) {
            char *av[3] = { (char *)wpkg, "list-upgradable", NULL };
            char *l = malloc(160);
            int cnt = 0, k2;
            out[0] = '\0';
            run_cmd(av, out, sizeof out);
            for (k2 = 0; out[k2]; k2++) if (out[k2] == '\n') cnt++;
            if (cnt > 0) snprintf(l, 160, "软件包　%d 个可升级（pkg upgrade）", cnt);
            else snprintf(l, 160, "软件包　已是最新");
            lines[n++] = l;
        }
    }
    {
        char *l = malloc(200);
        if (bad) snprintf(l, 200, "检查结果　%d 项需要处理", bad);
        else snprintf(l, 200, "检查结果　一切就绪");
        lines[n++] = l;
    }
    if (bad) {
        char *l = malloc(260);
        snprintf(l, 260, "处理建议　pkg install qemu-utils qemu-system-x86_64-headless qemu-system-aarch64-headless");
        lines[n++] = l;
        l = malloc(260);
        snprintf(l, 260, "　　　　　固件缺失时，把文件放到 %s/firmware", CONF_DIR);
        lines[n++] = l;
    }
    ui_pager("Qemu 环境检查", lines, n);
    for (i = 0; i < n; i++) free(lines[i]);
}

/* 显示宽度（按终端列数，不是字节） */
static int dwidth(const char *s) {
    wchar_t b[1024];
    size_t n, i;
    int w = 0;
    if (!s || !*s) return 0;
    n = mbstowcs(b, s, sizeof b / sizeof b[0] - 1);
    if (n == (size_t)-1) return (int)strlen(s);
    for (i = 0; i < n; i++) {
        int cw = wcwidth(b[i]);
        w += (cw < 0) ? 1 : cw;
    }
    return w;
}

/* 路径缩略：$HOME 换成 ~，太长就在中间打省略号，保留头和尾 */
static int len0(const char *s) { return (int)strlen(s); }

static void short_path(const char *in, char *out, int n, int maxw) {
    char tmp[PATHBUF * 2];
    int w, keep, head, tail, len;
    if (strncmp(in, HOME_DIR, strlen(HOME_DIR)) == 0)
        snprintf(tmp, sizeof tmp, "~%s", in + strlen(HOME_DIR));
    else
        snprintf(tmp, sizeof tmp, "%s", in);
    w = dwidth(tmp);
    if (w <= maxw) { snprintf(out, n, "%s", tmp); return; }
    if (maxw < 8) { snprintf(out, n, "%s", tmp + (len0(tmp) > maxw ? len0(tmp) - maxw : 0)); return; }
    keep = maxw - 3;
    head = keep / 2;
    tail = keep - head;
    len = (int)strlen(tmp);
    if (len <= head + tail) { snprintf(out, n, "%s", tmp); return; }
    snprintf(out, n, "%.*s...%s", head, tmp, tmp + len - tail);
}

static void yellow_hint(WINDOW *w, int y, const char *text) {
    wattrset(w, COLOR_PAIR(2) | A_BOLD);
    mvwhline(w, y, 0, ' ', getmaxx(w));
    put_u8(w, y, 1, text, getmaxx(w) - 2, COLOR_PAIR(2) | A_BOLD);
    wattrset(w, A_NORMAL);
}

/* 右对齐输出 */
static void put_right(WINDOW *w, int y, int right, const char *s, int attr) {
    int wd = dwidth(s);
    int x = right - wd;
    if (x < 0) x = 0;
    put_u8(w, y, x, s, right - x + 1, attr);
}




typedef struct {
    char name[64];
    char arch[16];
    char machine[32];
    char cpu[24];
    int  mem;
    int  cores;
    int  running;
    int  st;                 /* 0 未启动  1 暂停/已停止  2 运行中 */
    char display[16];
    int  vnc_port;
    int  vnc_ext;
} Card;

static int scan_cards(Card *cards, int maxn) {
    char names[64][64];
    int n = list_configs(names, 64), i, c = 0;
    for (i = 0; i < n && c < maxn; i++) {
        vm_load(names[i]);
        snprintf(cards[c].name, sizeof cards[c].name, "%s", vm.name);
        snprintf(cards[c].arch, sizeof cards[c].arch, "%s", vm.arch);
        snprintf(cards[c].machine, sizeof cards[c].machine, "%s", vm.machine);
        snprintf(cards[c].cpu, sizeof cards[c].cpu, "%s", vm.cpu);
        snprintf(cards[c].display, sizeof cards[c].display, "%s", vm.display);
        cards[c].mem = vm.mem_auto ? host_avail_mb() - 100 : vm.mem;
        cards[c].cores = vm.cores * vm.threads;
        cards[c].vnc_port = vm.vnc_port;
        cards[c].vnc_ext = vm.vnc_ext;
        cards[c].st = vm_state_of(names[i], read_pid(names[i]));
        cards[c].running = cards[c].st >= 2;
        c++;
    }
    return c;
}

static void draw_entry(WINDOW *w, int y, int x, int wid, const Card *c, int sel) {
    char info[160], right[48];
    int budget;
    const char *cpu = c->cpu[0] ? c->cpu : "max";
    const char *board = c->machine[0] ? c->machine : "pc";

    if (sel) {
        wattron(w, COLOR_PAIR(6));
        mvwhline(w, y, 0, ' ', getmaxx(w));
        mvwhline(w, y + 1, 0, ' ', getmaxx(w));
        wattroff(w, COLOR_PAIR(6));
    }
    put_u8(w, y, x + 1, c->name, wid - 22, sel ? (COLOR_PAIR(6) | A_BOLD) : 0);
    put_right(w, y, x + wid, c->arch, sel ? COLOR_PAIR(6) : g_grey);

    /* 状态文字先算，第二行左边剩多少位置才知道；VNC 单独画，不跟着状态变色 */
    {
        char vncstr[48] = "", ststr[24];
        unsigned st_attr;
        if (c->st == 3) {
            snprintf(ststr, sizeof ststr, "[运行中]");
            st_attr = COLOR_PAIR(4) | A_BOLD;
            if (wid >= 66) snprintf(vncstr, sizeof vncstr, c->vnc_ext ? "VNC : %d" : "VNC : unix", c->vnc_port);
        } else if (c->st == 2) {
            snprintf(ststr, sizeof ststr, "[已暂停]");
            st_attr = COLOR_PAIR(2) | A_BOLD;
            if (wid >= 66) snprintf(vncstr, sizeof vncstr, c->vnc_ext ? "VNC : %d" : "VNC : unix", c->vnc_port);
        } else if (c->st == 1) {
            snprintf(ststr, sizeof ststr, "[已挂起]");
            st_attr = COLOR_PAIR(1) | A_BOLD;
        } else {
            snprintf(ststr, sizeof ststr, "[未启动]");
            st_attr = g_grey;
        }
        snprintf(right, sizeof right, "%s%s%s", vncstr, vncstr[0] ? "   " : "", ststr);
        budget = wid - (int)dwidth(right) - 2;
        if (budget < 12) budget = 0;   /* 窄到放不下就别硬挤，免得和右边状态粘在一起 */

        /* 先把第二行左边信息画出来（下面还会用到 budget） */
        snprintf(info, sizeof info, "CPU：%s / 主板：%s / 核心：%d核 / RAM：%d MB",
                 cpu, board, c->cores, c->mem);
        if ((int)dwidth(info) > budget)
            snprintf(info, sizeof info, "%s / %s / %d核 / %dMB", cpu, board, c->cores, c->mem);
        if ((int)dwidth(info) > budget)
            snprintf(info, sizeof info, "%s / %d核 / %dMB", cpu, c->cores, c->mem);
        if (budget > 0) put_u8(w, y + 1, x + 1, info, budget, sel ? COLOR_PAIR(6) : g_grey);

        if (vncstr[0])
            put_right(w, y + 1, x + wid - (int)dwidth(ststr) - 3, vncstr, sel ? COLOR_PAIR(6) : 0);
        put_right(w, y + 1, x + wid, ststr, sel ? (COLOR_PAIR(6) | A_BOLD) : st_attr);
        return;
    }

}

static void rule(WINDOW *w, int y);

static const char *FW_FILES[] = {
    "QEMU_EFI.img", "QEMU_VARS.img", "RELEASEX64_OVMF.fd",
    "RELEASEX64_OVMF_VARS.fd"
};

/* 只统计当前架构真正需要的固件，别拿 ARM 的文件吓 x86 */
static int fw_missing(void) {
    char p[PATHBUF];
    if (!strcmp(vm.arch, "aarch64")) {
        fw_path(p, sizeof p, "QEMU_EFI.img");
        return file_exists(p) ? 0 : 1;
    }
    if (!strcmp(vm.arch, "x86_64") && !strcmp(vm.firmware, "uefi")) {
        fw_path(p, sizeof p, "RELEASEX64_OVMF.fd");
        if (!file_exists(p)) return 1;
    }
    return 0;
}

/* 固件页：看状态和目录，不含任何打包文件的拆解 */
static void firmware_screen(void) {
    int sel = 0;
    while (1) {
        WINDOW *w = newwin(LINES, COLS, 0, 0);
        g_top = w;
        char p[PATHBUF], sub[96];
        int miss;
        if (!w) return;
        keypad(w, TRUE);
        werase(w);

        put_u8(w, 0, 1, "固件", 0, A_BOLD);
        miss = fw_missing();
        snprintf(sub, sizeof sub, miss ? "缺 %d 个" : "已就绪", miss);
        put_right(w, 0, COLS - 2, sub, g_grey);
        rule(w, 1);

        {
            /* 三段：x86 BIOS（默认）、ARM UEFI、x86 UEFI */
            static const char *sec[3] = { "x86_64 BIOS（默认）", "ARM（aarch64）UEFI", "x86_64 UEFI" };
            static const char *files[3][2] = {
                { "bios.bin", NULL },
                { "QEMU_EFI.img", "QEMU_VARS.img" },
                { "RELEASEX64_OVMF.fd", "RELEASEX64_OVMF_VARS.fd" }
            };
            int y = 2, si, idx = 0;
            for (si = 0; si < 3; si++) {
                int k;
                if (y >= LINES - 3) break;
                put_u8(w, y, 1, sec[si], COLS - 2, A_BOLD | g_grey);
                y++;
                for (k = 0; k < 2 && files[si][k]; k++, idx++) {
                    int ok, need = 0, vars, bios;
                    char st[32];
                    if (y >= LINES - 3) break;
                    bios = (idx == 0);
                    vars = (idx == 2 || idx == 4);
                    fw_path(p, sizeof p, files[si][k]);
                    ok = file_exists(p);
                    need = (idx == 1) ? !strcmp(vm.arch, "aarch64")
                           : (idx == 3) ? (!strcmp(vm.arch, "x86_64") && !strcmp(vm.firmware, "uefi"))
                           : 0;
                    if (bios) snprintf(st, sizeof st, ok ? "已就绪" : "内置");
                    else if (vars) snprintf(st, sizeof st, "免配置");
                    else if (need) {
                        if (!ok) snprintf(st, sizeof st, "未安装");
                        else if (idx == 1 && !fw_arm_size_ok(p)) snprintf(st, sizeof st, "损坏");
                        else snprintf(st, sizeof st, "已就绪");
                    }
                    else snprintf(st, sizeof st, "无需");
                    if (idx == sel) {
                        wattron(w, COLOR_PAIR(6));
                        mvwhline(w, y, 0, ' ', getmaxx(w));
                        wattroff(w, COLOR_PAIR(6));
                    }
                    put_u8(w, y, 2, files[si][k], COLS - 14, idx == sel ? (COLOR_PAIR(6) | A_BOLD) : 0);
                    put_right(w, y, COLS - 2, st, idx == sel ? COLOR_PAIR(6)
                                                             : (need && (!ok || !strcmp(st, "损坏")) ? (COLOR_PAIR(3) | A_BOLD) : g_grey));
                    y++;
                    if (ok && y < LINES - 3) {
                        char sp[PATHBUF * 2];
                        short_path(p, sp, sizeof sp, COLS - 4);
                        put_u8(w, y, 4, sp, COLS - 4, idx == sel ? COLOR_PAIR(6) : g_grey);
                        y++;
                    }
                }
                y++;
            }
        }

        {
            char d[PATHBUF];
            snprintf(d, sizeof d, "%s/firmware", CONF_DIR);
            put_u8(w, LINES - 3, 1, d, COLS - 2, g_grey);
            status_or_hint_at(w, LINES - 2,
                              miss ? "缺固件：pkg install qemu-system-aarch64 qemu-system-aarch64-headless，装完自动就用，不用拷"
                                   : (strcmp(vm.arch, "aarch64") == 0 ? "直接用 Termux 里的固件；变量盘每台 VM 自动生成  Enter 看说明  Esc 返回"
                                                                      : "x86 默认 BIOS 不需要 UEFI；变量盘每台 VM 自动生成  Enter 看说明  Esc 返回"));
        }
        wrefresh(w);
        {
            int ch = wgetch(w);
            if (ch == 27) { close_win(w); return; }        /* 只留 Esc 退出，q 不再退出 */
            if (ch == KEY_UP && sel > 0) sel--;
            else if (ch == KEY_DOWN && sel < 4) sel++;
            else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                char d[PATHBUF];
                close_win(w);
                snprintf(d, sizeof d, "%s/firmware", CONF_DIR);
                ui_msg("Termux 里装的固件会自动使用，不需要拷。\n只有想换自定义固件时，才把文件放到：\n%s\n（这里的优先于系统目录，重名覆盖）", d);
                continue;
            }
            close_win(w);
        }
        touchwin(stdscr);
        refresh();
    }
}

static void draw_new_entry(WINDOW *w, int y, int x, int wid, int sel) {
    if (sel) {
        wattron(w, COLOR_PAIR(6));
        mvwhline(w, y, 0, ' ', getmaxx(w));
        mvwhline(w, y + 1, 0, ' ', getmaxx(w));   /* 和虚拟机条目一样高 */
        wattroff(w, COLOR_PAIR(6));
    }
    put_u8(w, y, x + 1, "＋ 新建虚拟机", wid, sel ? (COLOR_PAIR(6) | A_BOLD) : g_grey);
}

static void rule(WINDOW *w, int y) {
    unsigned a = g_grey;
    wattron(w, a);
    mvwhline(w, y, 1, ACS_HLINE, getmaxx(w) - 2);
    wattroff(w, a);
}

/* 名字可以带空格和中文（老配置就有 "alpine x86" 这种），
   只要不越界、不带路径分隔符、不以 . 开头就行 */
static int name_ok(const char *s) {
    int i;
    if (!s || !s[0]) return 1;                 /* 空名走时间戳 */
    if (strlen(s) > 48) return 0;
    if (s[0] == '.') return 0;
    if (strstr(s, "..")) return 0;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c == 127) return 0;
        if (c == '/' || c == '\\') return 0;
    }
    return 1;
}

static int prompt_make_disk(const char *name);

static int new_vm_named(const char *name) {
    char t[64];
    if (!name || !name[0]) {
        time_t now = time(NULL);
        struct tm *tmv = localtime(&now);
        strftime(t, sizeof t, "vm-%Y%m%d%H%M%S", tmv);
    } else {
        if (!name_ok(name)) {
            ui_msg("名字不能为空、不能带 / 或 ..、不能以 . 开头，最长 48 字节");
            return 0;
        }
        snprintf(t, sizeof t, "%s", name);
    }
    if (vm_load(t) == 0) { ui_msg("这个名字已经存在"); return 0; }
    vm_defaults(&vm);
    snprintf(vm.name, sizeof vm.name, "%s", t);
    if (vm_save(&vm) != 0)
        ui_msg("配置写不进去：%s 不可写（检查一下目录权限）", CONF_DIR);
    if (prompt_make_disk(t)) return 1;    /* 没盘装不了系统，直接就地问一句 */
    return 1;
}

/* 给这台 VM 建一块空 qcow2：~/start/<名字>/<名字>.qcow2
   磁盘在这一层，运行文件（pid/日志/socket）也就在这一层，不再套娃。 */
static int create_disk_for_vm(const char *name, int gb, char *outpath, int outn) {
    char dir[PATHBUF], out[2048], size[16];
    char *argv[8];
    struct stat st;

    snprintf(dir, sizeof dir, "%s/%s", CONF_DIR, name);
    ensure_dir(dir);
    snprintf(outpath, outn, "%s/%s.qcow2", dir, name);
    if (stat(outpath, &st) == 0) {
        if (st.st_size > 0) return 0;                  /* 已经有了，别覆盖人家的盘 */
        unlink(outpath);
    }
    snprintf(size, sizeof size, "%dG", gb);
    argv[0] = "qemu-img";
    argv[1] = "create";
    argv[2] = "-f";
    argv[3] = "qcow2";
    argv[4] = outpath;
    argv[5] = size;
    argv[6] = NULL;
    if (run_cmd(argv, out, sizeof out) != 0) {
        ui_msg("建磁盘失败：\n%s\n（先 pkg install qemu-img）", out[0] ? out : "qemu-img 没给出原因");
        return -1;
    }
    return 1;
}

/* 启动前发现这块 VM 连盘都没有：问一句就地建一块，装系统要用 */
static int prompt_make_disk(const char *name) {
    char path[PATHBUF], nm[64];
    int gb = 8;
    snprintf(nm, sizeof nm, "%s", name);          /* 传进来的可能是指向 vm.name 的指针 */
    if (!ui_confirm("还没有磁盘，先建一块 %s 用的空白盘？", nm)) return 0;
    if (!ui_num("磁盘大小 GB", "大小", 1, 512, 1, &gb)) return 0;
    if (create_disk_for_vm(nm, gb, path, sizeof path) < 0) return 0;
    vm_load(nm);
    snprintf(vm.hda, sizeof vm.hda, "%s", path);
    vm_save(&vm);
    ui_msg("磁盘已建好：%s\n再挂个 ISO 到 CDRom 就能开始装系统", path);
    return 1;
}

static void new_vm(void) {
    char t[64];
    t[0] = '\0';
    if (!ui_input("新建虚拟机", "命名虚拟机启动配置文件[字母 数字 下划线 横线]", "", t, sizeof t, 0)) return;
    new_vm_named(t);
}

/* 从 .sh 里把 VM_HDA 抠出来，用来问"磁盘要不要一起删" */
static void hda_of_conf(const char *name, char *out, int n) {
    char p[PATHBUF], line[PATHBUF + 48];
    FILE *f;
    out[0] = '\0';
    conf_path(name, p, sizeof p);
    f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VM_HDA=", 7) != 0) continue;
        {
            char *v = line + 7, *q = v;
            size_t len;
            while (*q && *q != '\n') q++;
            *q = '\0';
            if (v[0] == '\'') {
                v++;
                len = strlen(v);
                if (len && v[len - 1] == '\'') v[len - 1] = '\0';
                sh_unquote(v);
            }
            snprintf(out, n, "%s", v);
        }
        break;
    }
    fclose(f);
}

/* 删配置：两步问，都答在底行，不弹新窗口；第二步才碰磁盘 */
static void vm_delete(const char *name) {
    char nm[64], path[PATHBUF], hda[PATHBUF], msg[PATHBUF + 96];
    long mb;
    int disk_gone = 0;

    snprintf(nm, sizeof nm, "%s", name);         /* 下面 vm_load 会清参数，先存一份 */
    hda_of_conf(nm, hda, sizeof hda);
    if (!ui_confirm("删除 %s 的配置？", nm)) return;
    {
        int rp = read_pid(nm);
        if (rp > 0 && pid_alive(rp)) {
            char rq[2048], re[256];
            if (!ui_confirm("%s 还在跑（pid %d），先把它停掉再删？", nm, rp))
                return;
            if (qmp_exec(nm, "{\"execute\":\"quit\"}", rq, sizeof rq, re, sizeof re) != 0) {
                kill(rp, SIGKILL);
            }
            { int i; for (i = 0; i < 30 && pid_alive(rp); i++) usleep(100000); }
        }
    }

    conf_path(nm, path, sizeof path);
    if (unlink(path) != 0) { status_set(0, "删除失败：没有这个配置文件"); return; }
    conf_path_legacy(nm, path, sizeof path);
    unlink(path);

    if (hda[0] && file_exists(hda)) {
        char tail[48] = "";
        mb = file_mb(hda);
        if (mb >= 1024) snprintf(tail, sizeof tail, "（%.1fG）", mb / 1024.0);
        else if (mb > 0) snprintf(tail, sizeof tail, "（%ldM）", mb);
        if (ui_confirm("磁盘 %s%s 也一起删掉？删了就找不回来了", base_name(hda), tail)) {
            if (unlink(hda) == 0) disk_gone = 1;
        }
    }
    if (disk_gone) {
        /* 盘没了，盘旁边那个同名文件夹里的 pid / 日志 / 挂起标记也没意义了 */
        char dir[PATHBUF], f[PATHBUF];
        run_dir_of_disk(hda, nm, dir, sizeof dir);
        if (dir[0] && strcmp(dir, CONF_DIR) != 0) {
            const char *sfx[] = { "%s/%s.log", "%s/%s.pid", "%s/%s.qemu.log",
                                  "%s/%s.ppm", "%s/%s.suspend", "%s/%s.paused",
                                  "%s/vvm-%s.sock", "%s/qmp-%s.sock" };
            int k;
            for (k = 0; k < 8; k++) {
                snprintf(f, sizeof f, sfx[k], dir, nm);
                unlink(f);
            }
            rmdir(dir);
        }
    }
    if (disk_gone) snprintf(msg, sizeof msg, "已删除 %s，磁盘也一起删了", nm);
    else snprintf(msg, sizeof msg, "已删除 %s（磁盘还在）", nm);
    status_set(0, msg);
}

/* 日志看最后 400 行：开机报错、panic 都长在尾巴上，从头往后翻没意义 */
static void show_log(void) {
    char path[PATHBUF], *buf;
    static char *lines[400];
    FILE *f;
    long sz;
    int n = 0, i;
    size_t len = 0, k;
    { char d[PATHBUF]; runtime_dir_for(vm.name, d, sizeof d); snprintf(path, sizeof path, "%s/%s.log", d, vm.name); }
    f = fopen(path, "r");
    if (!f) { ui_msg("还没有日志（先启动一次）"); return; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz <= 0) { fclose(f); ui_msg("日志是空的"); return; }
    if (sz > 512 * 1024) sz = 512 * 1024;          /* 最多读尾部 512KB */
    fseek(f, -sz, SEEK_END);
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); ui_msg("内存不够，读不了日志"); return; }
    len = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[len] = '\0';
    /* 从后往前数够 400 行，再从那里开始逐行切 */
    {
        size_t start = 0, cnt = 0, pos = len;
        while (pos > 0) {
            pos--;
            if (buf[pos] == '\n' && pos + 1 < len) {
                if (++cnt >= 400) { start = pos + 1; break; }
            }
        }
        k = start;
    }
    while (k < len && n < 400) {
        char *nl = strchr(buf + k, '\n');
        size_t l;
        char *s2;
        if (nl) *nl = '\0';
        while (k < len && (buf[k] == '\r')) k++;
        l = strlen(buf + k);
        if (l) {
            while (l && (buf[k + l - 1] == '\r' || buf[k + l - 1] == ' ')) buf[k + l - 1] = '\0';
            if (buf[k]) { s2 = strdup(buf + k); if (s2) lines[n++] = s2; }
        }
        if (!nl) break;
        k = (size_t)(nl - buf) + 1;
    }
    free(buf);
    if (n == 0) ui_msg("日志是空的");
    else {
        char title[PATHBUF + 32];
        snprintf(title, sizeof title, "%s（尾部 %d 行）", path, n);
        ui_pager(title, lines, n);
    }
    for (i = 0; i < n; i++) free(lines[i]);
}

static int wait_pid_gone(pid_t pid, int seconds) {
    int i;
    for (i = 0; i < seconds * 10; i++) {
        if (!pid_alive(pid)) return 1;
        {
            struct timespec ts = { 0, 100000000L };
            nanosleep(&ts, NULL);
        }
    }
    return pid_alive(pid) ? 0 : 1;
}

static void vm_forget_pid(const char *name) {
    { char pp[PATHBUF]; paused_path_for(name, pp, sizeof pp); unlink(pp); }
    char pidp[PATHBUF];
    pid_path_for(name, pidp, sizeof pidp);
    unlink(pidp);
}

/* 正常关机：ACPI 只是“按一下电源键”，客机没 acpid 就没人理 */
static void vm_poweroff(void) {
    char reply[2048], err[256];
    int pid = read_pid(vm.name);
    if (!vm_running(vm.name)) { ui_msg("未启动"); return; }

    if (qmp_exec(vm.name, "{\"execute\":\"query-status\"}", reply, sizeof reply, err, sizeof err) == 0 &&
        strstr(reply, "\"paused\"")) {
        if (qmp_exec(vm.name, "{\"execute\":\"cont\"}", reply, sizeof reply, err, sizeof err) == 0)
            ui_msg("这台刚才处于暂停，已恢复运行再发关机");
    }

    if (qmp_exec(vm.name, "{\"execute\":\"system_powerdown\"}", reply, sizeof reply, err, sizeof err) != 0) {
        ui_msg("%s", err);
        return;
    }
    status_set(0, "已发 ACPI 关机，等客机自己退出…");
    if (wait_pid_gone(pid, 12)) {
        vm_forget_pid(vm.name);
        ui_msg("已关机");
        return;
    }
    if (ui_confirm("ACPI 关机 12 秒无响应（客机可能没有 acpid）。强制停止？")) {
        qmp_exec(vm.name, "{\"execute\":\"quit\"}", reply, sizeof reply, err, sizeof err);
        if (!wait_pid_gone(pid, 6)) kill(pid, SIGKILL);
        if (wait_pid_gone(pid, 3)) {
            vm_forget_pid(vm.name);
            ui_msg("已强制停止");
        } else ui_msg("进程 %d 停不掉，手动 kill -9 %d", pid, pid);
    } else {
        ui_msg("关机请求已发出，客机没退；要停就用「强制停止」");
    }
}

/* 强制停止：先 QMP quit，不行 SIGTERM，再不行 SIGKILL */
static void vm_force_stop(void) {
    char reply[2048], err[256], sp[PATHBUF];
    int pid = read_pid(vm.name), i;
    if (!vm_running(vm.name)) { vm_forget_pid(vm.name); ui_msg("虚拟机未启动"); return; }
    if (qmp_exec(vm.name, "{\"execute\":\"quit\"}", reply, sizeof reply, err, sizeof err) != 0 && pid > 0)
        kill(pid, SIGTERM);                        /* 连不上 QMP 才动刀 */
    if (pid > 0 && !wait_pid_gone(pid, 6)) kill(pid, SIGKILL);
    for (i = 0; pid <= 0 && i < 60 && vm_running(vm.name); i++) {   /* pid 文件丢了就盯 socket */
        struct timespec ts = { 0, 100000000L };
        nanosleep(&ts, NULL);
    }
    if (!vm_running(vm.name)) {
        vm_forget_pid(vm.name);
        ui_msg("已停止");
    } else {
        qmp_path_for(vm.name, sp, sizeof sp);      /* 停不掉至少把假标记清掉，别一直锁着 */
        unlink(sp);
        vm_forget_pid(vm.name);
        ui_msg("没确认停掉，已清掉 pid 和 socket 标记");
    }
}

static void do_control(int which) {
    char reply[2048], err[256], json[512];
    switch (which) {
    case 0: snprintf(json, sizeof json, "{\"execute\":\"query-status\"}"); break;
    case 1: snprintf(json, sizeof json, "{\"execute\":\"stop\"}"); break;
    case 2: snprintf(json, sizeof json, "{\"execute\":\"cont\"}"); break;
    case 3: snprintf(json, sizeof json, "{\"execute\":\"system_powerdown\"}"); break;
    case 4: snprintf(json, sizeof json, "{\"execute\":\"quit\"}"); break;
    case 5: snprintf(json, sizeof json, "{\"execute\":\"system_reset\"}"); break;
    case 6: snprintf(json, sizeof json,
             "{\"execute\":\"send-key\",\"arguments\":{\"keys\":["
             "{\"type\":\"qcode\",\"data\":\"ctrl\"},"
             "{\"type\":\"qcode\",\"data\":\"alt\"},"
             "{\"type\":\"qcode\",\"data\":\"delete\"}]}}"); break;
    default: return;
    }
    if (qmp_exec(vm.name, json, reply, sizeof reply, err, sizeof err) == 0) {
        char pp[PATHBUF];
        paused_path_for(vm.name, pp, sizeof pp);
        if (which == 1) { FILE *f = fopen(pp, "w"); if (f) fclose(f); }
        else if (which == 2) unlink(pp);
        if (which == 0) ui_msg("%s", reply);
        else ui_msg("已发送");
    } else ui_msg("%s", err);
}

/* 真挂起：把内存写进 qcow2，然后让 qemu 退出 */
static int disk_is_qcow2(const char *p) {
    FILE *f;
    unsigned char m[4];
    int ok = 0;
    if (!p || !p[0]) return 0;
    f = fopen(p, "rb");
    if (!f) return 0;
    if (fread(m, 1, 4, f) == 4)
        ok = (m[0] == 'Q' && m[1] == 'F' && m[2] == 'I' && m[3] == 0xfb);
    fclose(f);
    return ok;
}

/* 挂起要写的内存量（和启动时 -m 用同一个算法） */
static int suspend_mem_mb(void) {
    int mem = vm.mem_auto ? host_avail_mb() - 100 : vm.mem;
    if (mem < 64) mem = 64;
    if (mem > host_avail_mb()) mem = host_avail_mb() - 100 > 256 ? host_avail_mb() - 100 : 256;
    if (!strcmp(vm.arch, "ppc") && mem > 2048) mem = 2048;
    return mem;
}

static void vm_suspend(void) {
    char json[512], reply[2048], err[256], mk[PATHBUF];
    int pid = read_pid(vm.name), i;

    if (!pid_alive(pid)) { ui_msg("虚拟机未启动，先启动再挂起"); return; }
    if (!vm.hda[0] || !disk_is_qcow2(vm.hda)) {
        ui_msg("挂起要把内存写进磁盘快照，只有 qcow2 能存。\n当前磁盘不是 qcow2（或没挂盘），先正常关机再换盘。");
        return;
    }
    {   /* 快照大概要写出已用内存那么大，空间不够就提前说，别写一半失败 */
        char dir[PATHBUF];
        struct statvfs vfs;
        snprintf(dir, sizeof dir, "%s", vm.hda);
        { char *sl = strrchr(dir, '/'); if (sl) *sl = '\0'; }
        if (statvfs(dir[0] ? dir : "/", &vfs) == 0) {
            long long freesz = (long long)vfs.f_bavail * vfs.f_frsize;
            long long need = (long long)suspend_mem_mb() * 1024LL * 1024LL / 2;   /* 一般不会写满整块内存 */
            if (freesz < need) {
                char q[256];
                snprintf(q, sizeof q, "磁盘空间可能不够：剩 %lld MB，挂起大概要 %lld MB。继续？",
                         freesz / 1048576LL, need / 1048576LL);
                if (!ui_confirm("%s", q)) return;
            }
        }
    }
    snprintf(json, sizeof json,
             "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"savevm suspend\"}}");
    {
        /* savevm 会把客户机暂停、把内存整块写进 qcow2，桌面虚拟机动辄几分钟，
           QMP 这条连接在整个过程中都不回应。原来就在 UI 里干等，看起来像死了。
           现在丢到子进程里去等，这边每 0.3 秒刷一行“已 N 秒”，页面和按键都还在。 */
        char resf[PATHBUF], d[PATHBUF], msg[256];
        int pfd[2] = { -1, -1 }, st = 0;
        pid_t kid;
        int mem = suspend_mem_mb();
        runtime_dir_for(vm.name, d, sizeof d);
        snprintf(resf, sizeof resf, "%s/.suspend-res", d);
        unlink(resf);
        snprintf(msg, sizeof msg, "正在把 %d MB 内存写进磁盘…虚拟机画面暂停是正常的，别退出", mem);
        tty_line_msg(msg);
        if (pipe(pfd) != 0) { ui_msg("pipe 失败"); return; }
        kid = fork();
        if (kid == 0) {
            char r2[2048], e2[256];
            int okc;
            close(pfd[0]);
            okc = qmp_exec(vm.name, json, r2, sizeof r2, e2, sizeof e2);
            if (okc == 0 && (strstr(r2, "\"error\"") || strstr(r2, "Error:")))
                okc = -1, snprintf(e2, sizeof e2, "%.400s", r2);
            {
                FILE *rf = fopen(resf, "w");
                if (rf) { fprintf(rf, "%d\n%s\n", okc, okc ? e2 : "ok"); fclose(rf); }
            }
            if (write(pfd[1], "x", 1) < 0) { }
            close(pfd[1]);
            _exit(okc == 0 ? 0 : 1);
        }
        close(pfd[1]);
        {
            int waited = 0;
            char t[300];
            while (1) {
                pid_t w = waitpid(kid, &st, WNOHANG);
                if (w == kid) break;
                if (w < 0) break;
                napms(300);
                waited++;
                if (waited % 3 == 0) {     /* 大约每秒刷一次 */
                    snprintf(t, sizeof t, "%s  已 %d 秒", msg, waited / 3);
                    tty_line_msg(t);
                }
            }
            close(pfd[0]);
            { FILE *rf = fopen(resf, "r"); char e3[512]; int cod = -1;
              e3[0] = '\0';
              if (rf) { if (fscanf(rf, "%d", &cod) != 1) cod = -1; { int ch_ = fgetc(rf); (void)ch_; } if (!fgets(e3, sizeof e3, rf)) e3[0] = '\0'; fclose(rf); }
              unlink(resf);
              if (cod != 0) {
                  char *nl = strchr(e3, '\n'); if (nl) *nl = '\0';
                  tty_line_clear();
                  if (strstr(e3, "does not support snapshots")) {
                      ui_msg("这台虚拟机挂了可写的 UEFI 变量盘[pflash]，\nQEMU 的内部快照不支持这种设备，所以挂起存不了\n\n请用[正常关机]；或者把固件改成不用 UEFI 变量盘[会丢启动项记忆]\nx86 用默认 BIOS 的机器不受影响");
                  } else {
                      ui_msg("保存失败：%s", e3[0] ? e3 : "qemu 没有回应");
                  }
                  return;
              }
              tty_line_msg("内存已写进磁盘，正在关闭虚拟机…");
            }
        }
    }

    qmp_exec(vm.name, "{\"execute\":\"quit\"}", reply, sizeof reply, err, sizeof err);
    for (i = 0; i < 100; i++) {
        if (!pid_alive(pid)) break;
        usleep(100000);
    }
    suspend_path_for(vm.name, mk, sizeof mk);
    { FILE *f = fopen(mk, "w"); if (f) fclose(f); }
    ui_msg("已挂起，内存已写进磁盘\n下次启动会接着跑，可以放心清掉 Termux");
}

/* 扔掉挂起存档 */
static void vm_drop_suspend(void) {
    char mk[PATHBUF], reply[2048], err[256], json[512];
    int pid = read_pid(vm.name);

    if (pid_alive(pid)) {
        snprintf(json, sizeof json,
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"delvm suspend\"}}");
        if (qmp_exec(vm.name, json, reply, sizeof reply, err, sizeof err) != 0) { ui_msg("%s", err); return; }
    } else if (vm.hda[0]) {
        char *a[] = {"qemu-img", "snapshot", "-d", "suspend", vm.hda, NULL};
        char out[2048];
        run_cmd(a, out, sizeof out);
    }
    suspend_path_for(vm.name, mk, sizeof mk);
    unlink(mk);
    ui_msg("挂起存档已删除");
}

static void action_screen(void);

/* 单台虚拟机的控制页 */
static void vm_page(void) {
    char header[4096], vnc[128];
    WINDOW *pending = NULL;
    const char *items[] = {
        "启动虚拟机    [后台运行]",
        "控制台启动    [占用终端]",
        "暂停/恢复     [不保存RAM]",
        "挂起          [保存RAM]",
        "删除挂起存档",
        "正常关机      [常规关闭]",
        "强制断电      [杀Qemu进程]",
        "截图保存为JPG",
        "查看Qemu日志",
        "查看启动命令",
        "修改启动配置",
        "虚拟磁盘工具",
        "Qemu环境检查",
        "删除此配置"
    };
    while (1) {
        int pid = vm_live_pid(&vm);      /* pid 文件丢了也要认出“正在跑”，否则又去起一台撞锁 */
        char mems[32], disk[64];
        vnc_text(&vm, vnc, sizeof vnc);
        snprintf(mems, sizeof mems, "%d", vm.mem);
        snprintf(disk, sizeof disk, "%s", vm.hda[0] ? base_name(vm.hda) : "[未选择]");
        if (vm.hda[0]) {
            long mb = file_mb(vm.hda);
            if (mb >= 1024) snprintf(disk + strlen(disk), sizeof disk - strlen(disk), " %.1fG", mb / 1024.0);
            else if (mb > 0) snprintf(disk + strlen(disk), sizeof disk - strlen(disk), " %ldM", mb);
        }
        {
            char st[32];
            int stv = vm_state_of(vm.name, pid);
            if (stv == 3) snprintf(st, sizeof st, "[运行中]");
            else if (stv == 2) snprintf(st, sizeof st, "[已暂停]");
            else if (stv == 1) snprintf(st, sizeof st, "[已挂起]");
            else snprintf(st, sizeof st, "[未启动]");
            snprintf(header, sizeof header,
                     "%s / %s / %s / %s MB / %d核\n%s    %s %s  SSH端口转发:%d\n%s%s%s",
                     vm.cpu[0] ? vm.cpu : "max", vm.arch, vm.machine, mems, vm.cores * vm.threads,
                     st, strcmp(vm.display, "vnc") == 0 ? "VNC" : vm.display, vnc,
                     vm.ssh_port, disk,
                     vm.cdrom[0] ? "  +ISO" : "",
                     (vm.hda[0] && !strncmp(vm.hda, "/storage/", 9)) ? "  [内部存储：偏慢]" : "");
        }
        {
            WINDOW *mw;
            int sel;
            if (pending) { close_win(pending); pending = NULL; }   /* 上一轮留下的旧窗口 */
            g_keep_menu = 1;                 /* 先别关窗口：删除/断电的确认要画在同一屏上 */
            sel = ui_menu_hdr(vm.name, header, items, 14);
            mw = g_menu_w; g_menu_w = NULL;
            if (sel < 0) { if (mw) close_win(mw); return; }
            if (sel == 13) { vm_delete(vm.name); if (mw) close_win(mw); return; }
            if (sel == 4 || sel == 6) pending = mw;   /* 这两项会弹确认，窗口留到确认完 */
            else if (mw) close_win(mw);
            if (sel == 0) {
                char err[512];
                if (vm_validate(&vm, err, sizeof err) != EXIT_OK) {
                    if (!vm.hda[0] && !vm.hda2[0] && prompt_make_disk(vm.name)) {
                        if (vm_validate(&vm, err, sizeof err) != EXIT_OK) { ui_msg("%s", err); continue; }
                    } else { ui_msg("%s", err); continue; }
                }
                if (disk_conflict_guard(&vm)) continue;
                if (vm_running(vm.name)) { vm_start_foreground(&vm); continue; }   /* 已在跑就直接接控制台 */
                {
                    char rdir[PATHBUF];
                    char note[320];
                    int p;
                    runtime_dir_for(vm.name, rdir, sizeof rdir);
                    p = vm_start_background(&vm, err, sizeof err);
                    note_join(note, sizeof note);
                    if (p > 0) ui_msg("已启动 pid %d\nVNC：%s\n日志：%s/%s.log%s%s", p, vnc, rdir, vm.name,
                                      note[0] ? "\n" : "", note);
                    else ui_msg("启动失败：%s", err);
                }
            } else if (sel == 1) {
                char err[512];
                if (vm_validate(&vm, err, sizeof err) != EXIT_OK) {
                    if (!vm.hda[0] && !vm.hda2[0] && prompt_make_disk(vm.name)) {
                        if (vm_validate(&vm, err, sizeof err) != EXIT_OK) { ui_msg("%s", err); continue; }
                    } else { ui_msg("%s", err); continue; }
                }
                if (disk_conflict_guard(&vm)) continue;
                if (pid_alive(pid)) {
                    char sp[PATHBUF];
                    serial_sock_path(vm.name, sp, sizeof sp);
                    if (access(sp, F_OK) != 0) {
                        if (!ui_confirm("这台在后台运行，串口未连接，停止并改用控制台重启？")) continue;
                        vm_force_stop();
                        if (pid_alive(pid)) { ui_msg("没停掉，先[强制停止]再试"); continue; }
                    }
                }
                vm_start_foreground(&vm);
            } else if (sel == 2) {
                const char *act[] = {"暂停[继续占内存]", "恢复运行", "查看状态",
                                     "发送 Ctrl+Alt+Del", "系统重启[Reset]"};
                int a = ui_menu("控制", act, 5);
                if (a == 0) do_control(1);
                else if (a == 1) do_control(2);
                else if (a == 2) do_control(0);
                else if (a == 3) do_control(6);
                else if (a == 4) do_control(5);
            } else if (sel == 3) vm_suspend();
            else if (sel == 4) { if (ui_confirm("删掉存档后，就不能再快速回滚，是否继续？")) vm_drop_suspend(); }
            else if (sel == 5) vm_poweroff();
            else if (sel == 6) {
                int fp = read_pid(vm.name);
                if (!pid_alive(fp)) ui_msg("虚拟机未启动，无法断电");
                else if (ui_confirm("将会丢失数据，甚至损坏虚拟机，是否继续？")) vm_force_stop();
            } else if (sel == 7) {
                char path[PATHBUF], json[512], reply[2048], err[256];
                { char d[PATHBUF]; runtime_dir_for(vm.name, d, sizeof d); snprintf(path, sizeof path, "%s/%s.ppm", d, vm.name); }
                snprintf(json, sizeof json, "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"%s\"}}", path);
                if (qmp_exec(vm.name, json, reply, sizeof reply, err, sizeof err) == 0)
                    ui_msg("已截图：%s\n转 PNG：python3 -c \"from PIL import Image;Image.open('%s').save('%s.png')\"", path, path, path);
                else ui_msg("%s", err);
            } else if (sel == 8) show_log();
            else if (sel == 9) show_command();
            else if (sel == 10) action_screen();
            else if (sel == 11) menu_img();
            else if (sel == 12) menu_env();
            else if (sel == 13) { vm_delete(vm.name); return; }
            else return;
        }
    }
}

/* ------------------------------------------------------------------
 * 配置页：分组的字段列表
 * ------------------------------------------------------------------ */
enum {
    F_ARCH = 1, F_MACHINE, F_CPU, F_CORES, F_THREADS, F_MEM, F_GPU, F_AUDIO,
    F_HDA, F_HDA2, F_CD, F_CD2, F_FDA, F_FDB, F_IFTYPE, F_SHARED,
    F_DISPLAY, F_VNCADDR, F_VNCPORT, F_VNCPASS, F_SPICE,
    F_NIC, F_NETUSER, F_SSH, F_DNS1, F_DNS2, F_BOOT, F_BOOTMENU,
    F_FW, F_BIOS, F_CODE, F_VARS, F_LOCALTIME, F_OVERCOMMIT, F_NVIRT, F_ARGS
};

typedef struct { int hdr; const char *label; int id; } Row;

static const Row cfg_rows[] = {
    {1, "[硬件]", 0},
    {0, "架构", F_ARCH},
    {0, "主板", F_MACHINE},
    {0, "CPU 型号", F_CPU},
    {0, "核心数", F_CORES},
    {0, "每核线程", F_THREADS},
    {0, "RAM", F_MEM},
    {0, "显卡", F_GPU},
    {0, "声卡", F_AUDIO},
    {2, "", 0},
    {1, "[存储]", 0},
    {0, "HardDisk A", F_HDA},
    {0, "HardDisk B", F_HDA2},
    {0, "CDRom A", F_CD},
    {0, "CDRom B", F_CD2},
    {0, "Floppy A", F_FDA},
    {0, "Floppy B", F_FDB},
    {0, "磁盘接口", F_IFTYPE},
    {0, "共享目录", F_SHARED},
    {2, "", 0},
    {1, "[显示]", 0},
    {0, "显示方式", F_DISPLAY},
    {0, "VNC 地址", F_VNCADDR},
    {0, "VNC 端口", F_VNCPORT},
    {0, "VNC 密码", F_VNCPASS},
    {0, "SPICE 端口", F_SPICE},
    {2, "", 0},
    {1, "[网络]", 0},
    {0, "网卡", F_NIC},
    {0, "User模式网络", F_NETUSER},
    {0, "SSH 转发端口", F_SSH},
    {0, "DNS 服务器 1", F_DNS1},
    {0, "DNS 服务器 2", F_DNS2},
    {2, "", 0},
    {1, "[启动与固件]", 0},
    {0, "启动顺序", F_BOOT},
    {0, "启动菜单", F_BOOTMENU},
    {0, "固件", F_FW},
    {0, "自定义 BIOS", F_BIOS},
    {0, "OVMF CODE", F_CODE},
    {0, "OVMF VARS", F_VARS},
    {2, "", 0},
    {1, "[高级设置]", 0},
    {0, "时间跟随物理机", F_LOCALTIME},
    {0, "mem-lock=off", F_OVERCOMMIT},
    {0, "嵌套虚拟化", F_NVIRT},
    {0, "自定义Qemu参数", F_ARGS}
};
#define CFG_N ((int)(sizeof cfg_rows / sizeof cfg_rows[0]))

static void field_value(int id, char *out, int n) {
    switch (id) {
    case F_ARCH: snprintf(out, n, "%s", vm.arch); break;
    case F_MACHINE: snprintf(out, n, "%s", vm.machine[0] ? vm.machine : "[默认]"); break;
    case F_CPU: snprintf(out, n, "%s", vm.cpu[0] ? vm.cpu : "[默认]"); break;
    case F_CORES: snprintf(out, n, "%d", vm.cores); break;
    case F_THREADS: snprintf(out, n, "%d", vm.threads); break;
    case F_MEM: if (vm.mem_auto) snprintf(out, n, "自动（%d M）", host_avail_mb() - 100); else snprintf(out, n, "%d M", vm.mem); break;
    case F_GPU: snprintf(out, n, "%s", vm.gpu[0] ? vm.gpu : "[不设]"); break;
    case F_AUDIO: snprintf(out, n, "%s", strcmp(vm.audio, "none") ? vm.audio : "[无]"); break;
    case F_HDA: snprintf(out, n, "%s", vm.hda[0] ? vm.hda : "[未选择]"); break;
    case F_HDA2: snprintf(out, n, "%s", vm.hda2[0] ? vm.hda2 : "[未选择]"); break;
    case F_CD: snprintf(out, n, "%s", vm.cdrom[0] ? vm.cdrom : "[未选择]"); break;
    case F_CD2: snprintf(out, n, "%s", vm.cdrom2[0] ? vm.cdrom2 : "[未选择]"); break;
    case F_FDA: snprintf(out, n, "%s", vm.fda[0] ? vm.fda : "[未选择]"); break;
    case F_FDB: snprintf(out, n, "%s", vm.fdb[0] ? vm.fdb : "[未选择]"); break;
    case F_IFTYPE: snprintf(out, n, "%s", vm.if_type[0] ? vm.if_type : "[不指定]"); break;
    case F_SHARED:
        if (vm.shared && vm.shared_path[0]) snprintf(out, n, "%s", vm.shared_path);
        else snprintf(out, n, "[未启用]");
        break;
    case F_DISPLAY: snprintf(out, n, "%s", vm.display); break;
    case F_VNCADDR: snprintf(out, n, "%s", vm.vnc_addr); break;
    case F_VNCPORT: snprintf(out, n, "%d", vm.vnc_port); break;
    case F_VNCPASS: snprintf(out, n, "%s", vm.vnc_pass[0] ? vm.vnc_pass : "[无密码]"); break;
    case F_SPICE: snprintf(out, n, "%d", vm.spice_port); break;
    case F_NIC: snprintf(out, n, "%s", vm.nic[0] ? vm.nic : "[不接网卡]"); break;
    case F_NETUSER: snprintf(out, n, "%s", vm.net_user ? "[开]" : "[关]"); break;
    case F_SSH: snprintf(out, n, "%d", vm.ssh_port); break;
    case F_DNS1: snprintf(out, n, "%s", vm.dns1[0] ? vm.dns1 : "[空]"); break;
    case F_DNS2: snprintf(out, n, "%s", vm.dns2[0] ? vm.dns2 : "[空]"); break;
    case F_BOOT: boot_value(out, n); break;
    case F_BOOTMENU: snprintf(out, n, "%s", vm.boot_menu ? "[开]" : "[关]"); break;
    case F_FW:
        if (!strcmp(vm.firmware, "uefi")) snprintf(out, n, "[UEFI]");
        else if (!strcmp(vm.firmware, "none")) snprintf(out, n, "[不用固件]");
        else snprintf(out, n, "[默认BIOS]");
        break;
    case F_BIOS: snprintf(out, n, "%s", vm.bios_file[0] ? vm.bios_file : "[默认]"); break;
    case F_CODE: snprintf(out, n, "%s", vm.code_file[0] ? vm.code_file : "[默认]"); break;
    case F_VARS: snprintf(out, n, "%s", vm.vars_file[0] ? vm.vars_file : "[默认]"); break;
    case F_LOCALTIME: snprintf(out, n, "%s", vm.localtime ? "[开]" : "[关]"); break;
    case F_OVERCOMMIT: snprintf(out, n, "%s", vm.overcommit ? "[开]" : "[关]"); break;
    case F_NVIRT: snprintf(out, n, "%s", vm.nvirt ? "[开]" : "[关]"); break;
    case F_ARGS: snprintf(out, n, "%s", vm.args[0] ? vm.args : "[空]"); break;
    default: out[0] = '\0'; break;
    }
}

static void field_edit(int id) {
    char t[PATHBUF];
    switch (id) {
    case F_ARCH: menu_arch(); break;
    case F_MACHINE: menu_machine(); break;
    case F_CPU: menu_cpu(); break;
    case F_CORES: menu_cores(); break;
    case F_THREADS: menu_threads(); break;
    case F_MEM: menu_mem(); break;
    case F_GPU: menu_gpu(); break;
    case F_AUDIO: menu_audio(); break;
    case F_HDA: ui_browse("选择主磁盘", "/storage/emulated/0", ".qcow2", vm.hda, sizeof vm.hda, 0); break;
    case F_HDA2: ui_browse("选择第二块硬盘", "/storage/emulated/0", ".qcow2", vm.hda2, sizeof vm.hda2, 0); break;
    case F_CD: ui_browse("选择光盘 1", "/storage/emulated/0", ".iso", vm.cdrom, sizeof vm.cdrom, 0); break;
    case F_CD2: ui_browse("选择光盘 2", "/storage/emulated/0", ".iso", vm.cdrom2, sizeof vm.cdrom2, 0); break;
    case F_FDA: ui_browse("选择软盘 a", "/storage/emulated/0", ".img", vm.fda, sizeof vm.fda, 0); break;
    case F_FDB: ui_browse("选择软盘 b", "/storage/emulated/0", ".img", vm.fdb, sizeof vm.fdb, 0); break;
    case F_IFTYPE: {
        const char *items[] = {"virtio（最快）", "ide", "scsi", "sata", "nvme", "不指定（用 -hda）"};
        const char *vals[] = {"virtio", "ide", "scsi", "sata", "nvme", ""};
        pick_value("磁盘接口", vm.if_type, items, vals, 6, vm.if_type, sizeof vm.if_type);
        break;
    }
    case F_SHARED: {
        if (vm.shared && ui_confirm("关闭共享目录？\n当前：%s", vm.shared_path)) {
            vm.shared = 0;
        } else if (!vm.shared) {
            if (!vm.shared_path[0]) snprintf(vm.shared_path, sizeof vm.shared_path, "%s/shared", HOME_DIR);
            if (ui_browse("选择要共享的目录", "/storage/emulated/0", "", vm.shared_path, sizeof vm.shared_path, 1))
                vm.shared = 1;
        }
        break;
    }
    case F_DISPLAY: menu_display(); break;
    case F_VNCADDR: {
        snprintf(t, sizeof t, "%s", vm.vnc_addr);
        while (ui_input("VNC 地址", "127.0.0.1 只给本机，0.0.0.0 允许局域网", t, t, sizeof t, 0)) {
            char why[128];
            if (valid_addr(t, why, sizeof why)) {
                snprintf(vm.vnc_addr, sizeof vm.vnc_addr, "%s", t);
                break;
            }
            flash_err(why);
        }
        break;
    }
    case F_VNCPORT: ui_num("VNC 端口", "端口", 5900, 5999, 1, &vm.vnc_port); break;
    case F_VNCPASS: {
        snprintf(t, sizeof t, "%s", vm.vnc_pass);
        if (ui_input("VNC 密码", "留空表示不要密码", t, t, sizeof t, 0))
            snprintf(vm.vnc_pass, sizeof vm.vnc_pass, "%s", t);
        break;
    }
    case F_SPICE: ui_num("SPICE 端口", "端口", 1024, 65535, 100, &vm.spice_port); break;
    case F_NIC: menu_nic(); break;
    case F_NETUSER: vm.net_user = !vm.net_user; break;
    case F_SSH: ui_input_port("SSH 转发端口", &vm.ssh_port); break;
    case F_DNS1: snprintf(t, sizeof t, "%s", vm.dns1); if (ui_input("DNS 1", "留空表示不用", t, t, sizeof t, 0)) snprintf(vm.dns1, sizeof vm.dns1, "%s", t); break;
    case F_DNS2: snprintf(t, sizeof t, "%s", vm.dns2); if (ui_input("DNS 2", "留空表示不用", t, t, sizeof t, 0)) snprintf(vm.dns2, sizeof vm.dns2, "%s", t); break;
    case F_BOOT: menu_boot(); break;
    case F_BOOTMENU: vm.boot_menu = !vm.boot_menu; break;
    case F_FW: menu_firmware(); break;
    case F_BIOS: ui_browse("选择 BIOS 文件", "/storage/emulated/0", ".bin", vm.bios_file, sizeof vm.bios_file, 0); break;
    case F_CODE: ui_browse("选择 OVMF CODE", "/storage/emulated/0", ".fd", vm.code_file, sizeof vm.code_file, 0); break;
    case F_VARS: ui_browse("选择 OVMF VARS", "/storage/emulated/0", ".fd", vm.vars_file, sizeof vm.vars_file, 0); break;
    case F_LOCALTIME: vm.localtime = !vm.localtime; break;
    case F_OVERCOMMIT: vm.overcommit = !vm.overcommit; break;
    case F_NVIRT: vm.nvirt = !vm.nvirt; break;
    case F_ARGS: menu_args(); break;
    default: break;
    }
}

/* 配置页：一个可滚动的分组表单，方向键选字段、回车改 */
/* 配置页：分组字段，方向键选、回车改 */
/* 配置页：极简表单 */
static int neg_value(const char *v) {
    if (!v || v[0] != '[') return 0;
    if (strstr(v, "未") || strstr(v, "无") || strstr(v, "关") ||
        strstr(v, "空") || strstr(v, "不")) return 1;
    return 0;
}

static int cfg_inline_edit(WINDOW *w, int y, int id) {
    char *dst = NULL;
    char init[PATHBUF], got[PATHBUF];
    int maxlen = 0, ok, num = 0, lo = 0, hi = 0;
    snprintf(init, sizeof init, "%s", "");
    switch (id) {
    case F_VNCADDR: dst = vm.vnc_addr; maxlen = sizeof vm.vnc_addr; break;
    case F_VNCPASS: dst = vm.vnc_pass; maxlen = sizeof vm.vnc_pass; break;
    case F_DNS1: dst = vm.dns1; maxlen = sizeof vm.dns1; break;
    case F_DNS2: dst = vm.dns2; maxlen = sizeof vm.dns2; break;
    case F_ARGS: dst = vm.args; maxlen = sizeof vm.args; break;
    case F_SHARED: dst = vm.shared_path; maxlen = sizeof vm.shared_path; break;
    case F_VNCPORT: num = 1; lo = 5900; hi = 5999; break;
    case F_SPICE: num = 2; lo = 1024; hi = 65535; break;
    case F_SSH: num = 3; lo = 0; hi = 65535; break;
    case F_CORES: num = 4; lo = 1; hi = host_cores(); break;
    case F_THREADS: num = 5; lo = 1; hi = 4; break;
    case F_MEM: num = 6; lo = 256; hi = host_avail_mb(); break;
    default: return -1;
    }
    if (num == 1) snprintf(init, sizeof init, "%d", vm.vnc_port);
    else if (num == 2) snprintf(init, sizeof init, "%d", vm.spice_port);
    else if (num == 3) snprintf(init, sizeof init, "%d", vm.ssh_port > 0 ? vm.ssh_port : 0);
    else if (num == 4) snprintf(init, sizeof init, "%d", vm.cores);
    else if (num == 5) snprintf(init, sizeof init, "%d", vm.threads);
    else if (num == 6) { if (!vm.mem_auto) snprintf(init, sizeof init, "%d", vm.mem); }
    else snprintf(init, sizeof init, "%s", dst);
    {
        const Row *r = &cfg_rows[0];
        int i;
        for (i = 0; i < CFG_N; i++) if (cfg_rows[i].id == id) { r = &cfg_rows[i]; break; }
        mvwhline(w, y, 0, ' ', getmaxx(w));     /* 输入时整行保持终端原底色，不要白条 */
        wclrtoeol(w);
        put_u8(w, y, 2, r->label, 17, A_BOLD);
    }
    wrefresh(w);
    yellow_hint(w, getmaxy(w) - 2, "输入中：Enter 保存   Esc 取消");
    wrefresh(w);
    ok = inline_text(w, y, 20, init, got, maxlen > 0 ? maxlen : (int)sizeof got, id == F_VNCPASS, num != 0);
    if (!ok) { touchwin(w); return 0; }
    if (num) {
        char *p;
        int v;
        if (!got[0]) {
            if (num == 6) { vm.mem_auto = 1; return 1; }
            flash_err("这一项不能空着"); touchwin(w); return 0;
        }
        for (p = got; *p; p++) if (*p < '0' || *p > '9') { flash_err("只能填数字"); touchwin(w); return 0; }
        v = atoi(got);
        if (v < lo || v > hi) {
            char msg[96];
            if (num == 6) snprintf(msg, sizeof msg, "内存要在 %d-%d MB", lo, hi);
            else if (num == 3) snprintf(msg, sizeof msg, "端口填 0 关闭，或 1024-65535");
            else snprintf(msg, sizeof msg, "范围 %d-%d", lo, hi);
            flash_err(msg); touchwin(w); return 0;
        }
        if (num == 1) vm.vnc_port = v;
        else if (num == 2) vm.spice_port = v;
        else if (num == 3) vm.ssh_port = v;
        else if (num == 4) vm.cores = v;
        else if (num == 5) vm.threads = v;
        else if (num == 6) { vm.mem = v; vm.mem_auto = 0; }
        return 1;
    }
    if (id == F_VNCADDR) {
        char why[128];
        if (!valid_addr(got, why, sizeof why)) { flash_err(why); touchwin(w); return 0; }
    }
    snprintf(dst, maxlen, "%s", got);
    if (id == F_SHARED) vm.shared = got[0] ? 1 : 0;
    return 1;
}

static void cfg_paint_row(WINDOW *w, int rowi, int idx, int selp) {
    const Row *r = &cfg_rows[idx];
    int y = 2 + rowi;
    char val[PATHBUF];
    wmove(w, y, 0);
    wclrtoeol(w);                        /* 只清这一行，不整屏重画 */
    if (r->hdr == 2) return;
    if (r->hdr) { put_u8(w, y, 1, r->label, COLS - 2, A_BOLD); return; }
    field_value(r->id, val, sizeof val);
    if (selp) {
        wattron(w, COLOR_PAIR(6));
        mvwhline(w, y, 0, ' ', COLS);
        wattroff(w, COLOR_PAIR(6));
    }
    {
        int vattr = selp ? COLOR_PAIR(6) : (neg_value(val) ? g_grey : 0);
        put_u8(w, y, 2, r->label, 17, selp ? (COLOR_PAIR(6) | A_BOLD) : 0);
        put_u8(w, y, 20, val, COLS - 24, vattr);
    }
}

static void action_screen(void) {
    int sel = 1, top = 0, ch;
    WINDOW *w = NULL;
    int need = 1, last_act = -1, dirty = 0;

    while (1) {
        int rows_vis = LINES - 4, i;
        char head[192], pos[32];

        if (sel > CFG_N - 1) sel = CFG_N - 1;
        if (sel < top) top = sel;
        if (sel >= top + rows_vis) top = sel - rows_vis + 1;
        if (!w) {
            w = newwin(LINES, COLS, 0, 0);
    g_top = w;
            if (!w) return;
            keypad(w, TRUE);
            need = 1;
        }

        if (need) {
            werase(w);
            snprintf(head, sizeof head, dirty ? "%s - 配置 *" : "%s - 配置", vm.name);
            put_u8(w, 0, 1, head, COLS - 12, A_BOLD);
            put_right(w, 0, COLS - 2, vm.arch, g_grey);
            rule(w, 1);
            for (i = 0; i < rows_vis && top + i < CFG_N; i++)
                cfg_paint_row(w, i, top + i, (top + i) == sel);
            {
                const char *hint = "↑↓ [选择]   Enter [修改]   S [保存]   Esc [返回]";
                if (dwidth(hint) > COLS - 18) hint = "↑↓ Enter 改   Esc 返回";
                status_or_hint_at(w, LINES - 1, hint);
                snprintf(pos, sizeof pos, "%d/%d", sel + 1, CFG_N);
                if (status_active()) pos[0] = '\0';
                put_right(w, LINES - 1, COLS - 2, pos, g_grey);
            }
            wrefresh(w);
            need = 0;
            last_act = status_active();
        }

        wtimeout(w, 300);
        ch = wgetch(w);
        if (ch == ERR) {
            int act = status_active();
            if (act != last_act) { need = 1; last_act = act; }
            continue;
        }
        if (ch == 27) {
            if (dirty) {
                int k;
                status_or_hint_at(w, LINES - 1,
                    "有未保存的改动：S 保存并返回   N 不保存返回   Esc 继续修改");
                wrefresh(w);
                wtimeout(w, -1);
                k = wgetch(w);
                wtimeout(w, 300);
                if (k == 's' || k == 'S' || k == 'y' || k == 'Y') {
                    vm_save(&vm);
                } else if (k == 'n' || k == 'N' || k == 'q' || k == 'Q') {
                    { char keep[64]; snprintf(keep, sizeof keep, "%s", vm.name); vm_load(keep); }   /* 丢掉内存里的改动 */
                } else {
                    need = 1;
                    continue;                  /* 留在配置页继续改 */
                }
            }
            close_win(w);
            return;
        }

        if (ch == 's' || ch == 'S') {
            int k;
            char q[512], path[PATHBUF], shown[PATHBUF];
            const char *a = "保存到 %s ？[y] 保存 [n] 取消";
            const char *b = "保存 %s ？ y 保存  n 取消";
            const char *c = "保存 %s ？ y/n";
            const char *fmt;
            int fixedw, avail;
            snprintf(path, sizeof path, "%s/%s.sh", CONF_DIR, vm.name);
            snprintf(q, sizeof q, a, "");
            if (dwidth(q) + 10 <= COLS - 2) fmt = a;
            else {
                snprintf(q, sizeof q, b, "");
                if (dwidth(q) + 10 <= COLS - 2) fmt = b;
                else fmt = c;
            }
            snprintf(q, sizeof q, fmt, "");
            fixedw = dwidth(q);
            avail = COLS - 2 - fixedw;
            short_path(path, shown, sizeof shown, avail);
            snprintf(q, sizeof q, fmt, shown);
            yellow_hint(w, LINES - 1, q);
            wrefresh(w);
            wtimeout(w, -1);
            k = wgetch(w);
            wtimeout(w, 300);
            if (k == 'y' || k == 'Y' || k == 's' || k == 'S') {
                vm_save(&vm);
                dirty = 0;
                ui_msg("已保存\n%s/%s.sh", CONF_DIR, vm.name);
            }
            need = 1;
            continue;
        }

        if (ch == KEY_UP || ch == 'k' || ch == KEY_DOWN || ch == 'j') {
            int old = sel;
            int n2 = sel;
            if (ch == KEY_UP || ch == 'k') {
                do { n2--; } while (n2 > 0 && cfg_rows[n2].hdr);
                if (n2 >= 0 && !cfg_rows[n2].hdr) sel = n2;
            } else {
                do { n2++; } while (n2 < CFG_N - 1 && cfg_rows[n2].hdr);
                if (n2 <= CFG_N - 1 && !cfg_rows[n2].hdr) sel = n2;
            }
            if (sel < top || sel >= top + rows_vis) {
                need = 1;
            } else if (sel != old) {
                cfg_paint_row(w, old - top, old, 0);
                cfg_paint_row(w, sel - top, sel, 1);
                {
                    const char *hint = "↑↓ [选择]   Enter [修改]   S [保存]   Esc [返回]";
                    if (dwidth(hint) > COLS - 18) hint = "↑↓ Enter 改   Esc 返回";
                    status_or_hint_at(w, LINES - 1, hint);
                    snprintf(pos, sizeof pos, "%d/%d", sel + 1, CFG_N);
                    if (status_active()) pos[0] = '\0';
                    put_right(w, LINES - 1, COLS - 2, pos, g_grey);
                }
                wrefresh(w);
            }
            last_act = status_active();
            continue;
        }

        if (ch == KEY_NPAGE) {
            sel += rows_vis; if (sel > CFG_N - 1) sel = CFG_N - 1;
            top = sel - rows_vis + 1; if (top < 0) top = 0;
            need = 1; continue;
        }
        if (ch == KEY_PPAGE) {
            sel -= rows_vis; if (sel < 0) sel = 0;
            top = sel; if (top > CFG_N - rows_vis) top = CFG_N - rows_vis > 0 ? CFG_N - rows_vis : 0;
            need = 1; continue;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (!cfg_rows[sel].hdr) {
                int id = cfg_rows[sel].id;
                /* 能直接填的就在这一行填，别跳到别处去问 */
                {
                    int y = 2 + (sel - top);
                    int r = cfg_inline_edit(w, y, id);
                    if (r != -1) {
                        if (r) dirty = 1;
                        need = 1;
                        continue;
                    }
                }
                close_win(w);
                w = NULL;
                field_edit(id);
                dirty = 1;
                need = 1;
            }
            continue;
        }
    }
}




/* 主页：虚拟机卡片列表 */
/* 主页：虚拟机卡片列表 */
/* 主页：一条一条的虚拟机，简约列表 */
static void home_paint_slot(WINDOW *w, int top, int rowi, int idx, Card *cards, int n,
                            int total, int wid, int sel) {
    int y = 3 + rowi * 3;   /* 顶部空一行 */
    char sub[64];
    int miss;
    if (y + 2 > LINES - 2) return;
    mvwhline(w, y, 0, ' ', COLS);
    mvwhline(w, y + 1, 0, ' ', COLS);
    mvwhline(w, y + 2, 0, ' ', COLS);
    if (idx < n) {
        draw_entry(w, y, 1, wid, &cards[idx], sel);
    } else if (idx == n) {
        draw_new_entry(w, y, 1, wid, sel);
    } else {
        miss = fw_missing();
        if (miss) snprintf(sub, sizeof sub, "缺失：%d个", miss);
        else snprintf(sub, sizeof sub, "[就绪]");
        if (sel) {
            wattron(w, COLOR_PAIR(6));
            mvwhline(w, y, 0, ' ', getmaxx(w));
            mvwhline(w, y + 1, 0, ' ', getmaxx(w));
            wattroff(w, COLOR_PAIR(6));
        }
        put_u8(w, y, 1, "系统固件", wid - 10 > 4 ? wid - 10 : 4, sel ? (COLOR_PAIR(6) | A_BOLD) : 0);
        if (wid >= 30) put_right(w, y, wid, sub, sel ? COLOR_PAIR(6) : g_grey);
    }
    rule(w, y + 2);
    (void)top; (void)total;
}

static void home_screen(void) {
    int sel = 0, top = 0, ch;
    WINDOW *w = NULL;
    int need = 1, last_act = -1;

    while (1) {
        Card cards[32];
        int n = scan_cards(cards, 32);
        int total = n + 2;
        int wid, vis, i;

        vis = (LINES - 5) / 3;
        if (vis < 1) vis = 1;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;

        if (!w) {
            w = newwin(LINES, COLS, 0, 0);
    g_top = w;
            if (!w) return;
            keypad(w, TRUE);
        }
        wid = COLS - 3;
        if (wid > 76) wid = 76;

        if (need) {
            werase(w);
            put_u8(w, 1, 1, "Qemu Tools", 0, A_BOLD);
            put_right(w, 1, wid, "v" APP_VERSION, g_grey);
            rule(w, 2);
            for (i = 0; i < vis && top + i < total; i++) {
                int idx = top + i;
                if (3 + i * 3 + 2 > LINES - 2) break;
                home_paint_slot(w, top, i, idx, cards, n, total, wid, idx == sel);
            }
            {
                const char *hint = "↑↓ [选择]   Enter [打开]   b [启动]   c [配置]   x [删除]   Esc [退出]";
                if (dwidth(hint) > COLS - 2) hint = "↑↓ [选择]  Enter [打开]  c [配置]  Esc [退出]";
                if (dwidth(hint) > COLS - 2) hint = "↑↓[选择] Enter[打开] Esc[退出]";
                status_or_hint_at(w, LINES - 1, hint);
            }
            wrefresh(w);
            need = 0;
            last_act = status_active();
        }

        wtimeout(w, 300);
        ch = wgetch(w);
        if (ch == ERR) {
            int act = status_active();
            if (act != last_act) { need = 1; last_act = act; }
            continue;
        }

        if (ch == 27) { close_win(w); return; }        /* 只留 Esc 退出，q 不再退出 */

        if (ch == KEY_UP || ch == KEY_DOWN) {
            int old = sel;
            int moved = 0;
            if (ch == KEY_UP && sel > 0) { sel--; moved = 1; }
            if (ch == KEY_DOWN && sel < total - 1) { sel++; moved = 1; }
            if (moved) {
                if (sel < top || sel >= top + vis) {
                    need = 1;                       /* 翻页了，整屏重画 */
                } else {
                    home_paint_slot(w, top, old - top, old, cards, n, total, wid, 0);
                    home_paint_slot(w, top, sel - top, sel, cards, n, total, wid, 1);
                    {
                        const char *hint = "↑↓ [选择]   Enter [打开]   b [启动]   c [配置]   x [删除]   Esc [退出]";
                        if (dwidth(hint) > COLS - 2) hint = "↑↓ [选择]  Enter [打开]  c [配置]  Esc [退出]";
                        if (dwidth(hint) > COLS - 2) hint = "↑↓[选择] Enter[打开] Esc[退出]";
                        status_or_hint_at(w, LINES - 1, hint);
                    }
                    wrefresh(w);
                }
                last_act = status_active();
            }
            continue;
        }

        /* 按 n 只是把光标挪到“＋ 新建虚拟机”，跟回车一样就地输入 */
        if (ch == 'n' || ch == 'N') {
            sel = n;
            if (sel < top) top = sel;
            if (sel >= top + vis) top = sel - vis + 1;
        }

        /* 新建虚拟机：就在“＋ 新建虚拟机”这一行上输入，不跳新页面 */
        if ((ch == 'n' || ch == 'N' ||
             ch == '\n' || ch == '\r' || ch == KEY_ENTER) && sel == n) {
            int y = 3 + (sel - top) * 3;
            char nm[64];
            int x;
            mvwhline(w, y, 0, ' ', getmaxx(w));
            mvwhline(w, y + 1, 0, ' ', getmaxx(w));
            put_u8(w, y, 1, "＋ 新建虚拟机", 0, A_BOLD);
            x = getcurx(w) + 2;
            wrefresh(w);
            if (inline_text(w, y, x, "", nm, sizeof nm, 0, 0)) {
                new_vm_named(nm);
            }
            touchwin(stdscr);
            need = 1;
            continue;
        }

        if ((ch == 'x' || ch == 'X' || ch == 'b' || ch == 'B') && sel >= n) {
            status_set(0, "这张卡不是虚拟机，先去「＋ 新建虚拟机」");
            continue;
        }

        /* 需要弹确认的两件事先做完再关窗口：
           窗口一关，确认就只能画在空白的 stdscr 上，看着像跳到了另一个界面 */
        if ((ch == 'x' || ch == 'X') && sel < n) {
            vm_delete(cards[sel].name);
            close_win(w);                    /* 卡片没了，整屏重建一遍，别留旧格子 */
            w = NULL;
            need = 1;
            last_act = status_active();
            continue;
        }
        if ((ch == 'b' || ch == 'B') && sel < n) {
            vm_load(cards[sel].name);
            if (vm_validate(&vm, NULL, 0) == EXIT_OK) {
                int rp = vm_live_pid(&vm);
                if (!(rp > 0) && disk_conflict_guard(&vm)) {
                    need = 1;
                    last_act = status_active();
                    continue;
                }
            }
        }

        close_win(w);
        w = NULL;
        if ((ch == 'b' || ch == 'B') && sel < n) {
            char err[512];
            int runpid;
            vm_load(cards[sel].name);
            runpid = vm_live_pid(&vm);
            if (vm_validate(&vm, err, sizeof err) != EXIT_OK) { ui_msg("%s", err); }
            else if (runpid > 0) {
                /* 已经在跑：b 直接接控制台，不再重复启动 */
                vm_start_foreground(&vm);
            } else {
                char note[320];
                int p = vm_start_background(&vm, err, sizeof err);
                note_join(note, sizeof note);
                if (p > 0) ui_msg("已启动 %s（pid %d）%s%s", vm.name, p,
                                  note[0] ? "\n" : "", note);
                else ui_msg("启动失败：%s", err);
            }
        } else if ((ch == 'c' || ch == 'C') && sel < n) {
            vm_load(cards[sel].name);
            action_screen();
        } else if ((ch == 'x' || ch == 'X') && sel < n) {
            vm_delete(cards[sel].name);
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (sel == n) new_vm();
            else if (sel > n) firmware_screen();
            else { vm_load(cards[sel].name); vm_page(); }
        } else if (ch == KEY_RESIZE) {
            ;
        }
        touchwin(stdscr);
        need = 1;
        last_act = status_active();
    }
}




/* 配置名不允许跳出 ~/start（CLI 传参时防 ../ 之类） */
static int name_safe(const char *n) {
    if (!n || !*n) return 0;
    if (*n == '.' || strstr(n, "..")) return 0;
    if (strchr(n, '/') || strchr(n, '\\')) return 0;
    return 1;
}

static int cli_mode(int argc, char **argv) {
    char *outargs[MAXARGV];
    char pool[MAXARGV][PATHBUF];
    int n, i, bg;
    const char *name = (argc > 2) ? argv[2] : "default";
    char probe[PATHBUF], err[512];

    bg = (!strcmp(argv[1], "--run-bg") || !strcmp(argv[1], "--command-bg"));
    if (!name_safe(name)) {
        fprintf(stderr, "配置名不合法：%s\n", name);
        return EXIT_INVALID_PARAM;
    }
    conf_path(name, probe, sizeof probe);
    if (!file_exists(probe)) {
        fprintf(stderr, "找不到配置：%s\n", probe);
        return EXIT_FILE_MISS;
    }
    vm_load(name);
    if (vm_validate(&vm, err, sizeof err) != EXIT_OK) {
        fprintf(stderr, "%s\n", err);
        return EXIT_INVALID_PARAM;
    }
    /* 真要启动（不是 --command 预览）时，端口冲突也按 2222/5901 重新扫 */
    if (!strncmp(argv[1], "--run", 5)) vm_fix_ports(&vm);
    /* 后台形态和界面里「启动」走的是同一套串口通道，打印出来的才是真会执行的命令 */
    n = vm_build_argv(&vm, outargs, MAXARGV, pool, MAXARGV, bg, bg ? "auto" : NULL);
    if (!strcmp(argv[1], "--command") || !strcmp(argv[1], "--command-bg")) {
        for (i = 0; i < n; i++) printf("%s%s", i ? " " : "", outargs[i]);
        printf("\n");
        return EXIT_OK;
    }

    /* 真启动之前先看是不是已经有一台在跑，不然两块 qemu 抢同一块磁盘 */
    {
        int pid = read_pid(name);
        if (pid > 0 && pid_alive(pid)) {
            fprintf(stderr, "%s 已经在运行（pid %d），先停掉它再启动\n", name, pid);
            return EXIT_INVALID_PARAM;
        }
    }

    if (bg) {
        char sp[PATHBUF], qp[PATHBUF], pp[PATHBUF];
        int fd;
        serial_sock_path(vm.name, sp, sizeof sp);
        qmp_path_for(vm.name, qp, sizeof qp);
        unlink(sp);
        unlink(qp);
        pid_path_for(vm.name, pp, sizeof pp);
        fd = open(pp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {                     /* exec 之后这个 pid 就是 qemu 自己的 */
            char buf[32];
            snprintf(buf, sizeof buf, "%d\n", (int)getpid());
            if (write(fd, buf, strlen(buf)) < 0) { }
            close(fd);
        }
        { char sus[PATHBUF]; suspend_path_for(vm.name, sus, sizeof sus); unlink(sus); }
    }
    execvp(outargs[0], outargs);
    fprintf(stderr, "启动失败：%s\n", outargs[0]);
    return EXIT_QEMU_FAIL;
}

/* 子进程退出即回收，避免僵尸（容器/安卓里子进程可能挂到本进程名下） */
static void on_sigchld(int sig) {
    int old;
    (void)sig;
    old = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    errno = old;
}

static void on_term_signal(int sig) {
    if (!isendwin()) endwin();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void setup_reaper(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* 启动前自检：照原版脚本 POST 的做法，先确认 qemu 程序和固件在不在。
   有问题就整页列出来，而不是等用户点启动才在底行报一句被截断的话。 */
static void startup_env_check(void) {
    static char *lines[16];
    char a[PATHBUF], sp[PATHBUF], out[512];
    const char *bins[] = { "qemu-system-x86_64", "qemu-system-aarch64", "qemu-img" };
    const char *labels[] = { "x86_64 模拟器", "ARM64 模拟器", "磁盘工具" };
    int i, n = 0, bad = 0;

    for (i = 0; i < 3; i++) {
        const char *wp = which_path(bins[i]);
        char *l = malloc(PATHBUF + 160);
        if (!l) break;
        if (!wp[0]) {
            snprintf(l, PATHBUF + 160, "　%s　未安装", labels[i]);
            bad++;
        } else {
            char *av[3] = { (char *)bins[i], "--version", NULL };
            out[0] = '\0';
            if (run_cmd(av, out, sizeof out) != 0) {
                snprintf(l, PATHBUF + 160, "　%s　无法运行（安装包可能已损坏）", labels[i]);
                bad++;
            } else {
                snprintf(l, PATHBUF + 160, "　%s　正常", labels[i]);
            }
        }
        lines[n++] = l;
    }
    fw_path(a, sizeof a, "QEMU_EFI.img");
    {
        char *l = malloc(PATHBUF + 200);
        if (!file_exists(a)) {
            snprintf(l, PATHBUF + 200, "　ARM UEFI 固件　未安装");
            bad++;
        } else if (!fw_arm_size_ok(a)) {
            snprintf(l, PATHBUF + 200, "　ARM UEFI 固件　大小异常（完整应为 60–70 MB）");
            bad++;
        } else {
            short_path(a, sp, sizeof sp, COLS - 8);
            snprintf(l, PATHBUF + 200, "　ARM UEFI 固件　正常　%s", sp);
        }
        lines[n++] = l;
    }
    if (bad) {
        char *l = malloc(260);
        snprintf(l, 260, "发现 %d 个问题，对应架构的虚拟机可能起不来。", bad);
        lines[n++] = l;
        l = malloc(300);
        snprintf(l, 300, "修复：pkg install qemu-utils qemu-system-x86_64-headless qemu-system-aarch64-headless");
        lines[n++] = l;
        l = malloc(200);
        snprintf(l, 200, "按 q 或 Esc 继续使用（也可以先新建一台 x86 BIOS 虚拟机）。");
        lines[n++] = l;
        ui_pager("启动前环境检查", lines, n);
    }
    for (i = 0; i < n; i++) free(lines[i]);
}

int main(int argc, char **argv) {
    char *home = getenv("HOME");
    snprintf(HOME_DIR, sizeof HOME_DIR, "%s", home ? home : "/tmp");
    snprintf(ROOT_DIR, sizeof ROOT_DIR, "%s/.vvm", HOME_DIR);
    snprintf(CONF_DIR, sizeof CONF_DIR, "%s/start", HOME_DIR);
    snprintf(PREFIX_DIR, sizeof PREFIX_DIR, "%s", getenv("PREFIX") ? getenv("PREFIX") : "/usr");
    ensure_dir(CONF_DIR);      /* ~/.vvm 改成按需创建：只有用固件功能时才会建 */
    migrate_configs();

    vm_defaults(&vm);
    {
        char names[64][64];
        if (list_configs(names, 64) > 0) vm_load(names[0]);
    }

    if (argc > 1 && !strcmp(argv[1], "--firmware")) {
        int i, n = (int)(sizeof FW_FILES / sizeof FW_FILES[0]), bad = 0;
        char p[PATHBUF];
        printf("固件目录：%s/firmware\n", CONF_DIR);
        for (i = 0; i < n; i++) {
            fw_path(p, sizeof p, FW_FILES[i]);
            printf("  %-22s %s\n", FW_FILES[i], file_exists(p) ? "已有" : "缺失");
            if (!file_exists(p)) bad++;
        }
        printf("\n也可以直接用系统固件：pkg install qemu-system-aarch64-headless\n");
        return bad ? EXIT_FILE_MISS : EXIT_OK;
    }

    if (argc > 1) {
        if (!strcmp(argv[1], "--command") || !strcmp(argv[1], "--command-bg") ||
            !strcmp(argv[1], "--run") || !strcmp(argv[1], "--run-bg"))
            return cli_mode(argc, argv);
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            printf("vvm v%s —— Termux 上的 QEMU 虚拟机管理器\n\n", APP_VERSION);
            printf(
                   "  vvm                      打开菜单\n"
                   "  vvm --command [名字]     打印前台启动命令\n"
                   "  vvm --command-bg [名字]  打印后台启动命令\n"
                   "  vvm --run [名字]         前台启动\n"
                   "  vvm --firmware           查看固件状态和目录\n"
                   "  vvm --run-bg [名字]      后台启动\n"
                   "  vvm --version            只打印版本号\n\n"
                   "配置目录：%s\n"
                   "运行文件：<磁盘目录>/<磁盘名去后缀>/ 里（pid/日志/socket/截图）\n", CONF_DIR);
            return EXIT_OK;
        }
        if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
            printf("vvm v%s\n", APP_VERSION);
            return EXIT_OK;
        }
    }

    setup_reaper();
    ui_init();
    startup_env_check();
    home_screen();
    ui_done();
    return EXIT_OK;
}

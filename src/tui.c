#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "tui.h"
#include "fuzzy.h"
#include "pacman_source.h"
#include "aur_source.h"

/* Every render() calls get_description(), which shells out via popen()/
 * pclose() (fork+exec) to fetch a package description — meaning nearly
 * every keypress spawns and reaps a child process. That routinely
 * generates SIGCHLD, which can interrupt a blocking read() with
 * errno==EINTR. A plain read() that treats any non-1 return as "quit"
 * will therefore exit essentially at random whenever a keypress races
 * with a child process finishing. This wrapper retries on EINTR so an
 * interrupted syscall is never mistaken for EOF or a real error. */
static ssize_t read_retry(int fd, void *buf, size_t count) {
    for (;;) {
        ssize_t n = read(fd, buf, count);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

#define QUERY_MAX 256
#define KEY_ESC 0x1b
#define KEY_CTRL_C 0x03
#define KEY_BACKSPACE_1 0x7f
#define KEY_BACKSPACE_2 0x08
#define KEY_ENTER_1 '\r'
#define KEY_ENTER_2 '\n'
#define KEY_CTRL_N 0x0e
#define KEY_CTRL_P 0x10

/* Nordic palette, matching LazyOS branding */
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_BLUE    "\x1b[38;2;122;162;247m"   /* #7aa2f7 - core */
#define C_PURPLE  "\x1b[38;2;187;154;247m"   /* #bb9af7 - extra/prompt */
#define C_CYAN    "\x1b[38;2;125;207;255m"   /* #7dcfff - aur */
#define C_TEXT    "\x1b[38;2;192;202;245m"   /* #c0caf5 - default text */
#define C_SEL_BG  "\x1b[48;2;122;162;247m"   /* selected row background */
#define C_SEL_FG  "\x1b[38;2;26;30;46m"      /* #1a1e2e - selected row text */

static const char *repo_color(const char *repo) {
    if (strcmp(repo, "core") == 0) return C_BLUE;
    if (strcmp(repo, "extra") == 0) return C_PURPLE;
    if (strcmp(repo, "aur") == 0) return C_CYAN;
    return C_TEXT;
}

typedef struct {
    int score;
    size_t idx;
} Match;

static struct termios g_orig_termios;

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    printf("\x1b[?25h"); /* show cursor again */
    fflush(stdout);
}

static int enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return -1;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    /* NOTE: deliberately NOT touching c_oflag/OPOST here. Disabling output
     * processing means '\n' only moves down a line without returning to
     * column 0 (no automatic CR), which causes each printed line to drift
     * further right than the last. Leaving OPOST enabled keeps normal
     * NL->CRNL translation so our printf("...\n") calls behave sanely. */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    return 0;
}

static int get_terminal_size(int *out_rows, int *out_cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *out_rows = ws.ws_row;
        *out_cols = ws.ws_col;
        return 0;
    }
    *out_rows = 24;
    *out_cols = 80;
    return -1;
}

static int match_cmp(const void *a, const void *b) {
    const Match *ma = (const Match *)a;
    const Match *mb = (const Match *)b;
    if (mb->score != ma->score) return mb->score - ma->score;
    return 0;
}

/* Guards reads/writes of any Package's has_desc/fetching/description
 * fields, since background fetch threads write into them while the
 * main thread's render() reads them concurrently. */
static pthread_mutex_t g_desc_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Write end of a self-pipe: a background fetch thread writes one byte
 * here when it finishes, which wakes the main loop's select() so the
 * newly-arrived description shows up immediately instead of waiting
 * for the user's next keypress. */
static int g_wake_fd = -1;

typedef struct {
    Package *pkg;
} FetchJob;

static void *fetch_worker(void *arg) {
    FetchJob *job = (FetchJob *)arg;
    Package *pkg = job->pkg;
    free(job);

    char desc[DESC_MAX_LEN];
    int ok;
    if (strcmp(pkg->repo, "aur") == 0) {
        ok = aur_fetch_description(pkg->name, desc, sizeof(desc)) == 0;
    } else {
        ok = pacman_fetch_description(pkg->name, desc, sizeof(desc)) == 0;
    }

    pthread_mutex_lock(&g_desc_mutex);
    if (ok) {
        snprintf(pkg->description, DESC_MAX_LEN, "%s", desc);
    } else {
        snprintf(pkg->description, DESC_MAX_LEN, "(no description available)");
    }
    pkg->has_desc = 1;
    pkg->fetching = 0;
    pthread_mutex_unlock(&g_desc_mutex);

    if (g_wake_fd >= 0) {
        char byte = 1;
        ssize_t ignored = write(g_wake_fd, &byte, 1);
        (void)ignored; /* best-effort wakeup; a missed byte just means the
                         * next keypress or render picks up the result instead */
    }
    return NULL;
}

/* Never blocks. Returns the cached description if we already have it,
 * kicks off a background fetch (at most one in flight per package) and
 * returns a "Loading..." placeholder otherwise — the UI stays responsive
 * while pacman/curl run in another thread. */
static const char *get_description(Package *pkg) {
    pthread_mutex_lock(&g_desc_mutex);
    if (pkg->has_desc) {
        pthread_mutex_unlock(&g_desc_mutex);
        return pkg->description;
    }
    if (pkg->fetching) {
        pthread_mutex_unlock(&g_desc_mutex);
        return "Loading...";
    }
    pkg->fetching = 1;
    pthread_mutex_unlock(&g_desc_mutex);

    FetchJob *job = malloc(sizeof(FetchJob));
    if (!job) return "Loading...";
    job->pkg = pkg;

    pthread_t tid;
    if (pthread_create(&tid, NULL, fetch_worker, job) != 0) {
        free(job);
        pthread_mutex_lock(&g_desc_mutex);
        pkg->fetching = 0;
        pthread_mutex_unlock(&g_desc_mutex);
        return "Loading...";
    }
    pthread_detach(tid);
    return "Loading...";
}

/* Prints `text` in `style` (color codes), truncated/padded to exactly
 * `width` display columns, then resets. Assumes ASCII content (package
 * names/descriptions), so byte length == display width. */
static void print_cell(const char *style, const char *text, int width) {
    int len = (int)strlen(text);
    if (len > width) len = width;
    printf("%s%.*s", style, len, text);
    for (int i = len; i < width; i++) putchar(' ');
    printf(C_RESET);
}

/* Greedy word-wrap of `text` into up to `max_lines` lines of at most
 * `width` chars each, written into out_lines (each up to WRAP_LINE_MAX). */
#define WRAP_LINE_MAX 256
static int wrap_text(const char *text, int width, char out_lines[][WRAP_LINE_MAX], int max_lines) {
    int line_count = 0;
    const char *p = text;
    while (*p && line_count < max_lines) {
        int len = 0;
        int last_space = -1;
        while (p[len] && p[len] != '\n' && len < width) {
            if (p[len] == ' ') last_space = len;
            len++;
        }
        int cut = len;
        if (p[len] != '\0' && p[len] != '\n' && last_space > 0) cut = last_space;
        snprintf(out_lines[line_count], WRAP_LINE_MAX, "%.*s", cut, p);
        line_count++;
        p += cut;
        while (*p == ' ') p++;
    }
    return line_count;
}

static void render(PackageList *list, Match *matches, size_t match_count,
                    const char *query, size_t selected, int rows, int cols) {
    if (cols < 50) cols = 50;
    if (rows < 10) rows = 10;

    printf("\x1b[2J\x1b[H"); /* clear screen, cursor to top-left */

    int inner = cols - 2;              /* content width between the outer │ │ */
    int body_inner = inner - 1;        /* minus the middle vertical divider column */
    int list_w = body_inner * 2 / 5;
    int prev_w = body_inner - list_w;
    int body_rows = rows - 5;          /* top border, query, divider, bottom border, slack */
    if (body_rows < 3) body_rows = 3;

    /* top border with title */
    printf(C_BLUE "\xe2\x95\xad\xe2\x94\x80 " C_BOLD "lazy-search" C_RESET C_BLUE " ");
    for (int i = 0; i < inner - 14; i++) printf("\xe2\x94\x80");
    printf("\xe2\x95\xae" C_RESET "\n");

    /* query line: "│" + cell(inner) + "│" */
    char query_cell[300];
    snprintf(query_cell, sizeof(query_cell), " > %s", query);
    printf(C_BLUE "\xe2\x94\x82" C_RESET);
    print_cell(C_PURPLE C_BOLD, query_cell, inner);
    printf(C_BLUE "\xe2\x94\x82" C_RESET "\n");

    /* divider between query line and the two-pane body: "├" + dash(list_w) + "┬" + dash(prev_w) + "┤" */
    printf(C_BLUE "\xe2\x94\x9c");
    for (int i = 0; i < list_w; i++) printf("\xe2\x94\x80");
    printf("\xe2\x94\xac");
    for (int i = 0; i < prev_w; i++) printf("\xe2\x94\x80");
    printf("\xe2\x94\xa4" C_RESET "\n");

    /* build the wrapped preview text for the currently selected package */
    char wrap_lines[64][WRAP_LINE_MAX];
    int wrap_count = 0;
    char header_line[160] = "";
    if (match_count > 0) {
        Package *sel_pkg = &list->items[matches[selected].idx];
        const char *desc = get_description(sel_pkg);
        snprintf(header_line, sizeof(header_line), "%s/%s", sel_pkg->repo, sel_pkg->name);
        wrap_count = wrap_text(desc, prev_w - 1, wrap_lines, 64);
    }

    size_t start = 0;
    if (selected >= (size_t)body_rows) start = selected - body_rows + 1;

    for (int row = 0; row < body_rows; row++) {
        size_t i = start + (size_t)row;

        /* left pane: "│" + cell(list_w) */
        printf(C_BLUE "\xe2\x94\x82" C_RESET);
        if (i < match_count) {
            Package *pkg = &list->items[matches[i].idx];
            char cell[160];
            snprintf(cell, sizeof(cell), " %-10s %s", pkg->repo, pkg->name);
            const char *style = (i == selected) ? C_SEL_BG C_SEL_FG C_BOLD : repo_color(pkg->repo);
            print_cell(style, cell, list_w);
        } else {
            print_cell("", "", list_w);
        }

        /* middle divider + right pane: "│" + cell(prev_w) */
        printf(C_BLUE "\xe2\x94\x82" C_RESET);
        if (row == 0 && header_line[0] != '\0') {
            char cell[168];
            snprintf(cell, sizeof(cell), " %s", header_line);
            print_cell(C_CYAN C_BOLD, cell, prev_w);
        } else {
            int wi = row - 2; /* leave a blank line after the header */
            if (wi >= 0 && wi < wrap_count) {
                char cell[WRAP_LINE_MAX + 2];
                snprintf(cell, sizeof(cell), " %s", wrap_lines[wi]);
                print_cell(C_DIM C_TEXT, cell, prev_w);
            } else {
                print_cell("", "", prev_w);
            }
        }

        printf(C_BLUE "\xe2\x94\x82" C_RESET "\n");
    }

    /* bottom border with match count */
    char count_label[64];
    snprintf(count_label, sizeof(count_label), " %zu/%zu ", match_count, list->count);
    printf(C_BLUE "\xe2\x95\xb0\xe2\x94\x80" C_RESET C_DIM C_CYAN "%s" C_RESET C_BLUE, count_label);
    int used = 2 + (int)strlen(count_label);
    for (int i = 0; i < inner - used + 1; i++) printf("\xe2\x94\x80");
    printf("\xe2\x95\xaf" C_RESET "\n");
    fflush(stdout);
}

int tui_run(PackageList *list, char *out_selection, size_t out_size) {
    if (enable_raw_mode() != 0) return -1;
    printf("\x1b[?25l"); /* hide cursor while browsing */

    int wake_pipe[2] = {-1, -1};
    if (pipe(wake_pipe) == 0) {
        fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
        g_wake_fd = wake_pipe[1];
    }
    /* if the pipe failed to create, g_wake_fd stays -1 and background
     * fetches simply won't wake the UI early — it'll still pick up the
     * result on the next real keypress, just not instantly */

    char query[QUERY_MAX] = {0};
    size_t query_len = 0;
    size_t selected = 0;
    int result = -1;

    Match *matches = malloc(list->count * sizeof(Match));
    if (!matches) {
        g_wake_fd = -1;
        if (wake_pipe[0] >= 0) close(wake_pipe[0]);
        if (wake_pipe[1] >= 0) close(wake_pipe[1]);
        restore_terminal();
        return -1;
    }
    size_t match_count = 0;

    /* rebuild the match list whenever the query changes */
    #define RESCORE() do { \
        match_count = 0; \
        for (size_t i = 0; i < list->count; i++) { \
            int s = fuzzy_score(query, list->items[i].name); \
            if (s >= 0) { matches[match_count].score = s; matches[match_count].idx = i; match_count++; } \
        } \
        qsort(matches, match_count, sizeof(Match), match_cmp); \
        if (selected >= match_count) selected = match_count > 0 ? match_count - 1 : 0; \
    } while (0)

    RESCORE();

    for (;;) {
        int rows, cols;
        get_terminal_size(&rows, &cols);
        render(list, matches, match_count, query, selected, rows, cols);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int maxfd = STDIN_FILENO;
        if (wake_pipe[0] >= 0) {
            FD_SET(wake_pipe[0], &fds);
            if (wake_pipe[0] > maxfd) maxfd = wake_pipe[0];
        }

        int sr = select(maxfd + 1, &fds, NULL, NULL, NULL);
        if (sr < 0) {
            if (errno == EINTR) continue; /* signal (e.g. from a fetch thread's child) — just re-poll */
            break;
        }
        if (wake_pipe[0] >= 0 && FD_ISSET(wake_pipe[0], &fds)) {
            /* a background description fetch finished: drain the pipe
             * and loop back to re-render with the fresh result, without
             * consuming this as a keypress */
            char drain[64];
            while (read(wake_pipe[0], drain, sizeof(drain)) > 0) { }
            continue;
        }

        char c;
        if (read_retry(STDIN_FILENO, &c, 1) != 1) break;

        if (c == KEY_ESC) {
            /* Could be a lone Esc, or the start of an arrow-key escape
             * sequence: CSI (\x1b [ A/B) or SS3 (\x1b O A/B). Read all
             * available follow-up bytes in ONE call (rather than two
             * separate reads, which could race if the terminal ever
             * delivers the bytes in separate chunks) so we never
             * mistake a slightly-delayed arrow sequence for a lone Esc. */
            struct termios cur;
            tcgetattr(STDIN_FILENO, &cur);
            struct termios peek = cur;
            peek.c_cc[VMIN] = 0;
            peek.c_cc[VTIME] = 5; /* 500ms, generous on purpose */
            tcsetattr(STDIN_FILENO, TCSANOW, &peek);

            char seq[8] = {0};
            ssize_t n = read_retry(STDIN_FILENO, seq, sizeof(seq));
            tcsetattr(STDIN_FILENO, TCSANOW, &cur);

            if (n <= 0) {
                /* nothing followed Esc at all: genuinely standalone Esc.
                 * If there's an active query, clear it first (matches
                 * fzf convention) rather than quitting outright; only
                 * quit when the query is already empty. */
                if (query_len > 0) {
                    query_len = 0;
                    query[0] = '\0';
                    selected = 0;
                    RESCORE();
                    continue;
                }
                result = -1;
                break;
            }
            if (n >= 2 && (seq[0] == '[' || seq[0] == 'O')) {
                if (seq[1] == 'A') { /* up arrow */
                    if (selected > 0) selected--;
                } else if (seq[1] == 'B') { /* down arrow */
                    if (selected + 1 < match_count) selected++;
                }
                /* any other final byte (left/right/Fn/etc.): ignore */
            }
            /* Esc followed by something we don't recognize (Alt+key,
             * unsupported/partial sequence, etc.): just ignore it and
             * keep going — never quit on an unrecognized sequence. */
            continue;
        } else if (c == KEY_CTRL_C) {
            result = -1;
            break;
        } else if (c == KEY_ENTER_1 || c == KEY_ENTER_2) {
            if (match_count > 0) {
                Package *pkg = &list->items[matches[selected].idx];
                snprintf(out_selection, out_size, "%s/%s", pkg->repo, pkg->name);
                result = 0;
            }
            break;
        } else if (c == KEY_BACKSPACE_1 || c == KEY_BACKSPACE_2) {
            if (query_len > 0) {
                query[--query_len] = '\0';
                RESCORE();
            }
        } else if (c == KEY_CTRL_N) {
            if (selected + 1 < match_count) selected++;
        } else if (c == KEY_CTRL_P) {
            if (selected > 0) selected--;
        } else if (isprint((unsigned char)c) && query_len + 1 < QUERY_MAX) {
            query[query_len++] = c;
            query[query_len] = '\0';
            selected = 0;
            RESCORE();
        }
    }

    #undef RESCORE
    free(matches);
    g_wake_fd = -1;
    if (wake_pipe[0] >= 0) close(wake_pipe[0]);
    if (wake_pipe[1] >= 0) close(wake_pipe[1]);
    printf("\x1b[2J\x1b[H");
    restore_terminal();
    return result;
}

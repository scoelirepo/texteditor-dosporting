/*
 * COELIText - tiny ncurses-based text editor
 *
 * main.c
 * Minimal text editor demonstrating basic editing operations
 * (insert, delete, split lines, merge lines) using a fixed-size
 * in-memory buffer. Designed for simple editing tasks and as
 * an educational example.
 *
 * Usage: ctx_ncurses <file>
 *
 * Notes:
 * - Fixed-width buffer: MAX_LINES x COLS_PER_LINE characters.
 * - Lines are stored flat in a single allocated block `j`.
 * - The UI uses ncurses; screen layout reserves a header and footer.
 */

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define COLS_PER_LINE 80
#define MAX_LINES     5000

#define HEADER_HEIGHT    1
#define FOOTER_HEIGHT    2
#define CONTENT_HEIGHT   (LINES - HEADER_HEIGHT - FOOTER_HEIGHT)

static unsigned char *j;      // text buffer (flat: line*COLS_PER_LINE + col)
#define BUF(line, col) j[(line) * COLS_PER_LINE + (col)]

static unsigned long fcon = 0;   // number of lines currently in the buffer
static unsigned long cxp  = 0;   // index of first visible line in the file
static int lin = 0;              // cursor row on the screen (0..)
static int col = 1;              // cursor column (1..COLS_PER_LINE)
static int swin = 0;             // insert/overwrite mode toggle (INS)
static int indn = 0;             // auto-indent flag (currently unused)

static char filename[256];

void mwin(int nr, const char *mess);
void guida(void);
int  save_file(const char *fln, unsigned nc);
unsigned long load_file(const char *flnm, unsigned long mlin);
void refresh_screen(void);

/* --- utility functions --- */

// Helper: Find the index of the last non-space character in a line
// Returns -1 if the entire line is spaces, or 0..COLS_PER_LINE-1
static int find_last_content_col(unsigned long line) {
    for (int col = COLS_PER_LINE - 1; col >= 0; col--) {
        if (BUF(line, col) != ' ') {
            return col;
        }
    }
    return -1;
}

// Wrapper that checks if we can safely add a line
static int try_add_line(unsigned long line) {
    if (fcon >= MAX_LINES - 1) {
        mwin(1, "    MEMORY EXHAUSTED           ");
        return 0;
    }
    return 1;
}

/* --- line management functions (line commands) --- */

// insert a new empty line at position `line` (shifts following lines down)
int insert_blank_line(unsigned long line) {
    if (fcon >= MAX_LINES - 1) return 0;
    memmove(&BUF(line + 1, 0),
            &BUF(line, 0),
            (fcon - line) * COLS_PER_LINE);
    for (int y = 0; y < COLS_PER_LINE; y++)
        BUF(line, y) = ' ';
    fcon++;
    return 1;
}

// split the current line at column `col` (1-based). The remainder is moved to the new line
int split_line_at(unsigned long line, int col) {
    if (fcon >= MAX_LINES - 1) return 0;

    memmove(&BUF(line + 1, 0),
            &BUF(line, 0),
            (fcon - line) * COLS_PER_LINE);

    int src = col - 1;
    int dst = 0;
    while (src < COLS_PER_LINE && dst < COLS_PER_LINE) {
        BUF(line + 1, dst++) = BUF(line, src);
        BUF(line, src++) = ' ';
    }
    while (dst < COLS_PER_LINE)
        BUF(line + 1, dst++) = ' ';

    fcon++;
    return 1;
}

// delete the entire line `line` (mapped to Ctrl+Backspace)
void delete_line(unsigned long line) {
    if (fcon <= 1) {
        for (int y = 0; y < COLS_PER_LINE; y++)
            BUF(0, y) = ' ';
        fcon = 1;
        return;
    }
    memmove(&BUF(line, 0),
            &BUF(line + 1, 0),
            (fcon - line - 1) * COLS_PER_LINE);
    fcon--;
}

// merge the line `line` with the previous line (mapped to Alt+Backspace)
void merge_with_previous(unsigned long line) {
    if (line == 0) return;
    unsigned long prev = line - 1;

    int last_col = find_last_content_col(prev);
    int start = last_col + 1;
    
    if (start >= COLS_PER_LINE) {
        // Previous line is full; cannot merge without data loss
        mwin(1, "  PREVIOUS LINE IS FULL       ");
        return;
    }

    int src = 0;
    while (start < COLS_PER_LINE && src < COLS_PER_LINE) {
        BUF(prev, start++) = BUF(line, src++);
    }

    delete_line(line);
}

/* --- screen --- */

void refresh_screen(void) {
    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);
    int vis_lines = CONTENT_HEIGHT;

    attron(COLOR_PAIR(1));
    mvhline(0, 0, ' ', maxx);
    mvprintw(0, 0,
             "* COELIText : %-12s  COL %2d  LIN %-4ld   F1 save - ESC exit - F10 help",
             filename, col, cxp + lin + 1);
    attroff(COLOR_PAIR(1));

    attron(COLOR_PAIR(3));
    for (int scr_line = 0; scr_line < vis_lines; scr_line++) {
        unsigned long file_line = cxp + scr_line;
        move(scr_line + HEADER_HEIGHT, 0);
        if (file_line < fcon) {
            for (int c = 0; c < COLS_PER_LINE; c++)
                addch(BUF(file_line, c));
        } else {
            hline(' ', COLS_PER_LINE);
        }
    }
    attroff(COLOR_PAIR(3));

    if (fcon < (unsigned long)vis_lines) {
        move(fcon + HEADER_HEIGHT, 0);
        attron(COLOR_PAIR(2));
        const char *end = ">>>>>>>>>>>>>>>>>>>>  END  <<<<<<<<<<<<<<<<<<<<";
        int len = (int)strlen(end);
        int start = (COLS_PER_LINE - len) / 2;
        hline(' ', COLS_PER_LINE);
        mvaddnstr(fcon + HEADER_HEIGHT, start, end, len);
        attroff(COLOR_PAIR(2));
    }

    move(lin + HEADER_HEIGHT, col - 1);
    refresh();
}

/* --- file I/O --- */

int save_file(const char *fln, unsigned nc) {
    FILE *fp = fopen(fln, "w");
    if (!fp) return 1;

    for (unsigned long line = 0; line < nc; line++) {
        int last_col = find_last_content_col(line);
        
        if (last_col >= 0) {
            // Write from column 0 to last_col (inclusive)
            for (int col = 0; col <= last_col; col++) {
                fputc(BUF(line, col), fp);
            }
        }
        // Write newline
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

unsigned long load_file(const char *flnm, unsigned long mlin) {
    FILE *fp = fopen(flnm, "r");
    if (!fp) {
        mwin(1, "    CANNOT OPEN FILE           ");
        getch();
        return (unsigned long)-1;
    }

    unsigned long fline = 0;
    int clo = 0;
    int c;

    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') {
            while (clo < COLS_PER_LINE)
                BUF(fline, clo++) = ' ';
            clo = 0;
            fline++;
        } else if (c == '\t') {
            int y;
            for (y = 0; y <= 8 && clo < COLS_PER_LINE && (y == 0 || clo % 8); y++)
                BUF(fline, clo++) = ' ';
        } else {
            if (clo < COLS_PER_LINE)
                BUF(fline, clo++) = (unsigned char)c;
        }

        if (clo >= COLS_PER_LINE) {
            clo = 0;
            fline++;
            int next = fgetc(fp);
            if (next != '\n' && next != EOF)
                ungetc(next, fp);
        }

        if (fline > mlin) {
            mwin(1, "        MEMORY EXHAUSTED       ");
            getch();
            fclose(fp);
            return fline;
        }
    }

    if (clo != 0) {
        while (clo < COLS_PER_LINE)
            BUF(fline, clo++) = ' ';
        fline++;
    }

    fclose(fp);
    return fline;
}

/* --- windows --- */

void mwin(int nr, const char *mess) {
    int h = 7;
    int w = 42;
    int y = (LINES - h) / 2;
    int x = (COLS  - w) / 2;

    WINDOW *win = newwin(h, w, y, x);
    box(win, 0, 0);
    wattron(win, COLOR_PAIR(2));

    for (int i = 0; i < nr; i++) {
        mvwprintw(win, 2 + i, 2, "%.*s", 30, mess + 30 * i);
    }

    wattroff(win, COLOR_PAIR(2));
    wrefresh(win);
    getch();
    delwin(win);
    refresh_screen();
}

void guida(void) {
    const char *txt[] = {
        "Editing commands:",
        "Enter with INS inserts a line;",
        "Ctrl+L splits the line at the current column;",
        "Ctrl+Y deletes line; Alt+Y merges with previous;",
        "Ctrl+arrow (to add) fast movement;",
        "F2 auto indent ON/OFF (flag indn);",
        "F5 merge file; F6 block; F7 copy; F8 move."
    };
    int lines = sizeof(txt)/sizeof(txt[0]);
    int h = lines + 4;
    int w = 70;
    int y = (LINES - h) / 2;
    int x = (COLS  - w) / 2;

    WINDOW *win = newwin(h, w, y, x);
    box(win, 0, 0);
    wattron(win, COLOR_PAIR(2));
    for (int i = 0; i < lines; i++)
        mvwprintw(win, 2 + i, 2, "%s", txt[i]);
    wattroff(win, COLOR_PAIR(2));
    wrefresh(win);
    getch();
    delwin(win);
    refresh_screen();
}

/* --- main --- */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: ctx_ncurses file\n");
        return 1;
    }
    strncpy(filename, argv[1], sizeof(filename)-1);
    filename[sizeof(filename)-1] = '\0';

    j = malloc(MAX_LINES * COLS_PER_LINE);
    if (!j) {
        fprintf(stderr, "insufficient memory\n");
        return 1;
    }
    memset(j, ' ', MAX_LINES * COLS_PER_LINE);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fcon = 1;
    } else {
        fclose(fp);
        fcon = load_file(filename, MAX_LINES - 1);
        if (fcon == (unsigned long)-1) {
            free(j);
            return 1;
        }
        if (fcon == 0) fcon = 1;
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_BLACK, COLOR_CYAN);
        init_pair(2, COLOR_WHITE, COLOR_BLUE);
        init_pair(3, COLOR_WHITE, COLOR_BLACK);
    }

    cxp = 0;
    lin = 0;
    col = 1;

    refresh_screen();

    for (;;) {
        int ch = getch();

            /* handle Alt+Y as ESC followed by 'y' */
        if (ch == 27) {
            nodelay(stdscr, TRUE);
            int next = getch();
            nodelay(stdscr, FALSE);

            if (next == 'y' || next == 'Y') {
                unsigned long line = cxp + lin;
                merge_with_previous(line);
                if (lin > 0) lin--;
                else if (cxp > 0) cxp--;
                refresh_screen();
                continue;
            }

            /* plain ESC: confirm exit (accept 'y'/'Y' or 's'/'S') */
            mwin(1, "     CONFIRM EXIT? (y/n)       ");
            int c = getch();
            if (c == 's' || c == 'S' || c == 'y' || c == 'Y') {
                break;
            }
            refresh_screen();
            continue;
        }

        switch (ch) {
        case KEY_F(1):
            if (save_file(filename, (unsigned)fcon) != 0) {
                mwin(1, "   FILE ACCESS ERROR          ");
            }
            break;

        case KEY_F(10):
            guida();
            break;

        case KEY_UP:
            if (lin > 0) lin--;
            else if (cxp > 0) cxp--;
            break;

        case KEY_DOWN:
            if ((unsigned long)(cxp + lin + 1) < fcon &&
                lin < (CONTENT_HEIGHT - 1)) {
                lin++;
            } else if ((unsigned long)(cxp + CONTENT_HEIGHT) < fcon) {
                cxp++;
            }
            break;

        case KEY_LEFT:
            if (col > 1) col--;
            else if (lin > 0) { lin--; col = COLS_PER_LINE; }
            break;

        case KEY_RIGHT:
            if (col < COLS_PER_LINE) col++;
            else if ((unsigned long)(cxp + lin + 1) < fcon) {
                lin++; col = 1;
            }
            break;

        case KEY_HOME:
            col = 1;
            break;

        case KEY_END:
            col = COLS_PER_LINE;
            break;

        case KEY_IC: // INS
            swin = !swin;
            break;

        case KEY_BACKSPACE:
        case 127:
            if (col > 1) {
                unsigned long line = cxp + lin;
                if (swin) {
                    for (int y = col - 1; y < COLS_PER_LINE - 1; y++)
                        BUF(line, y - 1) = BUF(line, y);
                    BUF(line, COLS_PER_LINE - 1) = ' ';
                } else {
                    BUF(line, col - 2) = ' ';
                }
                col--;
            }
            break;

        case '\n': { // Enter: insert an empty line below
            unsigned long line = cxp + lin;
            if (!insert_blank_line(line + 1)) {
                mwin(2, "  CANNOT ADD LINES           \0"
                        "        MEMORY EXHAUSTED     \0");
                break;
            }
            if (lin < CONTENT_HEIGHT - 1) lin++;
            else cxp++;
            col = 1;
        }
            break;

        case 12: { // Ctrl+L = "Ctrl+Enter": split the line at the current column
            unsigned long line = cxp + lin;
            if (!split_line_at(line, col)) {
                mwin(2, "  CANNOT ADD LINES           \0"
                        "        MEMORY EXHAUSTED     \0");
                break;
            }
            if (lin < CONTENT_HEIGHT - 1) lin++;
            else cxp++;
            col = 1;
        }
            break;

        case 25: { // Ctrl+Y = "Ctrl+Backspace": delete the current line
            unsigned long line = cxp + lin;
            delete_line(line);
            if (cxp + lin >= fcon && lin > 0)
                lin--;
            if (cxp > 0 && cxp + CONTENT_HEIGHT > fcon)
                cxp--;
        }
            break;

        default:
            if (ch >= 32 && ch <= 126) {
                unsigned long line = cxp + lin;
                if (swin) {
                    for (int y = COLS_PER_LINE - 1; y >= col; y--)
                        BUF(line, y) = BUF(line, y - 1);
                }
                BUF(line, col - 1) = (unsigned char)ch;
                if (line >= fcon) fcon = line + 1;
                if (col < COLS_PER_LINE) col++;
            }
            break;
        }

        refresh_screen();
    }

    endwin();
    free(j);
    return 0;
}

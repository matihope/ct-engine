/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

typedef struct erow {
    int size;
    char *chars;
} erow;

struct editorConfig {
    int cx, cy;  // cursor x and y
    int screenrows;
    int screencols;
    int numrows;
    erow row;
    struct termios orig_termios;
};
struct editorConfig E;

/*** defines ***/

#define EDITOR_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum edtiorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,  // 1001
    ARROW_UP,  // 1002
    ARROW_DOWN, // 1003
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN, 
    DEL_KEY, 
};

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode(){
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | IXON | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if(nread == -1 && errno != EAGAIN) die("read");
        
    }

    if (c == '\x1b') {
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if(seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9'){
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~'){
                    switch (seq[1]){
                        case '1':
                        case '7': return HOME_KEY;
                        case '4':
                        case '8': return END_KEY;
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                    }
                }
                
            }
            
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf) - 1) {
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;
    if(ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDIN_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** file i/o ***/

void editorOpen(char *filename){
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");
    
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    if (linelen != -1){
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            linelen--;

        E.row.size = linelen;
        E.row.chars = malloc(linelen + 1);
        memcpy(E.row.chars, line, linelen);
        E.row.chars[linelen] = '\0';
        E.numrows = 1;
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);  // create space for the string

    if(new == NULL) return;
    memcpy(&new[ab->len], s, len);  // copy new string to the buffer

    // update variables
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < E.screenrows; ++y){
        if(y >= E.numrows) {
            if(y == E.screenrows / 3) {
                // welcome message
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "CEditor -- version %s", EDITOR_VERSION);
                if(welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if(padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else {
                // regular draw
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row.size;
            if(len > E.screencols)
                len = E.screencols;
            abAppend(ab, E.row.chars, len);
        }



        abAppend(ab, "\x1b[K", 3);
        if(y < E.screenrows - 1)
            abAppend(ab, "\r\n", 2);
    }    
}

void editorRefreshScreen(){
    struct abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6); // hide the cursor
    // abAppend(&ab, "\x1b[2J", 4);  // clear the screen
    abAppend(&ab, "\x1b[H", 3);  // set position to the beginning

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));
    
    abAppend(&ab, "\x1b[?25h", 6); // show the cursor
    
    write(STDIN_FILENO, ab.b, ab.len);  // write the buffer to stdin
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key){
    switch(key) {
        case ARROW_LEFT:
            if(E.cx > 0)
                E.cx--;
            break;
        case ARROW_RIGHT:
            if(E.cx < E.screencols)
            E.cx++;
            break;
        case ARROW_UP:
            if(E.cy > 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.screenrows)
                E.cy++;
            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch(c) {
        case CTRL_KEY('q'):
            write(STDIN_FILENO, "\x1b[2J", 4);
            write(STDIN_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
            E.cy = 0;
            break;
        case PAGE_DOWN:
            // {  
            //     int times = E.screenrows;
            //     while(times--)
            //         editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            // }
            E.cy = E.screenrows - 1;
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_RIGHT:
        case ARROW_LEFT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]){
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
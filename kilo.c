#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/ioctl.h>

#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <fcntl.h>

#define VERSION "0.0.1"
#define KILO_EDITOR "Kilo editor - version " VERSION

#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_SIZE 8

#define STATUSBAR_FEATURE 1
#define MESSAGE_FEATURE 1

enum editorKey {
  BS = 8, // CTRL_H
  ENTER = 13,
  CTRL_Q = 17,
  ESCAPE = 27,
  BACKSPACE = 127,
  ARROW_UP = 1000,
  ARROW_LEFT,
  ARROW_DOWN,
  ARROW_RIGHT,
  INSERT_KEY,
  DELETE_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow_s erow_t;

struct erow_s {
  int size;
  char *chars;
  int rsize;
  char *render;
};

#define EROW_INIT {0, NULL, 0, NULL}

/* data / global variables / global state / whatever */

static int err; // for termios specific functions
struct termios canonical;
int rows, cols;
int rowoff, coloff; int cx, cy; // navigation
int rx; // handle wide characters
int numrows;
erow_t *row_p;
char *fname;
int dirty, quit;
#if MESSAGE_FEATURE
char status_msg[80];
time_t message_time;
#endif


/* terminal */

void editorClearScreen(void);

void die(const char *s) {
  editorClearScreen();
  
  perror(s);
  exit(1);
}

void disableRawMode() {
  err = tcsetattr(STDIN_FILENO, TCSAFLUSH, &canonical);
  if (err == -1) die("tcsetattr");
}

void enableRawMode() {
  err = tcgetattr(STDIN_FILENO, &canonical);
  if (err == -1) die("tcgetattr");
  atexit(disableRawMode);
  
  struct termios raw = canonical;
  raw.c_iflag &= ~(ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  
  err = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  if (err == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char seq[2];
  char c;
  
  while (1) {
    nread = read(STDIN_FILENO, &c, 1);
    if (nread == -1) die("read");
    if (nread == 1) break;
  }
#define ESC '\x1b'
  if (c != ESC) return c;
  
  // TODO: what?!
  err = read(STDIN_FILENO, seq, 1);
  err += read(STDIN_FILENO, seq + 1, 1);
  if (err != 2) return ESC;
  
  if (seq[0] == '[') switch (seq[1]) {
    case 'A': return ARROW_UP;
    case 'B': return ARROW_DOWN;
    case 'C': return ARROW_RIGHT;
    case 'D': return ARROW_LEFT;
    case 'H': return HOME_KEY;
    case 'F': return END_KEY;
  }
  if (seq[0] == 'O') switch (seq[1]) {
    case 'H': return HOME_KEY;
    case 'F': return END_KEY;
  }
  
  err = read(STDIN_FILENO, seq + 2, 1);
  if (err != 1 || seq[0] != '[') return ESC;
  
  if (seq[2] == '~') switch (seq[1]) {
    case '1': return HOME_KEY;
    case '2': return INSERT_KEY;
    case '3': return DELETE_KEY;
    case '4': return END_KEY;
    case '5': return PAGE_UP;
    case '6': return PAGE_DOWN;
    case '7': return HOME_KEY;
    case '8': return END_KEY;
  }
  return ESC;
#undef ESC
}

void getCursorPosition() {
  char buf[32];
  unsigned int i = 0;
  
#define ESC "\x1b"
#define REQUEST ESC"[999B" ESC"[999C" ESC"[6n"
#define REQUEST_LENGTH 16
  err = write(STDOUT_FILENO, REQUEST, REQUEST_LENGTH);
#undef REQUEST
#undef ESC
  if (err != REQUEST_LENGTH) die("getCursorPosition");
#undef REQUEST_LENGTH
  
  while (i < sizeof(buf) - 1) {
    err = read(STDIN_FILENO, &buf[i], 1);
    if (err != 1 || buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';
  
#define ESC '\x1b'
  if (buf[0] != ESC || buf[1] != '[') die("CPR");
#undef ESC
  err = sscanf(&buf[2], "%d;%d", &rows, &cols);
  if (err != 2) die("sscanf");
}

void getWindowSize() {
  struct winsize _ws;
  
  err = ioctl(STDOUT_FILENO, TIOCGWINSZ, &_ws);
  if (err == 0) {
    rows = _ws.ws_row;
    cols = _ws.ws_col;
    return;
  }
  
  getCursorPosition(); // if cmd_get_window_size failed
}

#if MESSAGE_FEATURE

void setStatusMsg(const char *fmt, ...) {
  va_list ap;
  time(&message_time);
  
  va_start(ap, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
  va_end(ap);
}
#endif

void editorQuit() {
  quit--;
  if (!dirty || quit == 0) goto exit;
#if MESSAGE_FEATURE
  setStatusMsg("File has unsaved changes. "
    "Press Ctrl-Q one more time to quit.");
#endif
  return;
  
exit:
  editorClearScreen();
  exit(0);
}

/* row operations */

size_t rtrim(char *s, size_t length) {
  while (length > 0) {
    char c = s[length - 1];
    if (c == '\n' || c == '\r') length--; else break;
  }
  return length;
}

void editorRenderRow(erow_t *row) {
  int i, j, tabs, length;
  
  i = tabs = 0;
  for (; i < row->size; i++) {
    if (row->chars[i] == '\t') { tabs++; }
  }
  length = row->size + tabs * (TAB_SIZE - 1) + 1;
  
  // TODO: switch to realloc
  free(row->render);
  row->render = malloc(length);
  
  i = j = 0;
  for (; i < row->size; i++) switch (row->chars[i]) {
  case '\t':
    row->render[j++] = ' ';
    while (j % TAB_SIZE != 0) { row->render[j++] = ' '; }
    break;
  default:
    row->render[j++] = row->chars[i];
  }
  row->render[j] = '\0';
  row->rsize = j;
}

void editorInsertRow(int pos, char *s, size_t length) {
  erow_t *row;
  size_t newlen, tail_len;
  
  newlen = sizeof(erow_t) * (numrows + 1);
  tail_len = sizeof(erow_t) * (numrows - pos);
  
  row_p = realloc(row_p, newlen);
  row = row_p + pos;
  memmove(row + 1, row, tail_len);
  
  row->size = length;
  // TODO: switch to strdup
  row->chars = malloc(length + 1);
  memcpy(row->chars, s, length);
  row->chars[length] = '\0';
  
  editorRenderRow(row);
  numrows++;
  dirty = 1;
}

// TODO: convert to macros
void editorAppendRow(char *s, size_t length) {
  erow_t row = EROW_INIT;
  size_t newlen = sizeof(erow_t) * (numrows + 1);
  
  length = rtrim(s, length);
  
  row.chars = malloc(length + 1);
  memcpy(row.chars, s, length);
  row.chars[length] = '\0';
  row.size = length;
  
  row_p = realloc(row_p, newlen);
  row_p[numrows] = row;
  
  // THINK: render rows one by one or all at once?
  editorRenderRow(row_p + numrows);
  numrows++;
  dirty = 1;
}

void editorDeleteRow(int pos) {
  erow_t *tail = row_p + pos;
  int count;
  
  free(tail->render);
  free(tail->chars);
  
  count = sizeof(erow_t) * (numrows - pos);
  memmove(tail, tail + 1, count); // bug is here?
  numrows--;
  dirty = 1;
}

void editorAppendString(erow_t *r, char *s, size_t len) {
  int newlen = r->size + len;
  
  r->chars = realloc(r->chars, newlen + 1);
  strcpy(r->chars + r->size, s);
  r->size = newlen;
  
  editorRenderRow(r);
  dirty = 1;
}

/* editor operations */

void editorInsertChar(int c) {
  erow_t *row = row_p + cy;
  char *tail;
  
  row->chars = realloc(row->chars, row->size + 2);
  tail = row->chars + cx;
  memmove(tail + 1, tail, row->size - cx + 1);
  *tail = c;
  row->size++;
  
  editorRenderRow(row);
  cx++;
  dirty = 1;
}

void editorDeleteChar() {
  erow_t *row = row_p + cy;
  char *tail;
  int end, length;
  
  end = row->size;
  if (cx == end) goto end_of_line;
  
  tail = row->chars + cx;
  length = end - cx + 1;
  memmove(tail, tail + 1, length);
  row->size--;
  
  editorRenderRow(row);
  dirty = 1;
  return;
  
end_of_line:
  if (cy == numrows - 1) return;
  
  tail = (row + 1)->chars;
  length = (row + 1)->size;
  editorAppendString(row, tail, length);
  editorDeleteRow(cy + 1);
}

char *editorPrepareText(int *buflen) {
  char *buf, *p;
  int j, length = 0;
  
  for (j = 0; j < numrows; j++) {
    length += row_p[j].size + 1;
  }
  
  p = buf = malloc(length);
  if (!buf) die("prepareText");
  *buflen = length;
  
  for (j = 0; j < numrows; j++) {
    length = row_p[j].size;
    memcpy(p, row_p[j].chars, length);
    p += length;
    *p++ = '\n';
  }
  return buf;
}


/* file i/o */

void editorOpen(char *filename) {
  free(fname);
  fname = strdup(filename);
  
  FILE *fp = fopen(fname, "r");
  if (!fp) die("fopen");
  
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  
  while (1) {
    length = getline(&line, &capacity, fp);
    if (length == -1) break;
    editorAppendRow(line, length);
  }
  
  free(line);
  fclose(fp);
  dirty = 0;
}

void editorSave() {
  int length, fd, err;
  char *buf;
  
  if (fname == NULL) return;
  
  buf = editorPrepareText(&length);
  fd = open(fname, O_RDWR | O_CREAT, 0644);
  if (fd == -1) goto error;
/*
  By truncating the file ourselves, we are making the
  whole overwriting operation a little bit safer in case
  the ftruncate() call succeeds but the write() call
  fails. In that case, the file would still contain most
  of the data it had before.
  
  TODO ftruncate call after write is more robust?
 */
  err = ftruncate(fd, length);
  if (err == -1) goto error;
  
  err = write(fd, buf, length);
  if (err < length) goto error;
  
  err = fsync(fd);
  if (err == -1) goto error;
  
  close(fd);
  free(buf);
  dirty = 0;

#if MESSAGE_FEATURE
  setStatusMsg("%d bytes written to disk", length);
#endif
  return;
  
error:
  die("editorSave");
}

/* append buffer */

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, len + ab->len);
  
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab -> b = new;
  ab -> len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/* output */

void editorUpdateRx() {
  char *chars = row_p[cy].chars;
  int i = rx = 0;
  
  for (; i < cx; i++) {
    if (chars[i] == '\t') {
      rx += (TAB_SIZE - 1) - (rx % TAB_SIZE);
    }
    rx++;
  }
}

void editorScroll() {
  int diff, below_file, below_screen;
  
  if (cy >= numrows) return;
  if (cy < rowoff) { rowoff = cy; }
  
  diff = numrows - rows;
  below_file = (diff > 0 && rowoff > diff);
  if (below_file) { rowoff = diff; }
  
  diff = cy - rows + 1;
  below_screen = (rowoff < diff);
  if (below_screen) { rowoff = diff; }
  
  editorUpdateRx();
  if (rx < coloff) { coloff = rx; }
  if (rx >= coloff + cols) { coloff = rx - cols + 1; }
}

void editorClearScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
}

void editorWelcome() {
  char welcome[] = KILO_EDITOR "\n\r";
  int len = strlen(welcome);
  int padding = 0;
  
  if (len > cols) len = cols;
  padding = (cols - len) / 2;
  if (padding) {
    write(STDOUT_FILENO, "~", 1);
    padding--;
  }
  while (padding--) write(STDOUT_FILENO, " ", 1);
  write(STDOUT_FILENO, welcome, len);
}

void editorDrawRows() {
  int y = 0, z;
  z = (numrows < rows) ? numrows : rows;
  
  for (; y < z; y++) {
    int row = y + rowoff;
    int len = row_p[row].rsize - coloff;
    if (len < 0) { len = 0; }
    if (len > cols) { len = cols; }
    
    write(STDOUT_FILENO, row_p[row].render, len);
#if STATUSBAR_FEATURE || MESSAGE_FEATURE
    write(STDOUT_FILENO, "\r\n", 2);
#else
    if (y < rows - 1) write(STDOUT_FILENO, "\r\n", 2);
#endif
  }
  
  for (; y < rows; y++) {
    if (numrows == 0 && y == rows / 3) {
      editorWelcome();
      continue;
    }
#if STATUSBAR_FEATURE || MESSAGE_FEATURE
    write(STDOUT_FILENO, "~\r\n", 3);
#else
    write(STDOUT_FILENO, "~", 1);
    if (y < rows - 1) write(STDOUT_FILENO, "\r\n", 2);
#endif
  }
}

#if STATUSBAR_FEATURE

void editorDrawStatusBar() {
  int len, rlen;
  char *filename;
#define BUFLEN 80
  char status[BUFLEN];
  char rstatus[BUFLEN];
  
#define FORMAT "%.20s - %d lines"
  filename = fname ? fname : "[No Name]";
  len = snprintf(status, BUFLEN, FORMAT, filename, numrows);
#undef FORMAT
  if (dirty) {
    strcpy(status + len, " (modified)");
    len += 11;
  }
  rlen = snprintf(rstatus, BUFLEN, "%d/%d", cy + 1, numrows);
#undef BUFLEN
  if (len > cols) { len = cols; }

#define ESC "\x1b"
  write(STDOUT_FILENO, ESC"[1m", 4);
  write(STDOUT_FILENO, status, len);
  while (cols - len++ > rlen) write(STDOUT_FILENO, " ", 1);
  write(STDOUT_FILENO, rstatus, rlen);
  write(STDOUT_FILENO, ESC"[m", 3);
#undef ESC
#if MESSAGE_FEATURE
  write(STDOUT_FILENO, "\r\n", 2);
#endif
}
#endif

#if MESSAGE_FEATURE

void editorDrawMessageBar() {
  int msglen = strlen(status_msg);
  if (msglen > cols) { msglen = cols; }
  
#define ESC "\x1b"
#if ! STATUSBAR_FEATURE
  write(STDOUT_FILENO, ESC"[1m", 4);
#endif
  write(STDOUT_FILENO, ESC"[K", 3);
  if (time(NULL) - message_time < 5) {
    write(STDOUT_FILENO, status_msg, msglen);
  }
#if ! STATUSBAR_FEATURE
  write(STDOUT_FILENO, ESC"[m", 3);
#endif
#undef ESC
}
#endif

void editorCursorPosition() {
  int rowpos = (cy - rowoff) + 1;
  int colpos = (rx - coloff) + 1;
  char buf[32];
  
#define ESC "\x1b"
  snprintf(buf, 32, ESC"[%d;%dH", rowpos, colpos);
#undef ESC
  write(STDOUT_FILENO, buf, strlen(buf));
}

void editorRefreshScreen() {
  //write(STDOUT_FILENO, "\x1b[?25l", 6);
  editorScroll();
  editorClearScreen();
  editorDrawRows();
#if STATUSBAR_FEATURE
  editorDrawStatusBar();
#endif
#if MESSAGE_FEATURE
  editorDrawMessageBar();
#endif
  editorCursorPosition();
  
  //write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* input */

void editorMoveCursor(int key) {
  int length, inside_line, end_of_line;
  
  switch (key) {
  case ARROW_UP:
    if (cy > 0) { cy--; } break;
  case ARROW_LEFT:
    if (cx > 0) { cx--; } else
    if (cy > 0) { cy--; cx = cols; }
    break;
  case ARROW_DOWN:
    if (cy < numrows - 1) { cy++; } break;
  case ARROW_RIGHT:
    length = (cy < numrows) ? row_p[cy].size : 0;
    inside_line = (length && cx < length);
    end_of_line = (cy < numrows - 1 && cx == length);
    
    if (inside_line) { cx++; } else
    if (end_of_line) { cy++; cx = 0; }
    break;
  case HOME_KEY:
    cx = 0; break;
  case END_KEY:
    cx = (cy < numrows) ? row_p[cy].size : 0;
    break;
  }
  
  // TODO: eliminate copy-paste code
  length = (cy < numrows) ? row_p[cy].size : 0;
  if (cx > length) { cx = length; }
}

void editorMoveScreen(int key) {
  int lines = rows - 1;
  cy = rowoff;
  if (key == PAGE_DOWN) { cy += lines; }
  if (cy > numrows) { cy = numrows; }
  
  // TODO: optimize unnecessary function calls
  key = (key == PAGE_UP) ? ARROW_UP : ARROW_DOWN;
  while (lines--) { editorMoveCursor(key); }
}

void editorProcessKeypress() {
  
  int key = editorReadKey();
  
  if (key == CTRL_Q) {
    editorQuit();
    return;
  }
  quit = 2; // times to quit if dirty
  
  switch (key) {
  
  case ENTER:
  case INSERT_KEY:
  case CTRL_KEY('l'):
  case ESCAPE:
    break;
  
  case CTRL_KEY('s'):
    editorSave();
    break;
  
  case ARROW_UP:
  case ARROW_LEFT:
  case ARROW_DOWN:
  case ARROW_RIGHT:
  case HOME_KEY:
  case END_KEY:
    editorMoveCursor(key);
    break;
  
  case PAGE_UP:
  case PAGE_DOWN:
    editorMoveScreen(key);
    break;
  
  case BS:
  case BACKSPACE:
    if (cy == 0 && cx == 0) break;
    editorMoveCursor(ARROW_LEFT);
  case DELETE_KEY:
    if (numrows == 0) break;
    editorDeleteChar();
    break;
  
  default:
    if (numrows == 0) editorAppendRow("", 0);
    editorInsertChar(key);
  } // switch (key)
}

int main(int argc, char *argv[]) {
  enableRawMode();
  getWindowSize();
#if STATUSBAR_FEATURE
  rows -= 1;
#endif
#if MESSAGE_FEATURE
  rows -= 1;
#endif
  rowoff = 0;
  coloff = 0;
  cx = cy = 0;
  fname = NULL;
  row_p = NULL;
  numrows = 0;
  dirty = 0;
  
  
  if (argc > 1) {
    editorOpen(argv[1]);
  }
  
#if MESSAGE_FEATURE
  setStatusMsg("HELP: Ctrl-Q = quit, Ctrl-S = save");
#endif
  
  while(1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  
  return 0;
}


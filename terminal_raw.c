#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct termios canonical;

void disableRawMode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &canonical);
}

void enableRawMode() {
  tcgetattr(STDIN_FILENO, &canonical);
  atexit(disableRawMode);
  
  struct termios raw = canonical;
  raw.c_iflag &= ~(ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 4;
  
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
  enableRawMode();
  
  while(1) {
    char c = '\0';
    read(STDIN_FILENO, &c, 1);
    if (!c) continue;
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d '%c'\r\n", c, c);
    }
    if (c == CTRL_KEY('q')) break;
  }
  
  return 0;
}


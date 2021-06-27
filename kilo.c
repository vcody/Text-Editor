#include <ctype.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/*** DEFINES ***/
// Handles ctrl+(key) presses
#define CTRL_KEY(k) ((k) & 0x1f)
// Initialize empty buffer
#define APP_BUFFER_INIT {NULL, 0}
// Text editor version number
#define VERSION "0.0.1"

/*** DATA ***/
struct editorConfig {
	// Cursor position (x, y)
	int cx, cy;
	// holds screen dimensions
	// orig_termios: original terminal settings
	int screenrows;
	int screencols;

	struct termios orig_termios;
};

struct editorConfig E;

/*** TERMINAL ***/
void die(const char *s) {
	// Clears screen, resets cursor...
	// Then, error quits w/ string prior to error for context
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(s);
	exit(1);
}

void disableRawMode() {
	// Set new attributes to copy of original struct
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	// Retrieves terminal's attributes using tcgetattr()
	// Puts attributes into struct, modifies them...
	// In this case, we are removing the ECHO feature
	// Sets new attributes to terminal using tcsetattr()
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;

	// c_lflag = "local/miscellaneous flags"
	// Turning off ICANON makes input be read byte-by-byte
	// Turning off ISIG stops from sending (Ctrl-c, Ctrl-z) SIGINT/SIGTSTP signals
	// Turning off IEXTEN stops (Ctrl-v) literal character sending
	// Bit flip using AND-NOT operator
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

	// Turning off IXON stops "software flow control" (Ctrl-s, Ctrl-q)
	// Turning off ICRNL stops (Ctrl-m) translating carriage returns...
	// BRKINT, INPCK, ISTRIP are "tradition" and not necessarily needed in modern terminals
	// (e.g. '\r') as being inputted as newlines (e.g. '\n')
	raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP); 

	// Turning off OPOST stops all output processing...
	// Since terminal translates '\n' => '\r\n'
	raw.c_oflag &= ~(OPOST);

	// Sets the character size to 8 bits per byte (already default)
	raw.c_cflag |= (CS8);

	// Sets min. number of bytes of input needed before read() returns
	raw.c_cc[VMIN] = 0;
	// Sets max. amount of time to wait before read() returns (in 1/10 seconds)
	raw.c_cc[VTIME] = 1;

	// TCSAFLUSH argument specifies when to apply change
	// In this case, it discards input and waits for all pending output
	// ^ Note: when in Cygwin (like me currently), this behavior may not happen...
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

// Waits for a keypress, then returns it
// Doesn't work for multiple-byte sequences
char editorReadKey() {
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}
	return c;
}

int getCursorPosition(int *rows, int *cols) {
	char buffer[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;
	
	// Read chars into buffer until it reads 'R'
	while (i < sizeof(buffer) - 1) {
		if (read(STDIN_FILENO, &buffer[i], 1) != 1)
			break;
		if (buffer[i] == 'R')
			break;
		i++;
	}
	buffer[i] = '\0';
	
	// Make sure first char is escape sequence
	if (buffer[0] != '\x1b' || buffer[1] != '[')
		return -1;

	if (sscanf(&buffer[2], "%d;%d", rows, cols) != 2)
		return -1;
	
	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;
	
	// Move cursor to bottom-right of screen
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** APPEND BUFFER ***/
struct app_buffer {
	// *b: pointer to buffer in memory
	// len: length
	char *b;
	int len;
};

// Appends string to buffer 
void abAppend(struct app_buffer *ab, const char *s, int len) {
	// Allocate memory, size of current string + appending string	
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	
	// Copy string after end of current data in buffer...
	// Then, update pointer/length of app_buffer
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

// Deallocates memory used by app_buffer
void abFree(struct app_buffer *ab) {
	free(ab->b);
}

/*** OUTPUT ***/
void editorDrawRows(struct app_buffer *ab) {
	// Writes length rows ~ characters on start of each row
	for (int i= 0; i < E.screenrows; i++) { 
		// Display welcome message
		if (i == E.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
				"CV Editor -- ver. %s", VERSION);
			if (welcomelen > E.screencols) welcomelen = E.screencols;

			// Centers message
			int padding = (E.screencols - welcomelen) / 2;
			if (padding) {
				abAppend(ab, "~", 1);
				padding--;
			}
			while (padding--) abAppend(ab, " ", 1);
			abAppend(ab, welcome, welcomelen);
		}
		else abAppend(ab, "~", 1);
		
		// Escape sequence to clear lines 
		abAppend(ab, "\x1b[K", 3);

		// If it isn't the last row...
		if (i < E.screenrows - 1)
			abAppend(ab, "\r\n", 2);
	}
}

void editorRefreshScreen() {
	struct app_buffer ab = APP_BUFFER_INIT;

	// Uses escape sequences to hide cursor, reset cursor
	abAppend(&ab, "\x1b[?25l", 6); 
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	// Moves cursor on screen
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buffer, strlen(buffer)); 

	// Show cursor
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** INPUT ***/
// Adds WASD cursor movement
void editorMoveCursor(char key) {
	switch (key) {
		case 'a':
			E.cx--;
			break;
		case 'd':
			E.cx++;
			break;
		case 'w':
			E.cy--;
			break;
		case 's':
			E.cy++;
			break;
	}
}

// Takes keypress, then handles it based on switch case 
void editorProcessKeypress() {
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case 'w':
		case 's':
		case 'a':
		case 'd':
			editorMoveCursor(c);
			break;
	}
}

/*** INIT ***/
void initEditor() {
	// Cursor position (x,y)
	E.cx = 0;
	E.cy = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) 
		die("getWindowSize");
}

int main() {
	// Terminal starts in canonical/cooked mode 
	// (i.e. keyboard input is sent upon Enter press)
	// This changes it to what is needed, raw mode
	// (i.e. keyboard input is processed per key input)
	enableRawMode();
	initEditor();

	// Read bytes from stdin until Ctrl-q press
	const int True = 1;
	while (True) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}

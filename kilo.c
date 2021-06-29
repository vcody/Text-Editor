// Feature test macros
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/*** DEFINES ***/
// Handles ctrl+(key) presses
#define CTRL_KEY(k) ((k) & 0x1f)
// Initialize empty buffer
#define APP_BUFFER_INIT {NULL, 0}
// Text editor version number
#define VERSION "0.0.1"
// Length of tabs (max. chars needed)
#define TAB_STOP 8

// Arrow key constants
// > 1000 to be out of char range
enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY
};

/*** DATA ***/
// Stores row of text
typedef struct erow {
	int size;
	char *chars;
	// Render information
	int rsize;
	char *render;
} erow;

// Global state
struct editorConfig {
	// Cursor position (x, y)
	// Render index
	int cx, cy;
	int rx;
	// Holds screen dimensions
	// orig_termios: original terminal settings
	int screenrows;
	int screencols;
	// For row text storing
	int numrows;
	erow *row;
	// Row/col that user is currently on
	int rowoffset;
	int coloffset;
	// Status bar information
	char *filename;
	char statusmsg[80];
	time_t msg_time;

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
int editorReadKey() {
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	// Reading for arrow keys
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch(seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else { 
				switch(seq[1]) { 
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'F': return END_KEY;
					case 'H': return HOME_KEY;
				}
			}
		}
		else if (seq[0] == '0') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	}
	else 
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

/*** ROW OPERATIONS ***/
int editorRowCxToRx(erow *row, int cx) {
	// Converts chars index -> render index
	int rx = 0; 
	for (int i = 0; i < cx; i++) {
		// If tab...
		if (row->chars[i] == '\t')
			// Find # cols to left of next tab stop
			rx += (TAB_STOP - 1) - (rx % TAB_STOP);
		rx++;
	}
	return rx;
}
void editorUpdateRow(erow *row) {
	// Renders each text row, replacing tabs with spaces
	int tabs = 0;
	// Count # of tabs for memory allocation
	for (int j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;
	
	free(row->render);
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	int idx = 0;
	for (int j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			// Append spaces until tab stop (col. % 8 == 0)
			while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
		}
		else
			row->render[idx++] = row->chars[j];
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
	// Adds row s of size len to text
	// Allocate size of erow * number of rows
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
}

/*** FILE I/O ***/
void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);

	// Open file
	FILE *fp = fopen(filename, "r");	
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	linelen = getline(&line, &linecap, fp);
	
	// While there are more lines to be read...
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		
		editorAppendRow(line, linelen);
	}
	// Close pointer/file
	free(line);
	fclose(fp);
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
void editorScroll() {
	E.rx = E.cx;
	if (E.cy < E.numrows)
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

	// If user scrolls up...
	if (E.cy < E.rowoffset) 
		E.rowoffset = E.cy;
	// If user scrolls down...
	if (E.cy >= E.rowoffset + E.screenrows)
		E.rowoffset = E.cy - E.screenrows + 1;
	// If user scrolls left...
	if (E.rx < E.coloffset)
		E.coloffset = E.rx;
	// If user scrolls right...
	if (E.rx >= E.coloffset + E.screencols)
		E.coloffset = E.rx - E.screencols + 1;
}

void editorDrawRows(struct app_buffer *ab) {
	// Writes length rows ~ characters on start of each row
	for (int i= 0; i < E.screenrows; i++) { 
		int filerow = i + E.rowoffset;
		// If drawing row that isn't part of text buffer...
		if (filerow >= E.numrows) {
		// Display welcome message if no file specified
		if (E.numrows == 0 && i == E.screenrows / 3) {
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
		}
		else {
			int len = E.row[filerow].rsize - E.coloffset;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloffset], len);
		}
		// Escape sequence to clear lines 
		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
}

void editorDrawStatusBar(struct app_buffer *ab) {
	abAppend(ab, "\x1b[7m", 4);

	char status[80];
	char rstatus[80];

	// Displays filename, up to 20 chars
	int len = snprintf(status, sizeof(status), "%.20s - %d lines",
		E.filename ? E.filename : "[No Name]", E.numrows);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
		E.cy + 1, E.numrows);

	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);

	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		}
		else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMsgBar(struct app_buffer *ab) {
	abAppend(ab, "\x1b[K", 3);
	int length = strlen(E.statusmsg);
	if (length > E.screencols) length = E.screencols;
	if (length && time(NULL) - E.msg_time < 5) 
		abAppend(ab, E.statusmsg, length);
}

void editorRefreshScreen() {
	editorScroll();

	struct app_buffer ab = APP_BUFFER_INIT;

	// Uses escape sequences to hide cursor, reset cursor
	abAppend(&ab, "\x1b[?25l", 6); 
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMsgBar(&ab);

	// Moves cursor on screen
	char buffer[32];
	// Use (E.cy - E.rowoffset) for proper cursor behavior upon scrolling
	// Use (E.cx - E.coloffset) for proper cursor behavior upon scrolling
	snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 1, (E.rx - E.coloffset) + 1);
	abAppend(&ab, buffer, strlen(buffer)); 

	// Show cursor
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMsg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);

	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);

	E.msg_time = time(NULL);
}

/*** INPUT ***/
// Adds arrow key cursor movement
void editorMoveCursor(int key) {
	// So cursor doesn't go past current line...
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			// Makes sure user goes from beginning of line to end of previous line upon backspaces
			if (E.cx != 0) 
				E.cx--;
			else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size; 
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) 
				E.cx++;
			else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0)
				E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows)
				E.cy++;
			break;
	}
	
	// Makes sure cursor stays on end of line
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen)
		E.cx = rowlen;
}

// Takes keypress, then handles it based on switch case 
void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP)
					E.cy = E.rowoffset;
				else if (c == PAGE_DOWN) { 
					E.cy = E.rowoffset + E.screenrows - 1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/*** INIT ***/
void initEditor() {
	// Cursor position (x,y)
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;

	// Row data
	E.numrows = 0; 
	E.rowoffset = 0;
	E.coloffset = 0;
	E.row = NULL;

	// File information
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.msg_time = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) 
		die("getWindowSize");
	
	// So editorDrawRows() doesn't draw text @ bottom of screen
	E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
	// Terminal starts in canonical/cooked mode 
	// (i.e. keyboard input is sent upon Enter press)
	// This changes it to what is needed, raw mode
	// (i.e. keyboard input is processed per key input)
	enableRawMode();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);

	editorSetStatusMsg("HELP: Ctrl-Q to quit");

	// Read bytes from stdin until Ctrl-q press
	const int True = 1;
	while (True) {
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}

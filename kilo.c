/*** INCLUDES ***/
#include <ctype.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

/*** DATA ***/
struct termios orig_termios;

/*** TERMINAL ***/
void die(const char *s) {
	// perror() takes global errno variable and prints msg for it
	// *s represents string given to perror() prior to error...
	// Therefore, giving context to what part of code gave said error
	// Then, we exit with status of 1 (failure)
	perror(s);
	exit(1);
}

void disableRawMode() {
	// Set new attributes to copy of original struct
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	// Retrieves terminal's attributes using tcgetattr()
	// Puts attributes into struct, modifies them...
	// In this case, we are removing the ECHO feature
	// Sets new attributes to terminal using tcsetattr()
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = orig_termios;

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

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	// TCSAFLUSH argument specifies when to apply change
	// In this case, it discards input and waits for all pending output
	// ^ Note: when in Cygwin (like me currently), this behavior may not happen...
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

/*** INIT ***/
int main() {
	// Terminal starts in canonical/cooked mode 
	// (i.e. keyboard input is sent upon Enter press)
	// This changes it to what is needed, raw mode
	// (i.e. keyboard input is processed per key input)
	enableRawMode();

	// Read 1 byte from STDIN into c, until end of bytes or q press
	const int True = 1;
	while (True) {
		char c = '\0';

		// Using Cygwin, EAGAIN is not treated as an error...
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
		// if (read(STDIN_FILENO, &c, 1) == -1)
			die("read");

		// If c is control character...
		// Control ASCII codes: 0-31, 127 (non-printable characters)
		if (iscntrl(c)) 
			printf("%d\r\n", c);
		else
			printf("%d ('%c')\r\n", c, c);
		if (c == 'q') break;
	}
	return 0;
}

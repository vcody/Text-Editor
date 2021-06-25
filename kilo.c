#include <ctype.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>

struct termios orig_termios;

void disableRawMode() {
	// Set new attributes to copy of original struct
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
	// Retrieves terminal's attributes using tcgetattr()
	// Puts attributes into struct, modifies them...
	// In this case, we are removing the ECHO feature
	// Sets new attributes to terminal using tcsetattr()
	tcgetattr(STDIN_FILENO, &orig_termios);
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
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
	// Terminal starts in canonical/cooked mode 
	// (i.e. keyboard input is sent upon Enter press)
	// This changes it to what is needed, raw mode
	// (i.e. keyboard input is processed per key input)
	enableRawMode();

	char c;

	// Read 1 byte from STDIN into c, until end of bytes or q press
	const int True = 1;
	while (True) {
		char c = '\0';
		read(STDIN_FILENO, &c, 1);
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

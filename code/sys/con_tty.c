/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#ifndef DEDICATED
#include "../client/client.h"
#endif

#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/*
 * the amount of characters to read for tty console input
 */
#define TTY_INPUT_BUFER_SIZE 32

/*
=============================================================
tty console routines

NOTE: if the user is editing a line when something gets printed to the early
console then it won't look good so we provide CON_Hide and CON_Show to be
called before and after a stdout or stderr output
=============================================================
*/

extern qboolean stdinIsATTY;
static qboolean stdin_active;
// general flag to tell about tty console mode
static qboolean ttycon_on = qfalse;
static int ttycon_hide = 0;
static int ttycon_show_overdue = 0;

// some key codes that the terminal may be using, initialised on start up
static int TTY_erase;
static int TTY_eof;

static struct termios TTY_tc;

static undobuf_t TTY_undobuf;
static yankbuf_t TTY_yankbuf;
static field_t TTY_con = { .undobuf = &TTY_undobuf, .yankbuf = &TTY_yankbuf };

static int prev_cur_y, prev_cur_x = 1;

// This is somewhat of aduplicate of the graphical console history
// but it's safer more modular to have our own here
#define CON_HISTORY 32
static hist_t ttyEditLines[ CON_HISTORY ];
static int hist_current = -1, hist_count = 0;

#ifndef DEDICATED
// Don't use "]" as it would be the same as in-game console,
//   this makes it clear where input came from.
#define TTY_CONSOLE_PROMPT "tty]"
#else
#define TTY_CONSOLE_PROMPT "]"
#endif

static void HistToField( field_t *field, hist_t *hist ) {
	field->cursor = hist->cursor;
	field->scroll = 0;
	field->widthInChars = 0;
	field->undobuf = &TTY_undobuf;
	field->yankbuf = &TTY_yankbuf;
	Q_strncpyz( field->buffer, hist->buffer, MAX_EDIT_LINE );
}

static void FieldToHist( hist_t *hist, field_t *field ) {
	hist->cursor = field->cursor;
	hist->scroll = 0;
	Q_strncpyz( hist->buffer, field->buffer, MAX_EDIT_LINE );
}

/*
==================
CON_FlushIn

Flush stdin, I suspect some terminals are sending a LOT of shit
FIXME relevant?
==================
*/
static void CON_FlushIn( void )
{
	char key;
	while (read(STDIN_FILENO, &key, 1)!=-1);
}

/*
==================
CON_Back

Output a backspace

NOTE: it seems on some terminals just sending '\b' is not enough so instead we
send "\b \b"
(FIXME there may be a way to find out if '\b' alone would work though)
==================
*/
static void CON_Back( void )
{
	char key;
	size_t UNUSED_VAR size;

	key = '\b';
	size = write(STDOUT_FILENO, &key, 1);
	key = ' ';
	size = write(STDOUT_FILENO, &key, 1);
	key = '\b';
	size = write(STDOUT_FILENO, &key, 1);
}

/*
==================
CON_Hide

Clear the display of the line currently edited
bring cursor back to beginning of line
==================
*/
static void CON_Hide( void )
{
	if( ttycon_on )
	{
		int i;
		if (ttycon_hide)
		{
			ttycon_hide++;
			return;
		}
		if (TTY_con.cursor>0)
		{
			for (i=0; i<TTY_con.cursor; i++)
			{
				CON_Back();
			}
		}
		// Delete prompt
		for (i = strlen(TTY_CONSOLE_PROMPT); i > 0; i--) {
			CON_Back();
		}
		ttycon_hide++;
	}
}

static void CON_RedrawEditLine( void );

/*
==================
CON_Show

Show the current line.
==================
*/
static void CON_Show( void )
{
	if( ttycon_on )
	{
		assert(ttycon_hide>0);
		ttycon_hide--;

		if (ttycon_hide == 0)
		{
			CON_RedrawEditLine();
		}
	}
}

/*
==================
CON_Shutdown

Never exit without calling this, or your terminal will be left in a pretty bad state
==================
*/
void CON_Shutdown( void )
{
	if (ttycon_on)
	{
		CON_Hide();
		tcsetattr (STDIN_FILENO, TCSADRAIN, &TTY_tc);
	}

	// Restore blocking to stdin reads
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);
}

/*
==================
Hist_Add
==================
*/
void Hist_Add(field_t *field)
{
	int i;

	// Don't save blank lines in history.
	if (!field->cursor)
		return;

	assert(hist_count <= CON_HISTORY);
	assert(hist_count >= 0);
	assert(hist_current >= -1);
	assert(hist_current <= hist_count);
	// make some room
	for (i=CON_HISTORY-1; i>0; i--)
	{
		ttyEditLines[i] = ttyEditLines[i-1];
	}
	FieldToHist(ttyEditLines, field);
	if (hist_count<CON_HISTORY)
	{
		hist_count++;
	}
	hist_current = -1; // re-init
}

/*
==================
Hist_Prev
==================
*/
hist_t *Hist_Prev( void )
{
	int hist_prev;
	assert(hist_count <= CON_HISTORY);
	assert(hist_count >= 0);
	assert(hist_current >= -1);
	assert(hist_current <= hist_count);
	hist_prev = hist_current + 1;
	if (hist_prev >= hist_count)
	{
		return NULL;
	}
	hist_current++;
	return &(ttyEditLines[hist_current]);
}

/*
==================
Hist_Next
==================
*/
hist_t *Hist_Next( void )
{
	assert(hist_count <= CON_HISTORY);
	assert(hist_count >= 0);
	assert(hist_current >= -1);
	assert(hist_current <= hist_count);
	if (hist_current >= 0)
	{
		hist_current--;
	}
	if (hist_current == -1)
	{
		return NULL;
	}
	return &(ttyEditLines[hist_current]);
}

/*
==================
CON_SigCont
Reinitialize console input after receiving SIGCONT, as on Linux the terminal seems to lose all
set attributes if user did CTRL+Z and then does fg again.
==================
*/

void CON_SigCont(int signum)
{
	CON_Init();
}

/*
==================
CON_RedrawEditLine

Redraws the entire edit line
==================
*/
static void CON_RedrawEditLine( void )
{
	struct winsize size;
	ioctl(STDOUT_FILENO,TIOCGWINSZ,&size);
	int width = size.ws_col;

	size_t buflen = strlen(TTY_con.buffer);
	size_t len = buflen + sizeof TTY_CONSOLE_PROMPT - 1;
	int buf_y = (len - 1) / width;
	int buf_x = (len - 1) % width;

	int cur_y = (TTY_con.cursor + sizeof TTY_CONSOLE_PROMPT - 1) / width;
	int cur_x = (TTY_con.cursor + sizeof TTY_CONSOLE_PROMPT - 1) % width;

	// move to first line of previously inserted text
	if (prev_cur_y > 0)
	{
		char s[1024];
		size_t n = Com_sprintf(s, sizeof s, "\x1b[%dA", prev_cur_y);
		write(STDOUT_FILENO, s, n);
	}

	// move to first column of previously inserted text
	if (prev_cur_x > 0)
	{
		char s[1024];
		size_t n = Com_sprintf(s, sizeof s, "\x1b[%dD", prev_cur_x);
		write(STDOUT_FILENO, s, n);
	}

	// clear from cursor to end of screen
	char s[] = "\x1b[J";
	write(STDOUT_FILENO, s, sizeof s - 1);

	// write the prompt, the buffer, and a space (so the line wraps down)
	write(STDOUT_FILENO, TTY_CONSOLE_PROMPT, sizeof TTY_CONSOLE_PROMPT - 1);
	write(STDOUT_FILENO, TTY_con.buffer, buflen);

	// write space if at end of line (to create new line below for the
	// cursor)
	if (buf_x == width - 1)
	{
		char spc = ' ';
		write(STDOUT_FILENO, &spc, 1);
	}

	// move to the line where the cursor is
	if (buf_y - cur_y > 0)
	{
		char s[1024];
		size_t n = Com_sprintf(s, sizeof s, "\x1b[%dA", buf_y - cur_y);
		write(STDOUT_FILENO, s, n);
	}

	// move to column where the cursor is
	if (buf_x - cur_x + 1 > 0)
	{
		char s[1024];
		size_t n = Com_sprintf(s, sizeof s, "\x1b[%dD", buf_x - cur_x + 1);
		write(STDOUT_FILENO, s, n);
	}
	else if (buf_x - cur_x + 1 < 0)
	{
		char s[1024];
		size_t n = Com_sprintf(s, sizeof s, "\x1b[%dC", -(buf_x - cur_x + 1));
		write(STDOUT_FILENO, s, n);
	}

	// save cursor position for next call
	prev_cur_y = cur_y, prev_cur_x = cur_x;
}

/*
==================
CON_Init

Initialize the console input (tty mode if possible)
==================
*/
void CON_Init( void )
{
	struct termios tc;

	// If the process is backgrounded (running non interactively)
	// then SIGTTIN or SIGTOU is emitted, if not caught, turns into a SIGSTP
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);

	// If SIGCONT is received, reinitialize console
	signal(SIGCONT, CON_SigCont);

	// Make stdin reads non-blocking
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK );

	if (!stdinIsATTY)
	{
		Com_Printf("tty console mode disabled\n");
		ttycon_on = qfalse;
		stdin_active = qtrue;
		return;
	}

	Field_Clear(&TTY_con);
	tcgetattr (STDIN_FILENO, &TTY_tc);
	TTY_erase = TTY_tc.c_cc[VERASE];
	TTY_eof = TTY_tc.c_cc[VEOF];
	tc = TTY_tc;

	/*
	ECHO: don't echo input characters
	ICANON: enable canonical mode.  This  enables  the  special
	characters  EOF,  EOL,  EOL2, ERASE, KILL, REPRINT,
	STATUS, and WERASE, and buffers by lines.
	ISIG: when any of the characters  INTR,  QUIT,  SUSP,  or
	DSUSP are received, generate the corresponding signal
	*/
	tc.c_lflag &= ~(ECHO | ICANON);

	/*
	ISTRIP strip off bit 8
	INPCK enable input parity checking
	*/
	tc.c_iflag &= ~(ISTRIP | INPCK);
	tc.c_cc[VMIN] = 1;
	tc.c_cc[VTIME] = 0;
	tc.c_cc[VINTR] = 0;
	tcsetattr (STDIN_FILENO, TCSADRAIN, &tc);
	ttycon_on = qtrue;
	ttycon_hide = 1; // Mark as hidden, so prompt is shown in CON_Show
	CON_Show();
}

/*
================
CON_ClearScreen

Generate ASCII escape code to clear the screen, and redraw the edit-line
================
*/
static void CON_ClearScreen( void )
{
	write(STDOUT_FILENO, "\x1B[2J\x1B[H", 7);
	CON_RedrawEditLine();
}

void CON_HistPrev( void )
{
	hist_t *history = Hist_Prev();
	if (history)
	{
		CON_Hide();
		HistToField(&TTY_con, history);
		CON_Show();
	}
	CON_FlushIn();
}

void CON_HistNext( void )
{
	hist_t *history = Hist_Next();
	CON_Hide();
	if (history)
	{
		HistToField(&TTY_con, history);
	}
	else
	{
		Field_Clear(&TTY_con);
	}
	CON_Show();
	CON_FlushIn();
}

typedef struct {
	ssize_t i;
	ssize_t n;
	char data[TTY_INPUT_BUFER_SIZE];
} tty_input_buffer;

/*
================
getch_from_buf

get a keyboard input character from the tty input buffer

returns character if it is available or -1 if not available
================
 */
int getch_from_buf(tty_input_buffer *buf)
{
	if (++buf->i >= buf->n) {
		buf->i = 0;
		buf->n = read(STDIN_FILENO, &buf->data, sizeof buf->data);
		if (buf->n < 0) {
			buf->n = 0;
			return -1;
		}
	}
	return buf->data[buf->i];
}

/*
==================
CON_Input
==================
*/
char *CON_Input( void )
{
	// we use this when sending back commands
	static char text[MAX_EDIT_LINE];
	static qboolean lastchar_esc = qfalse;
	qboolean meta_on;
	static tty_input_buffer input = { 0 };
	int key;
	size_t UNUSED_VAR size;

	if(ttycon_on)
	{
		while ((key = getch_from_buf(&input)) != -1)
		{
			meta_on = lastchar_esc;
			lastchar_esc = qfalse;
			if (key == '\x1B')
			{
				lastchar_esc = qtrue;
			}

			// we have something
			// backspace?
			// NOTE TTimo testing a lot of values .. seems it's the only way to get it to work everywhere
			if ((key == TTY_erase) || (key == 127) || (key == 8))
			{
				if (meta_on)
					Field_RuboutWord(&TTY_con);
				else
					Field_RuboutChar(&TTY_con);
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == '\n')
			{
#ifndef DEDICATED
				// if not in the game explicitly prepend a slash if needed
				if (clc.state != CA_ACTIVE && con_autochat->integer && TTY_con.cursor &&
					TTY_con.buffer[0] != '/' && TTY_con.buffer[0] != '\\')
				{
					memmove(TTY_con.buffer + 1, TTY_con.buffer, sizeof(TTY_con.buffer) - 1);
					TTY_con.buffer[0] = '\\';
					TTY_con.cursor++;
				}

				if (TTY_con.buffer[0] == '/' || TTY_con.buffer[0] == '\\') {
					Q_strncpyz(text, TTY_con.buffer + 1, sizeof(text));
				} else if (TTY_con.cursor) {
					if (con_autochat->integer) {
						Com_sprintf(text, sizeof(text), "cmd say %s", TTY_con.buffer);
					} else {
						Q_strncpyz(text, TTY_con.buffer, sizeof(text));
					}
				} else {
					text[0] = '\0';
				}

				// push it in history
				Hist_Add(&TTY_con);
				CON_Hide();
				Com_Printf("%s%s\n", TTY_CONSOLE_PROMPT, TTY_con.buffer);
				Field_Clear(&TTY_con);
				CON_Show();
#else
				// push it in history
				Hist_Add(&TTY_con);
				Q_strncpyz(text, TTY_con.buffer, sizeof(text));
				Field_Clear(&TTY_con);
				key = '\n';
				size = write(STDOUT_FILENO, &key, 1);
				size = write(STDOUT_FILENO, TTY_CONSOLE_PROMPT, strlen(TTY_CONSOLE_PROMPT));
#endif
				prev_cur_y = 0;
				prev_cur_x = 1;

				return text;
			}
			if (key == CTRL('V')) {
				Field_ClipboardPaste( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('Z') || key == CTRL('_')) {
				Field_Undo( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('Y')) {
				Field_Yank( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'y' && meta_on) {
				Field_YankRotate( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('C')) {
				Field_Clear( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'u' && meta_on) {
				Field_MakeWordUpper( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'l' && meta_on ) {
				Field_MakeWordLower( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'c' && meta_on ) {
				Field_MakeWordCapitalized( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('T')) {
				Field_TransposeChars( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 't' && meta_on ) {
				Field_TransposeWords( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('F')) {
				Field_MoveForwardChar( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'f' && meta_on ) {
				Field_MoveForwardWord( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('B')) {
				Field_MoveBackChar( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'b' && meta_on) {
				Field_MoveBackWord( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('D')) {
				Field_DeleteChar( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (tolower(key) == 'd' && meta_on) {
				Field_DeleteWord( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('W')) {
				Field_RuboutLongWord( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('U')) {
				Field_RuboutLine( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('K')) {
				Field_DeleteLine( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('A')) {
				Field_MoveLineStart( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('E')) {
				Field_MoveLineEnd( &TTY_con );
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('N')) {
				CON_HistNext();
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('P')) {
				CON_HistPrev();
				CON_RedrawEditLine();
				return NULL;
			}
			if (key == CTRL('L')) {
				CON_ClearScreen();
			}
			if (key == '\t') {
				if (*TTY_con.buffer) {
					CON_Hide();
					Field_AutoComplete( &TTY_con );
					CON_Show();
				}
				CON_RedrawEditLine();
				return NULL;
			}

			// VT 100 keys
			if (meta_on && (key == '[' || key == 'O')) {
				qboolean ctrl_on = qfalse;
				if ((key = getch_from_buf(&input)) == -1)
					return NULL;

				// check if the user pressed CTRL-<ARROW>
				if (key == '1') {
					if ((key = getch_from_buf(&input)) == -1)
						return NULL;

					if (key == ';') {
						if ((key = getch_from_buf(&input)) == -1)
							return NULL;

						if (key == '5') {
							ctrl_on = qtrue;

							if ((key = getch_from_buf(&input)) == -1)
								return NULL;
						}
					}
				}

				// check for DELETE key
				if (key == '3') {
					if ((key = getch_from_buf(&input)) == -1)
						return NULL;

					if (key == '~') {
						Field_DeleteChar( &TTY_con );
						CON_RedrawEditLine();
						return NULL;
					}
				}

				switch (key) {
					case 'A': // up arrow
						CON_HistPrev();
						return NULL;
					case 'B': // down arrow
						CON_HistNext();
						return NULL;
					case 'D': // left arrow
						if (ctrl_on)
							Field_MoveBackWord( &TTY_con );
						else
							Field_MoveBackChar( &TTY_con );
						CON_RedrawEditLine();
						return NULL;
					case 'C': // right arrow
						if (ctrl_on)
							Field_MoveForwardWord( &TTY_con );
						else
							Field_MoveForwardChar( &TTY_con );
						CON_RedrawEditLine();
						return NULL;
					case 'H': // home
						Field_MoveLineStart( &TTY_con );
						CON_RedrawEditLine();
						return NULL;
					case 'F': // end
						Field_MoveLineEnd( &TTY_con );
						CON_RedrawEditLine();
						return NULL;
				}
			}

			if (isprint(key)) {
				Field_InsertChar( &TTY_con, key );
				CON_RedrawEditLine();
			}
		}

		return NULL;
	}
	else if (stdin_active) {
		int     len;
		fd_set  fdset;
		struct timeval timeout;

		FD_ZERO(&fdset);
		FD_SET(STDIN_FILENO, &fdset); // stdin
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		if(select (STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == -1 || !FD_ISSET(STDIN_FILENO, &fdset))
			return NULL;

		len = read(STDIN_FILENO, text, sizeof(text));
		if (len == 0) { // eof!
			stdin_active = qfalse;
			return NULL;
		}

		if (len < 1)
			return NULL;
		text[len-1] = 0;    // rip off the /n and terminate

		return text;
	}
	return NULL;
}

/*
==================
CON_Print
==================
*/
void CON_Print( const char *msg )
{
	if (!msg[0])
		return;

	CON_Hide( );

	if( com_ansiColor && com_ansiColor->integer )
		Sys_AnsiColorPrint( msg );
	else
		fputs( msg, stderr );

	if (!ttycon_on) {
		// CON_Hide didn't do anything.
		return;
	}

	// Only print prompt when msg ends with a newline, otherwise the console
	//   might get garbled when output does not fit on one line.
	if (msg[strlen(msg) - 1] == '\n') {
		CON_Show();

		// Run CON_Show the number of times it was deferred.
		while (ttycon_show_overdue > 0) {
			CON_Show();
			ttycon_show_overdue--;
		}
	}
	else
	{
		// Defer calling CON_Show
		ttycon_show_overdue++;
	}
}

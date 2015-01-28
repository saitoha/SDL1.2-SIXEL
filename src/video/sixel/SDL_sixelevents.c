/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

#include <stdio.h>

#include <sixel.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>

#include "SDL.h"
#include "../../events/SDL_sysevents.h"
#include "../../events/SDL_events_c.h"
#include "SDL_sixelvideo.h"
#include "SDL_sixelevents_c.h"

#if 0
#define SIXEL_DEBUG 1
#endif

#define SIXEL_UP                (1 << 12 | ('A' - '@'))
#define SIXEL_DOWN              (1 << 12 | ('B' - '@'))
#define SIXEL_RIGHT             (1 << 12 | ('C' - '@'))
#define SIXEL_LEFT              (1 << 12 | ('D' - '@'))
#define SIXEL_END               (1 << 12 | ('F' - '@'))
#define SIXEL_HOME              (1 << 12 | ('H' - '@'))
#define SIXEL_F1                (1 << 12 | ('P' - '@'))
#define SIXEL_F2                (1 << 12 | ('Q' - '@'))
#define SIXEL_F3                (1 << 12 | ('R' - '@'))
#define SIXEL_F4                (1 << 12 | ('S' - '@'))
#define SIXEL_FKEYS             (1 << 12 | ('~' - '@'))
#define SIXEL_MOUSE_SGR         (1 << 12 | ('<' - ';') << 4 << 6 | ('M' - '@'))
#define SIXEL_MOUSE_SGR_RELEASE (1 << 12 | ('<' - ';') << 4 << 6 | ('m' - '@'))
#define SIXEL_MOUSE_DEC         (1 << 12 | ('&' - 0x1f) << 6 | ('w' - '@'))
#define SIXEL_DTTERM_SEQS       (1 << 12 | ('t' - '@'))
#define SIXEL_UNKNOWN           (513)

typedef struct _key {
	int params[256];
	int nparams;
	int value;
} sixel_key_t;

enum _state {
	STATE_GROUND = 0,
	STATE_ESC = 1,
	STATE_CSI = 2,
	STATE_CSI_IGNORE = 3,
	STATE_CSI_PARAM = 4,
};

/* The translation tables from a console scancode to a SDL keysym */
static SDLKey keymap[1 << 13];

static int SendModifierKey(int state, Uint8 press_state);
static int GetScancode(int code);
static int GetKsymScancode(SDLKey sym);
static int GetState(int code);
static SDL_keysym *TranslateKey(int scancode, SDL_keysym *keysym);

static int get_input(char *buf, int size) {
	fd_set fdset;
	struct timeval timeout;
	FD_ZERO(&fdset);
	FD_SET(STDIN_FILENO, &fdset);
	timeout.tv_sec = 0;
	timeout.tv_usec = 1;
	if ( select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == 1 )
		return read(STDIN_FILENO, buf, size);
	return 0;
}

static int getkeys(char *buf, int nread, sixel_key_t *keys)
{
	int i, c;
	int size = 0;
	static int state = STATE_GROUND;
	static int ibytes = 0;
	static int pbytes = 0;

	for ( i=0; i<nread; i++ ) {
		c = buf[i];
		switch (state) {
		case STATE_GROUND:
			switch (c) {
			case 0x1b:
				state = STATE_ESC;
				break;
			default:
				keys[size++].value = c;
				break;
			}
			break;
		case STATE_ESC:
			switch (c) {
			case 'O':
			case '[':
				keys[size].nparams = 0;
				pbytes = 0;
				state = STATE_CSI;
				break;
			default:
				keys[size++].value = 0x1b;
				keys[size++].value = c;
				state = STATE_GROUND;
				break;
			}
			break;
		case STATE_CSI:
			switch (c) {
			case '\x1b':
				state = STATE_ESC;
				break;
			case '\x00'...'\x1a':
			case '\x1c'...'\x1f':
			case '\x7f':
				break;
			case ' '...'/':
				ibytes = c - ' ';
				pbytes = 0;
				state = STATE_CSI_PARAM;
				break;
			case '0'...'9':
				ibytes = 0;
				pbytes = c - '0';
				keys[size].nparams = 0;
				state = STATE_CSI_PARAM;
				break;
			case '<'...'?':
				ibytes = (c - ';') << 4;
				keys[size].nparams = 0;
				state = STATE_CSI_PARAM;
				break;
			case '@'...'~':
				keys[size].nparams = 0;
				keys[size++].value = 1 << 12 | (c - '@');
				state = STATE_GROUND;
				break;
			default:
				state = STATE_GROUND;
				break;
			}
			break;
		case STATE_CSI_PARAM:
			switch (c) {
			case '\x1b':
				state = STATE_ESC;
				break;
			case '\x00'...'\x1a':
			case '\x1c'...'\x1f':
			case '\x7f':
				break;
			case ' '...'/':
				ibytes |= c - 0x1f;
				state = STATE_CSI_PARAM;
				break;
			case '0'...'9':
				pbytes = pbytes * 10 + c - '0';
				state = STATE_CSI_PARAM;
				break;
			case ':'...';':
				if ( keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params) ) {
					keys[size].params[keys[size].nparams++] = pbytes;
					pbytes = 0;
				}
				break;
			case '@'...'~':
				if ( keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params) ) {
					keys[size].params[keys[size].nparams++] = pbytes;
					keys[size++].value = 1 << 12 | ibytes << 6  | c - '@';
				}
				state = STATE_GROUND;
				break;
			default:
				state = STATE_GROUND;
				break;
			}
			break;
		}
	}
	return size;
}


void SIXEL_PumpEvents(_THIS)
{
	int posted = 0;
	static int prev_x = -1, prev_y = -1;
#if SIXEL_DEBUG
	static int events = 0;
#endif
	char buf[4096];
	static sixel_key_t keys[4096];
	SDL_keysym keysym;
	sixel_key_t *key;
	int nread, nkeys;
	int i;

	if(!this->screen) /* Wait till we got the screen initialized */
		return;

	SDL_mutexP(SIXEL_mutex);
	nread = get_input(buf, sizeof(buf));
	SDL_mutexV(SIXEL_mutex);
	if ( nread > 0 ) {
		nkeys = getkeys(buf, nread, keys);
		if ( nkeys >= sizeof(keys) / sizeof(*keys) )
			nkeys = sizeof(keys) / sizeof(*keys) - 1;
		for ( i = 0; i < nkeys; ++i ) {
			key = keys + i;
			switch (key->value) {
			case SIXEL_DTTERM_SEQS:
				switch (key->params[0]) {
				case 4:
					SIXEL_pixel_h = key->params[1];
					SIXEL_pixel_w = key->params[2];
					break;
				case 8:
					SIXEL_cell_h = key->params[1];
					SIXEL_cell_w = key->params[2];
					break;
				default:
					break;
				}
				break;
			case SIXEL_MOUSE_SGR:
				if ( key->nparams < 3 )
					break;
				if ( SIXEL_cell_h == 0 || SIXEL_cell_w == 0 )
					break;
				SIXEL_mouse_y = (key->params[2] - 1) * (SIXEL_pixel_h / SIXEL_cell_h);
				SIXEL_mouse_x = (key->params[1] - 1) * (SIXEL_pixel_w / SIXEL_cell_w);
				switch (key->params[0]) {
				case 0:
					if (!(SIXEL_mouse_button & 1)) {
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 1, 0, 0);
						SIXEL_mouse_button |= 1;
					}
					break;
				case 1:
					if (!(SIXEL_mouse_button & 2)) {
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 2, 0, 0);
						SIXEL_mouse_button |= 2;
					}
					break;
				case 2:
					if (!(SIXEL_mouse_button & 4)) {
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 3, 0, 0);
						SIXEL_mouse_button |= 4;
					}
					break;
				case 33: /* button1 dragging */
				case 34: /* button2 dragging */
				case 35: /* button3 dragging */
				default:
					break;
				}
				break;
			case SIXEL_MOUSE_SGR_RELEASE:
				if ( key->nparams < 3 )
					break;
				if ( SIXEL_cell_h == 0 || SIXEL_cell_w == 0 )
					break;
				SIXEL_mouse_y = (key->params[2] - 1) * SIXEL_pixel_h / SIXEL_cell_h;
				SIXEL_mouse_x = (key->params[1] - 1) * SIXEL_pixel_w / SIXEL_cell_w;
				switch (key->params[0]) {
				case 0:
					if (SIXEL_mouse_button & 1) {
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 1, 0, 0);
						SIXEL_mouse_button ^= 1;
					}
					break;
				case 1:
					if (SIXEL_mouse_button & 2) {
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 2, 0, 0);
						SIXEL_mouse_button ^= 2;
					}
					break;
				case 2:
					if (SIXEL_mouse_button & 4) {
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 3, 0, 0);
						SIXEL_mouse_button ^= 4;
					}
					break;
				default:
					break;
				}
				break;
			case SIXEL_MOUSE_DEC:
				if (key->nparams >= 4) {
					SIXEL_mouse_y = key->params[2];
					SIXEL_mouse_x = key->params[3];
					//prev_x = prev_y = -1;
					switch ( key->params[0] ) {
					case 1:
						break;
					case 2:
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 1, 0, 0);
						SIXEL_mouse_button |= 1;
						break;
					case 3:
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 1, 0, 0);
						SIXEL_mouse_button &= ~1;
						break;
					case 4:
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 2, 0, 0);
						SIXEL_mouse_button |= 2;
						break;
					case 5:
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 2, 0, 0);
						SIXEL_mouse_button &= ~2;
						break;
					case 6:
						posted += SDL_PrivateMouseButton(SDL_PRESSED, 3, 0, 0);
						SIXEL_mouse_button |= 4;
						break;
					case 7:
						posted += SDL_PrivateMouseButton(SDL_RELEASED, 3, 0, 0);
						SIXEL_mouse_button &= ~4;
						break;
					case 32:
					case 64:
					default:
						break;
					}
					SIXEL_mouse_button = key->params[1];
				}
				printf("\033['|");
				fflush(stdout);
				break;
			case SIXEL_FKEYS:
				keysym.scancode = key->value;
				keysym.mod = KMOD_NONE;
				keysym.unicode = 0;
				switch ( key->params[0] ) {
				case 2:
					keysym.sym = SDLK_INSERT;
					break;
				case 3:
					keysym.sym = SDLK_DELETE;
					break;
				case 5:
					keysym.sym = SDLK_PAGEUP;
					break;
				case 6:
					keysym.sym = SDLK_PAGEDOWN;
					break;
				case 7:
					keysym.sym = SDLK_HOME;  /* RXVT */
					break;
				case 8:
					keysym.sym = SDLK_END;  /* RXVT */
					break;
				case 11:
					keysym.sym = SDLK_F1;  /* RXVT */
					break;
				case 12:
					keysym.sym = SDLK_F2;  /* RXVT */
					break;
				case 13:
					keysym.sym = SDLK_F3;  /* RXVT */
					break;
				case 14:
					keysym.sym = SDLK_F4;  /* RXVT */
					break;
				case 15:
					keysym.sym = SDLK_F5;
					break;
				case 17:
					keysym.sym = SDLK_F6;
					break;
				case 18:
					keysym.sym = SDLK_F7;
					break;
				case 19:
					keysym.sym = SDLK_F8;
					break;
				case 20:
					keysym.sym = SDLK_F9;
					break;
				case 21:
					keysym.sym = SDLK_F10;
					break;
				case 23:
					keysym.sym = SDLK_F11;
					break;
				case 24:
					keysym.sym = SDLK_F12;
					break;
				default:
					keysym.sym = SDLK_UNKNOWN;
					break;
				}
				keysym.scancode = GetKsymScancode(keysym.sym);
				if (key->nparams == 2) {
					key->params[1]--;
					posted += SendModifierKey(key->params[1], SDL_PRESSED);
				}
				posted += SDL_PrivateKeyboard(SDL_PRESSED, &keysym);
				if (key->nparams == 2) {
					posted += SendModifierKey(key->params[1], SDL_RELEASED);
				}
				posted += SDL_PrivateKeyboard(SDL_RELEASED, &keysym);
				break;
			default:
				if ( (key->value >= SIXEL_UP && key->value <= SIXEL_LEFT) ||
					(key->value >= SIXEL_END && key->value <= SIXEL_HOME) ||
					(key->value >= SIXEL_F1 && key->value <= SIXEL_F4) ) {
					keysym.mod = KMOD_NONE;
					keysym.unicode = 0;
					switch(key->value) {
					case SIXEL_UP: keysym.sym = SDLK_UP; break;
					case SIXEL_DOWN: keysym.sym = SDLK_DOWN; break;
					case SIXEL_RIGHT: keysym.sym = SDLK_RIGHT; break;
					case SIXEL_LEFT: keysym.sym = SDLK_LEFT; break;
					case SIXEL_HOME: keysym.sym = SDLK_HOME; break;
					case SIXEL_END: keysym.sym = SDLK_END; break;
					case SIXEL_F1: keysym.sym = SDLK_F1; break;
					case SIXEL_F2: keysym.sym = SDLK_F2; break;
					case SIXEL_F3: keysym.sym = SDLK_F3; break;
					default: keysym.sym = SDLK_F4; break;
					}
					keysym.scancode = GetKsymScancode(keysym.sym);
					if (key->nparams >= 1) {
						key->params[key->nparams-1]--;
						posted += SendModifierKey(key->params[key->nparams-1], SDL_PRESSED);
					}
					posted += SDL_PrivateKeyboard(SDL_PRESSED, &keysym);
					if (key->nparams >= 1) {
						posted += SendModifierKey(key->params[key->nparams-1], SDL_RELEASED);
					}
					posted += SDL_PrivateKeyboard(SDL_RELEASED, &keysym);
				}
				else {
					int state = GetState(key->value);
					if (state) SendModifierKey(state, SDL_PRESSED);
					posted += SDL_PrivateKeyboard(SDL_PRESSED, TranslateKey(key->value, &keysym));
					if (state) SendModifierKey(state, SDL_RELEASED);
					posted += SDL_PrivateKeyboard(SDL_RELEASED, TranslateKey(key->value, &keysym));
				}
				break;
			}
		}
	}

	if ( prev_x != SIXEL_mouse_x || prev_y != SIXEL_mouse_y ) {
		SDL_Lock_EventThread();
		SDL_PrivateAppActive(1, SDL_APPMOUSEFOCUS);
		posted += SDL_PrivateMouseMotion(0, 0, SIXEL_mouse_x, SIXEL_mouse_y);
		prev_x = SIXEL_mouse_x;
		prev_y = SIXEL_mouse_y;
		SDL_Unlock_EventThread();
	}

#if SIXEL_DEBUG
	printf("\033[32;1Hevents: %5d button: [%1d] cursor: (%3d, %3d)\n",
		events++,
		SIXEL_mouse_button,
		SIXEL_mouse_x, SIXEL_mouse_y);
#endif
}

void SIXEL_InitOSKeymap(_THIS)
{
	int i;
	static const char *std_keys = " 01234567890&#'()_-|$*+-=/\\:;.,!?<>{}[]@~%^\x9";
	const char *std;

	/* Initialize the AAlib key translation table */
	for ( i=0; i<SDL_arraysize(keymap); ++i )
		keymap[i] = SDLK_UNKNOWN;

	/* Alphabet keys */
	for ( i=0; i<26; ++i ){
		keymap['a' + i] = SDLK_a + i;
		keymap['A' + i] = SDLK_a + i;
	}
	/* Function keys */
	for ( i = 0; i < 12; ++i ){
		keymap[334 + i] = SDLK_F1+i;
	}
	/* Keys that have the same symbols and don't have to be translated */
	for(std = std_keys; *std; std ++) {
		keymap[(int)(*std)] = *std;
	}

	keymap[0x17] = SDLK_LSUPER;
	keymap[0x0d] = SDLK_RETURN;
	keymap[0x08] = SDLK_BACKSPACE;
	keymap[0x1b] = SDLK_ESCAPE;
	keymap[0x7f] = SDLK_BACKSPACE;

	keymap[369] = SDLK_LSHIFT;
	keymap[370] = SDLK_RSHIFT;
	keymap[371] = SDLK_LCTRL;
	keymap[372] = SDLK_RCTRL;
	keymap[377] = SDLK_LALT;
	keymap[270] = SDLK_RALT;
	keymap[271] = SDLK_NUMLOCK;
	keymap[373] = SDLK_CAPSLOCK;
	keymap[164] = SDLK_SCROLLOCK;

	keymap[243] = SDLK_INSERT;
	keymap[304] = SDLK_DELETE;
	keymap[224] = SDLK_HOME;
	keymap[231] = SDLK_END;
	keymap[229] = SDLK_PAGEUP;
	keymap[230] = SDLK_PAGEDOWN;

	keymap[241] = SDLK_PRINT;
	keymap[163] = SDLK_BREAK;

	keymap[302] = SDLK_KP0;
	keymap[300] = SDLK_KP1;
	keymap[297] = SDLK_KP2;
	keymap[299] = SDLK_KP3;
	keymap[294] = SDLK_KP4;
	keymap[301] = SDLK_KP5;
	keymap[296] = SDLK_KP6;
	keymap[293] = SDLK_KP7;
	keymap[295] = SDLK_KP8;
	keymap[298] = SDLK_KP9;

	keymap[SIXEL_UP] = SDLK_UP;
	keymap[SIXEL_DOWN] = SDLK_DOWN;
	keymap[SIXEL_LEFT] = SDLK_LEFT;
	keymap[SIXEL_RIGHT] = SDLK_RIGHT;
	keymap[SIXEL_F1] = SDLK_F1;
	keymap[SIXEL_F2] = SDLK_F2;
	keymap[SIXEL_F3] = SDLK_F3;
	keymap[SIXEL_F4] = SDLK_F4;

}

static int SendModifierKey(int state, Uint8 press_state)
{
	SDL_keysym  keysym;
	int posted = 0;

	if (state & 1) {
		posted += SDL_PrivateKeyboard(press_state, TranslateKey(369 /* SDLK_LSHIFT */, &keysym));
	}
	if (state & 2) {
		posted += SDL_PrivateKeyboard(press_state, TranslateKey(377 /* SDLK_LALT */, &keysym));
	}
	if (state & 4) {
		posted += SDL_PrivateKeyboard(press_state, TranslateKey(371 /* SDLK_LCTRL */, &keysym));
	}
	return posted;
}

static char* GetKbdType(void)
{
	char* env;
	env = getenv("SDL_SIXEL_KBD");
	if (env) {
		return env;
	}
	else {
		return "";
	}
}

static int GetScancode(int code)
{
	static u_char us101_kbd_tbl[] = {
		 0,  0,  0,  0,  0,  0,  0,  0, 14, 15, 28,  0,  0, 28,  0,  0, /* 0x0 - 0xf */
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0, /* 0x10 - 0x1f */
		57,  2, 40,  4,  5,  6,  8, 40, 10, 11,  9, 13, 51, 12, 52, 53, /* 0x20 - 0x2f */
		11,  2,  3,  4,  5,  6,  7,  8,  9, 10, 39, 39, 51, 13, 52, 53, /* 0x30 - 0x3f */
		 3, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x40 - 0x4f */
		25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 26, 43, 27,  7, 12, /* 0x50 - 0x5f */
		41, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x60 - 0x6f */
		25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 26, 43, 27, 41,  0, /* 0x70 - 0x7f */
	};
	static u_char jp106_kbd_tbl[] = {
		 0,  0,  0,  0,  0,  0,  0,  0, 14, 15, 28,  0,  0, 28,  0,  0, /* 0x0 - 0xf */
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  0,  0,  0, /* 0x10 - 0x1f */
		57,  2,  3,  4,  5,  6,  7,  8,  9, 10, 40, 39, 51, 12, 52, 53, /* 0x20 - 0x2f */
		11,  2,  3,  4,  5,  6,  7,  8,  9, 10, 40, 39, 51, 12, 52, 53, /* 0x30 - 0x3f */
		26, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x40 - 0x4f */
		25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 27, 43, 43, 13, 12, /* 0x50 - 0x5f */
		26, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, /* 0x60 - 0x6f */
		25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 27, 43, 43, 13,  0, /* 0x70 - 0x7f */
	};
	static u_char* tbl;

	if (!tbl) {
		if (strcmp(GetKbdType(),"jp106") == 0) {
			tbl = jp106_kbd_tbl;
		}
		else {
			tbl = us101_kbd_tbl;
		}
	}

	if (code == 369 /* SDLK_LSHIFT */) {
		return 42+8;
	}
	else if(code == 377 /* SDLK_LALT */) {
		return 56+8;
	}
	else if(code == 371 /* SDLK_LCTRL */) {
		return 29+8;
	}
	else if(code <= 0x7f && tbl[code] > 0)
	{
		return tbl[code]+8;
	}
	else {
		return 0;
	}
}

static int GetKsymScancode(SDLKey sym)
{
	switch(sym) {
	case SDLK_INSERT: return 110+8;
	case SDLK_DELETE: return 111+8;
	case SDLK_PAGEUP: return 104+8;
	case SDLK_PAGEDOWN: return 109+8;
	case SDLK_F1: return 59+8;
	case SDLK_F2: return 60+8;
	case SDLK_F3: return 61+8;
	case SDLK_F4: return 62+8;
	case SDLK_F5: return 63+8;
	case SDLK_F6: return 64+8;
	case SDLK_F7: return 65+8;
	case SDLK_F8: return 66+8;
	case SDLK_F9: return 67+8;
	case SDLK_F10: return 68+8;
	case SDLK_F11: return 87+8;
	case SDLK_F12: return 88+8;
	case SDLK_RIGHT: return 106+8;
	case SDLK_LEFT: return 105+8;
	case SDLK_UP: return 103+8;
	case SDLK_DOWN: return 108+8;
	case SDLK_HOME: return 102+8;
	case SDLK_END: return 107+8;
	default: return 0;
	}
}

static int GetState(int code)
{
	if (code < 0x20) {
		if (GetScancode(code) == 0) {
			return 4;	/* Control */
		}
	}
	else if (code <= 0x7f) {
		static u_char us101_kbd_tbl[] = {
			0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 0x20 - 0x2f */
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, /* 0x30 - 0x3f */
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40 - 0x4f */
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, /* 0x50 - 0x5f */
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x60 - 0x6f */
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, /* 0x70 - 0x7f */
		};
		static u_char jp106_kbd_tbl[] = {
			0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, /* 0x20 - 0x2f */
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* 0x30 - 0x3f */
			0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40 - 0x4f */
			1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, /* 0x50 - 0x5f */
			1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x60 - 0x6f */
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, /* 0x70 - 0x7f */
		};
		static u_char* tbl;

		if (!tbl) {
			if (strcmp(GetKbdType(),"jp106") == 0) {
				tbl = jp106_kbd_tbl;
			}
			else {
				tbl = us101_kbd_tbl;
			}
		}

		/* Shift */
		return tbl[code - 0x20];
	}
	return 0;
}

static SDL_keysym *TranslateKey(int scancode, SDL_keysym *keysym)
{
	/* Sanity check */
	if ( scancode >= SDL_arraysize(keymap) )
		scancode = SIXEL_UNKNOWN;

	/* Set the keysym information */
	keysym->scancode = GetScancode(scancode);
	if (keysym->scancode == 0 && scancode < 0x20) {
		/* It seems Ctrl+N key */
		keysym->scancode = GetScancode(scancode+0x60);
		keysym->sym = keymap[scancode+0x60];
	}
	else {
		keysym->sym = keymap[scancode];
	}
	keysym->mod = KMOD_NONE;

	/* If UNICODE is on, get the UNICODE value for the key */
	keysym->unicode = 0;
	if ( SDL_TranslateUNICODE ) {
		/* Populate the unicode field with the ASCII value */
		if (scancode <= 0x7f) {
			keysym->unicode = scancode;
		}
	}
	return(keysym);
}


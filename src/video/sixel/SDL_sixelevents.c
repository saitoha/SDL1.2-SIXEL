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

static const int SIXEL_UP = 1 << 12 | 'A' - '@';
static const int SIXEL_DOWN = 1 << 12 | 'B' - '@';
static const int SIXEL_LEFT = 1 << 12 | 'C' - '@';
static const int SIXEL_RIGHT = 1 << 12 | 'D' - '@';
static const int SIXEL_MOUSE_SGR = 1 << 12 | ('<' - ';') << 4 << 6 | 'M' - '@';
static const int SIXEL_MOUSE_SGR_RELEASE = 1 << 12 | ('<' - ';') << 4 << 6 | 'm' - '@';
static const int SIXEL_MOUSE_DEC = 1 << 12 | ('&' - 0x1f) << 6 | 'w' - '@';
static const int SIXEL_DTTERM_SEQS = 1 << 12 | 't' - '@';
static const int SIXEL_UNKNOWN = 513;

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

int prefix_state = 0;
int lock_state = 0;
int lock_key = -1;
int state = STATE_GROUND;
int ibytes = 0;
int pbytes = 0;

/* The translation tables from a console scancode to a SDL keysym */
static SDLKey keymap[1 << 13];


static SDL_keysym *TranslateKey(int scancode, SDL_keysym *keysym);


static int get_input(char *buf, int size) {
	fd_set fdset;
	struct timeval timeout;
	FD_ZERO(&fdset);
	FD_SET(STDIN_FILENO, &fdset);
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if ( select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == 1 )
		return read(STDIN_FILENO, buf, size);
	return 0;
}

static int getkeys(char *buf, int nread, sixel_key_t *keys)
{
	int i, c;
	int size = 0;

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
				ibytes |= c - ' ';
				state = STATE_CSI_PARAM;
				break;
			case '0'...'9':
				pbytes = pbytes * 10 + c - '0';
				state = STATE_CSI_PARAM;
				break;
			case ':'...';':
				if (keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params)) {
					keys[size].params[keys[size].nparams++] = pbytes;
					pbytes = 0;
				}
				break;
			case '@'...'~':
				if (keys[size].nparams < sizeof(keys[size].params) / sizeof(*keys[size].params)) {
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
	static int mouse_button = -1, mouse_x = -1, mouse_y = -1;
	static int prev_button = -1, prev_x = -1, prev_y = -1;
#if SIXEL_DEBUG
	static int events = 0;
#endif
	static sixel_key_t keys[4096];
	SDL_keysym keysym;
	char buf[4096];
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
				mouse_y = (key->params[2] - 1) * (SIXEL_pixel_h / SIXEL_cell_h);
				mouse_x = (key->params[1] - 1) * (SIXEL_pixel_w / SIXEL_cell_w);
				switch (key->params[0]) {
				case 0:
					posted += SDL_PrivateMouseButton(SDL_PRESSED, 1, 0, 0);
					break;
				case 1:
					posted += SDL_PrivateMouseButton(SDL_PRESSED, 2, 0, 0);
					break;
				case 2:
					posted += SDL_PrivateMouseButton(SDL_PRESSED, 3, 0, 0);
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
				mouse_y = (key->params[2] - 1) * SIXEL_pixel_h / SIXEL_cell_h;
				mouse_x = (key->params[1] - 1) * SIXEL_pixel_w / SIXEL_cell_w;
				switch (key->params[0]) {
				case 0:
					posted += SDL_PrivateMouseButton(SDL_RELEASED, 1, 0, 0);
					break;
				case 1:
					posted += SDL_PrivateMouseButton(SDL_RELEASED, 2, 0, 0);
					break;
				case 2:
					posted += SDL_PrivateMouseButton(SDL_RELEASED, 3, 0, 0);
					break;
				default:
					break;
				}
				break;
			case SIXEL_MOUSE_DEC:
				printf("%d\n", key->nparams);
				exit(0);
				break;
			default:
				if ( key->value >= 'A' && key->value <= 'Z' ) {
					posted += SDL_PrivateKeyboard(SDL_PRESSED, TranslateKey(369 /* SDLK_LSHIFT */, &keysym));
					posted += SDL_PrivateKeyboard(SDL_PRESSED, TranslateKey(key->value, &keysym));
					posted += SDL_PrivateKeyboard(SDL_RELEASED, TranslateKey(key->value, &keysym));
					posted += SDL_PrivateKeyboard(SDL_RELEASED, TranslateKey(369 /* SDLK_LSHIFT */, &keysym));
				} else {
					posted += SDL_PrivateKeyboard(SDL_PRESSED, TranslateKey(key->value, &keysym));
					posted += SDL_PrivateKeyboard(SDL_RELEASED, TranslateKey(key->value, &keysym));
				}
				break;
			}
		}
	}

	if ( prev_x != mouse_x && prev_y != mouse_y ) {
		if ( mouse_y > this->screen->h )
			mouse_y = this->screen->h - 1;
		if ( mouse_x > this->screen->w )
			mouse_x = this->screen->w - 1;
#if 0
		SDL_GetMouseState(&prev_x, &prev_y);
		posted += SDL_PrivateMouseMotion(0, 1, mouse_x * 640/1024 - prev_x, mouse_y * 480/768 - prev_y);
#else
		posted += SDL_PrivateMouseMotion(0, 0, mouse_x, mouse_y);
#endif
		prev_x = mouse_x; prev_y = mouse_y;
	}
	prev_button = mouse_button;
# if SIXEL_DEBUG
	printf("\033[29;1Hevents: %5d [%d] (%d, %d), (%d, %d), (%d, %d), (%d, %d)\n",
		events++, keys[0].value,
		prev_x, prev_y,
		SIXEL_pixel_h, SIXEL_pixel_w,
		SIXEL_cell_h, SIXEL_cell_w,
		this->screen->w, this->screen->h);
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

}

static SDL_keysym *TranslateKey(int scancode, SDL_keysym *keysym)
{
	/* Sanity check */
	if ( scancode >= SDL_arraysize(keymap) )
		scancode = SIXEL_UNKNOWN;

	/* Set the keysym information */
	keysym->scancode = scancode;
	keysym->sym = keymap[scancode];
	keysym->mod = KMOD_NONE;

	/* If UNICODE is on, get the UNICODE value for the key */
	keysym->unicode = 0;
	if ( SDL_TranslateUNICODE ) {
		/* Populate the unicode field with the ASCII value */
		keysym->unicode = scancode;
	}
	return(keysym);
}


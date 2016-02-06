/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 2003  Sam Hocevar

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    Sam Hocevar
    sam@zoy.org
*/

/* libsixel based SDL video driver implementation.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#if SDL_VIDEO_OPENGL_OSMESA
# include <GL/osmesa.h>
#endif
#include "SDL.h"
#include "SDL_error.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_sixelvideo.h"
#include "SDL_sixelevents_c.h"

#include <sixel.h>
#include <termios.h>
#include <math.h>

/* Initialization/Query functions */
static int SIXEL_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **SIXEL_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *SIXEL_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static void SIXEL_VideoQuit(_THIS);
#if SDL_VIDEO_OPENGL_OSMESA
static void *SIXEL_GL_GetProcAddress(_THIS, const char* proc);
static int SIXEL_GL_LoadLibrary(_THIS, const char* path);
static int SIXEL_GL_GetAttribute(_THIS, SDL_GLattr attrib, int* value);
static int SIXEL_GL_MakeCurrent(_THIS);
static void SIXEL_GL_SwapBuffers(_THIS);
#endif

/* Various screen update functions available */
static void SIXEL_UpdateRects(_THIS, int numrects, SDL_Rect *rects);

/* Hardware surface functions */
static int SIXEL_AllocHWSurface(_THIS, SDL_Surface *surface);
static int SIXEL_LockHWSurface(_THIS, SDL_Surface *surface);
static int SIXEL_FlipHWSurface(_THIS, SDL_Surface *surface);
static void SIXEL_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void SIXEL_FreeHWSurface(_THIS, SDL_Surface *surface);

/* #define SIXEL_VIDEO_DEBUG 1 */
/* Caption control */
static void SIXEL_SetCaption(_THIS, const char *title, const char *icon);

/* Cache the VideoDevice struct */
static struct SDL_VideoDevice *local_this;

struct termios orig_termios;

static void tty_raw(void)
{
	struct termios raw;

	if ( tcgetattr(fileno(stdin), &orig_termios) < 0 )
		perror("can't set raw mode");
	raw = orig_termios;
	raw.c_iflag &= ~(/*BRKINT |*/ ICRNL /*| INPCK | ISTRIP | IXON*/);
	raw.c_lflag &= ~(ECHO | ICANON /*| IEXTEN | ISIG*/);
	raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
	raw.c_cc[VINTR] = 0; raw.c_cc[VKILL] = 0; raw.c_cc[VQUIT] = 0;
	raw.c_cc[VSTOP] = 0; raw.c_cc[VSUSP] = 0;
	if ( tcsetattr(fileno(stdin), TCSAFLUSH, &raw) < 0 )
		perror("can't set raw mode");
}

int tty_restore(void)
{
	if ( tcsetattr(fileno(stdin), TCSAFLUSH, &orig_termios) < 0 )
		return -1;
	return 0;
}

/* libsixel driver bootstrap functions */

static int SIXEL_Available(void)
{
	return 1; /* Always available ! */
}

static void SIXEL_DeleteDevice(SDL_VideoDevice *device)
{
	free(device->hidden);
	free(device);
}


void SIXEL_UpdateMouse(_THIS)
{
	SDL_PrivateMouseMotion (0, 0, SIXEL_mouse_x, SIXEL_mouse_y);
}

static SDL_VideoDevice *SIXEL_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	tty_raw();
	printf("\033c");
	printf("\033[?25l");
	printf("\033[?1003h");
	printf("\033[?1006h");
	printf("\033[>2p");
#if 1
	printf("\033[1;1'z\033[3'{\033[1'{");
	printf("\033['|");
#endif

	/* Initialize all variables that we clean on shutdown */
	device = (SDL_VideoDevice *)malloc(sizeof(SDL_VideoDevice));
	if ( device ) {
		memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
				malloc((sizeof *device->hidden));
	}
	if ( (device == NULL) || (device->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( device ) {
			free(device);
		}
		return(0);
	}
	memset(device->hidden, 0, (sizeof *device->hidden));

	/* Set the function pointers */
	device->VideoInit = SIXEL_VideoInit;
	device->ListModes = SIXEL_ListModes;
	device->SetVideoMode = SIXEL_SetVideoMode;
	device->CreateYUVOverlay = NULL;
	device->SetColors = NULL;
	device->UpdateRects = NULL;
	device->VideoQuit = SIXEL_VideoQuit;
#if SDL_VIDEO_OPENGL_OSMESA
	device->GL_LoadLibrary = SIXEL_GL_LoadLibrary;
	device->GL_GetProcAddress = SIXEL_GL_GetProcAddress;
	device->GL_GetAttribute = SIXEL_GL_GetAttribute;
	device->GL_MakeCurrent = SIXEL_GL_MakeCurrent;
	device->GL_SwapBuffers = SIXEL_GL_SwapBuffers;
#endif

	device->AllocHWSurface = SIXEL_AllocHWSurface;
	device->CheckHWBlit = NULL;
	device->FillHWRect = NULL;
	device->SetHWColorKey = NULL;
	device->SetHWAlpha = NULL;
	device->LockHWSurface = SIXEL_LockHWSurface;
	device->UnlockHWSurface = SIXEL_UnlockHWSurface;
	device->FlipHWSurface = SIXEL_FlipHWSurface;
	device->FreeHWSurface = SIXEL_FreeHWSurface;
	device->SetCaption = SIXEL_SetCaption;
	device->SetIcon = NULL;
	device->IconifyWindow = NULL;
	device->GrabInput = NULL;
	device->GetWMInfo = NULL;
	device->UpdateMouse = SIXEL_UpdateMouse;
	device->InitOSKeymap = SIXEL_InitOSKeymap;
	device->PumpEvents = SIXEL_PumpEvents;
	device->free = SIXEL_DeleteDevice;

	return device;
}

VideoBootStrap SIXEL_bootstrap = {
	"sixel", "SIXEL terminal",
	SIXEL_Available, SIXEL_CreateDevice
};

static int sixel_write(char *data, int size, void *priv)
{
	return fwrite(data, 1, size, (FILE *)priv);
}

int SIXEL_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	int i;

	/* Initialize all variables that we clean on shutdown */
	for ( i=0; i<SDL_NUMMODES; ++i ) {
		SDL_modelist[i] = malloc(sizeof(SDL_Rect));
		SDL_modelist[i]->x = SDL_modelist[i]->y = 0;
	}
	/* Modes sorted largest to smallest */
	SDL_modelist[0]->w = 1024; SDL_modelist[0]->h = 768;
	SDL_modelist[1]->w = 800; SDL_modelist[1]->h = 600;
	SDL_modelist[2]->w = 640; SDL_modelist[2]->h = 480;
	SDL_modelist[3]->w = 320; SDL_modelist[3]->h = 400;
	SDL_modelist[4]->w = 320; SDL_modelist[4]->h = 240;
	SDL_modelist[5]->w = 320; SDL_modelist[5]->h = 200;
	SDL_modelist[6] = NULL;

#if SDL_VIDEO_OPENGL_OSMESA
	this->gl_config.driver_loaded = 1;
#endif
	SIXEL_mutex = SDL_CreateMutex();

	/* Initialize the library */

	/* Initialize private variables */
	SIXEL_bitmap = NULL;
	SIXEL_buffer = NULL;

	local_this = this;

	/* Determine the screen depth (use default 8-bit depth) */
	vformat->BitsPerPixel = 24;
	vformat->BytesPerPixel = 3;
	SIXEL_output = sixel_output_create(sixel_write, stdout);
	SIXEL_dither = sixel_dither_get(BUILTIN_XTERM256);
#if 0
	sixel_dither_set_diffusion_type(SIXEL_dither, DIFFUSE_FS);
#endif

	/* We're done! */
	return(0);
}

SDL_Rect **SIXEL_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	if(format->BitsPerPixel != 24)
		return NULL;

	 if ( flags & SDL_FULLSCREEN ) {
		return SDL_modelist;
	} else {
		return (SDL_Rect **) -1;
	}
}

SDL_Surface *SIXEL_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	if ( SIXEL_buffer ) {
		free( SIXEL_buffer );
		SIXEL_buffer = NULL;
	}
	SDL_PrivateAppActive(1, SDL_APPINPUTFOCUS | SDL_APPMOUSEFOCUS);

	SIXEL_buffer = calloc(1, 4 * width * height);
	if ( ! SIXEL_buffer ) {
		SDL_SetError("Couldn't allocate buffer for requested mode");
		return(NULL);
	}
	SIXEL_bitmap = calloc(1, 4 * width * height);
	if ( ! SIXEL_bitmap ) {
		SDL_SetError("Couldn't allocate buffer for requested mode");
		return(NULL);
	}

	/* Allocate the new pixel format for the screen */
	if (!SDL_ReallocFormat(current, 24, 0x0000ff, 0x00ff00, 0xff0000, 0)) {
		return(NULL);
	}

	/* Set up the new mode framebuffer */
	current->flags = SDL_FULLSCREEN;
	SIXEL_w = current->w = width;
	SIXEL_h = current->h = height;
	SIXEL_pixel_w = 0;
	SIXEL_pixel_h = 0;
	SIXEL_cell_w = 0;
	SIXEL_cell_h = 0;
	SIXEL_mouse_x = width / 2;
	SIXEL_mouse_y = height / 2;
	SIXEL_mouse_button = 0;
	SIXEL_update_rect.x = -1;
	SIXEL_update_rect.y = -1;
	SIXEL_update_rect.w = -1;
	SIXEL_update_rect.h = -1;
#if SDL_VIDEO_OPENGL_OSMESA
	SIXEL_glcontext = OSMesaCreateContextExt(GL_RGB, 24, 0, 0, NULL);
#endif
	current->pitch = width * 3;
	current->pixels = SIXEL_buffer;
#if SDL_VIDEO_OPENGL_OSMESA
	current->flags |= SDL_OPENGL;
#endif

	/* Set the blit function */
	this->UpdateRects = SIXEL_UpdateRects;

	printf("\033[14t\033[18t\n");

	/* We're done */
	return(current);
}

#define min(lhs, rhs) ((lhs) < (rhs) ? (lhs): (rhs))
#define max(lhs, rhs) ((lhs) > (rhs) ? (lhs): (rhs))

static int SIXEL_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	return 0;
}

static void SIXEL_FreeHWSurface(_THIS, SDL_Surface *surface)
{
}

static int SIXEL_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return 0;
}

static void SIXEL_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
}

static void SIXEL_SetCaption(_THIS, const char *title, const char *icon)
{
	printf("\033]1;%s\033\\", icon);
	printf("\033]2;%s\033\\", title);
}

static int SIXEL_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	int start_row = 1;
	int start_col = 1;

	memcpy(SIXEL_bitmap, SIXEL_buffer, SIXEL_h * SIXEL_w * 3);
	printf("\033[%d;%dH", start_row, start_col);
	sixel_encode(SIXEL_bitmap, SIXEL_w, SIXEL_h, 3, SIXEL_dither, SIXEL_output);
	fflush(stdout);

	return 0;
}

static void SIXEL_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	int start_row = 1, start_col = 1;
	int cell_height = 0, cell_width = 0;
	int i, y;
	unsigned char *src, *dst;
#if SIXEL_VIDEO_DEBUG
	static int frames = 0;
	char *format;
#endif
	SDL_mutexP(SIXEL_mutex);

	if ( SIXEL_cell_h != 0 && SIXEL_pixel_h != 0 ) {
		for (i = 0; i < numrects; ++i, ++rects) {
			start_row = 1;
			start_col = 1;
			cell_height = SIXEL_pixel_h / SIXEL_cell_h;
			cell_width = SIXEL_pixel_w / SIXEL_cell_w;
			start_row += rects->y / cell_height;
			start_col += rects->x / cell_width;
			rects->h += rects->y - (start_row - 1) * cell_height;
			rects->w += rects->x - (start_col - 1) * cell_width;
			rects->y = (start_row - 1) * cell_height;
			rects->x = (start_col - 1) * cell_width;
			rects->h = min(((rects->y + rects->h) / cell_height + 1) * cell_height, SIXEL_h) - rects->y;
			rects->w = min(((rects->x + rects->w) / cell_width + 1) * cell_width, SIXEL_w) - rects->x;

			if ( rects->x == 0 && rects->w == SIXEL_w ) {
				dst = SIXEL_bitmap;
				src = SIXEL_buffer + rects->y * SIXEL_w * 3;
				memcpy(dst, src, rects->h * SIXEL_w * 3);
			} else {
				for (y = rects->y; y < rects->y + rects->h; ++y) {
					dst = SIXEL_bitmap + (y - rects->y) * rects->w * 3;
					src = SIXEL_buffer + y * SIXEL_w * 3 + rects->x * 3;
					memcpy(dst, src, rects->w * 3);
				}
			}
			printf("\033[%d;%dH", start_row, start_col);
			sixel_encode(SIXEL_bitmap, rects->w, rects->h, 3, SIXEL_dither, SIXEL_output);
#if SIXEL_VIDEO_DEBUG
			format = "\033[100;1Hframes: %05d, x: %04d, y: %04d, w: %04d, h: %04d";
			printf(format, ++frames, rects->x, rects->y, rects->w, rects->h);
#endif
		}
	} else {
		memcpy(SIXEL_bitmap, SIXEL_buffer, SIXEL_h * SIXEL_w * 3);
		printf("\033[%d;1H", start_row);
		sixel_encode(SIXEL_bitmap, SIXEL_w, SIXEL_h, 3, SIXEL_dither, SIXEL_output);
	}
	fflush(stdout);
	SDL_mutexV(SIXEL_mutex);
}

/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects.
*/
void SIXEL_VideoQuit(_THIS)
{
	int i;

	tty_restore();

	printf("\033\\");
#if USE_DECMOUSE
#else
	printf("\033[?1006l");
#endif
	printf("\033[>0p");
	printf("\033[?1003l");
	printf("\033[?25h");

	sixel_dither_unref(SIXEL_dither);
	sixel_output_unref(SIXEL_output);

	/* Free video mode lists */
	for ( i=0; i<SDL_NUMMODES; ++i ) {
		if ( SDL_modelist[i] != NULL ) {
			free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
		}
	}

	SDL_DestroyMutex(SIXEL_mutex);
}

typedef struct sixel_gl_funcmap
{
	const char *name;
	void *func;
} sixel_gl_funcmap_t;

#if SDL_VIDEO_OPENGL_OSMESA
void SIXEL_GL_SwapBuffers(_THIS)
{
	int start_row = 1;
	int start_col = 1;

	glFlush();
	memcpy(SIXEL_bitmap, SIXEL_buffer, SIXEL_h * SIXEL_w * 3);
	printf("\033[%d;%dH", start_row, start_col);
	sixel_encode(SIXEL_bitmap, SIXEL_w, SIXEL_h, 3, SIXEL_dither, SIXEL_output);
}
#endif

#if SDL_VIDEO_OPENGL_OSMESA
void *SIXEL_GL_GetProcAddress(_THIS, const char* proc)
{
	int i;
	static sixel_gl_funcmap_t map[] = {
		{"glBegin", glBegin},
		{"glBindTexture", glBindTexture},
		{"glBlendFunc", glBlendFunc},
		{"glColor4f", glColor4f},
		{"glDisable", glDisable},
		{"glEnable", glEnable},
		{"glEnd", glEnd},
		{"glFlush", glFlush},
		{"glGenTextures", glGenTextures},
		{"glGetString", glGetString},
		{"glLoadIdentity", glLoadIdentity},
		{"glMatrixMode", glMatrixMode},
		{"glOrtho", glOrtho},
		{"glPixelStorei", glPixelStorei},
		{"glPopAttrib", glPopAttrib},
		{"glPopClientAttrib", glPopClientAttrib},
		{"glPopMatrix", glPopMatrix},
		{"glPushAttrib", glPushAttrib},
		{"glPushClientAttrib", glPushClientAttrib},
		{"glPushMatrix", glPushMatrix},
		{"glTexCoord2f", glTexCoord2f},
		{"glTexEnvf", glTexEnvf},
		{"glTexImage2D", glTexImage2D},
		{"glTexParameteri", glTexParameteri},
		{"glTexSubImage2D", glTexSubImage2D},
		{"glVertex2i", glVertex2i},
		{"glViewport", glViewport},
	};
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (strcmp(map[i].name, proc) == 0) {
			return map[i].func;
		}
	}
	return NULL;
}
#endif

#if SDL_VIDEO_OPENGL_OSMESA
/* Passing a NULL path means load pointers from the application */
int SIXEL_GL_LoadLibrary(_THIS, const char* path)
{
	return 0;
}
#endif

#if SDL_VIDEO_OPENGL_OSMESA
/* Get attribute data from glX. */
int SIXEL_GL_GetAttribute(_THIS, SDL_GLattr attrib, int* value)
{
	int retval = -1;
	int unsupported = 0;
	int sixel_attrib = 0;

	switch( attrib ) {
	case SDL_GL_RED_SIZE:
		break;
	case SDL_GL_GREEN_SIZE:
		break;
	case SDL_GL_BLUE_SIZE:
		break;
	case SDL_GL_ALPHA_SIZE:
		break;
	case SDL_GL_DOUBLEBUFFER:
		break;
	case SDL_GL_BUFFER_SIZE:
		break;
	case SDL_GL_DEPTH_SIZE:
		break;
	case SDL_GL_STENCIL_SIZE:
		break;
	case SDL_GL_ACCUM_RED_SIZE:
		break;
	case SDL_GL_ACCUM_GREEN_SIZE:
		break;
	case SDL_GL_ACCUM_BLUE_SIZE:
		break;
	case SDL_GL_ACCUM_ALPHA_SIZE:
		break;
	case SDL_GL_STEREO:
		break;
	case SDL_GL_MULTISAMPLEBUFFERS:
		break;
	case SDL_GL_MULTISAMPLESAMPLES:
		break;
	case SDL_GL_ACCELERATED_VISUAL:
		break;
	case SDL_GL_SWAP_CONTROL:
		break;
	default:
		unsupported = 1;
		break;
	}

	if (unsupported) {
		SDL_SetError("OpenGL attribute is unsupported on this system");
	}
	return retval;
}
#endif

#if SDL_VIDEO_OPENGL_OSMESA
/* Make the current context active */
int SIXEL_GL_MakeCurrent(_THIS)
{
	int retval;

	retval = 0;

	if ( ! SIXEL_buffer) {
		SDL_SetError("Unable to make GL context current 0");
		retval = -1;
	}
	if ( ! SIXEL_glcontext) {
		SDL_SetError("Unable to make GL context current 1");
		retval = -1;
	}
	if ( ! OSMesaMakeCurrent(SIXEL_glcontext, (void*)SIXEL_buffer, GL_UNSIGNED_BYTE, SIXEL_w, SIXEL_h)) {
		SDL_SetError("Unable to make GL context current 2");
		retval = -1;
	}
	return(retval);
}
#endif

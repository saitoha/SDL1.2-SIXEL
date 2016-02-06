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

#ifndef _SDL_sixelvideo_h
#define _SDL_sixelvideo_h

#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "SDL_mutex.h"

#include <sys/time.h>
#include <time.h>

#include <sixel.h>

/* Hidden "this" pointer for the video functions */
#define _THIS SDL_VideoDevice *this

#define SDL_NUMMODES 6

/* Private display data */
struct SDL_PrivateVideoData {
	SDL_Rect *SDL_modelist[SDL_NUMMODES+1];
	SDL_mutex *mutex;

	sixel_dither_t *dither;
	sixel_output_t *output;

	unsigned char *bitmap;
	unsigned char *buffer;
	int w, h;
	int pixel_w, pixel_h;
	int cell_w, cell_h;
	int mouse_x, mouse_y;
	int mouse_button;
	SDL_Rect update_rect;
#if SDL_VIDEO_OPENGL_OSMESA
	void *glcontext;
#endif
};

/* Old variable names */
#define SDL_modelist		(this->hidden->SDL_modelist)
#define SIXEL_palette		(this->hidden->palette)
#define SIXEL_bitmap		(this->hidden->bitmap)
#define SIXEL_buffer		(this->hidden->buffer)

#define SIXEL_w			(this->hidden->w)
#define SIXEL_h			(this->hidden->h)
#define SIXEL_pixel_w		(this->hidden->pixel_w)
#define SIXEL_pixel_h		(this->hidden->pixel_h)
#define SIXEL_cell_w		(this->hidden->cell_w)
#define SIXEL_cell_h		(this->hidden->cell_h)
#define SIXEL_mouse_x		(this->hidden->mouse_x)
#define SIXEL_mouse_y		(this->hidden->mouse_y)
#define SIXEL_mouse_button	(this->hidden->mouse_button)

#define SIXEL_output		(this->hidden->output)
#define SIXEL_dither		(this->hidden->dither)

#define SIXEL_mutex		(this->hidden->mutex)
#define SIXEL_update_rect	(this->hidden->update_rect)
#if SDL_VIDEO_OPENGL_OSMESA
# define SIXEL_glcontext		(this->hidden->glcontext)
#endif

#endif /* _SDL_sixelvideo_h */

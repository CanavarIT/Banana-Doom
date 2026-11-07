// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
//
// $Log:$
//
// DESCRIPTION:
//	Main loop menu stuff.
//	Default Config File.
//	PCX Screenshots.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_misc.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>


#include "doomdef.h"

#include "z_zone.h"

#include "m_swap.h"
#include "m_argv.h"

#include "w_wad.h"

#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

#include "hu_stuff.h"

// State.
#include "doomstat.h"

// Data.
#include "dstrings.h"

#include "m_misc.h"

//
// M_DrawText
// Returns the final X coordinate
// HU_Init must have been called to init the font
//
extern patch_t*		hu_font[HU_FONTSIZE];

int
M_DrawText
( int		x,
  int		y,
  boolean	direct,
  char*		string )
{
    int 	c;
    int		w;

    while (*string)
    {
	c = toupper(*string) - HU_FONTSTART;
	string++;
	if (c < 0 || c> HU_FONTSIZE)
	{
	    x += 4;
	    continue;
	}
		
	w = SHORT (hu_font[c]->width);
	if (x+w > SCREENWIDTH)
	    break;
	if (direct)
	    V_DrawPatchDirect(x, y, 0, hu_font[c]);
	else
	    V_DrawPatch(x, y, 0, hu_font[c]);
	x+=w;
    }

    return x;
}




//
// M_WriteFile
//
#ifndef O_BINARY
#define O_BINARY 0
#endif

boolean
M_WriteFile
( char const*	name,
  void*		source,
  int		length )
{
    int		handle;
    int		count;
	
    handle = open ( name, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);

    if (handle == -1)
	return false;

    count = write (handle, source, length);
    close (handle);
	
    if (count < length)
	return false;
		
    return true;
}


//
// M_ReadFile
//
int
M_ReadFile
( char const*	name,
  byte**	buffer )
{
    int	handle, count, length;
    struct stat	fileinfo;
    byte		*buf;
	
    handle = open (name, O_RDONLY | O_BINARY);
    if (handle == -1)
	I_Error ("Couldn't read file %s", name);
    if (fstat (handle,&fileinfo) == -1)
	I_Error ("Couldn't read file %s", name);
    length = fileinfo.st_size;
    buf = Z_Malloc (length, PU_STATIC, NULL);
    count = read (handle, buf, length);
    close (handle);
	
    if (count < length)
	I_Error ("Couldn't read file %s", name);
		
    *buffer = buf;
    return length;
}


//
// DEFAULTS (integers only; stored in default.cfg in the app's data dir)
//
int		usemouse;
int		usejoystick;

extern int	mouseSensitivity;
extern int	showMessages;
extern int	detailLevel;
extern int	screenblocks;

typedef struct
{
    const char*	name;
    int*	location;
    int		defaultvalue;
} default_t;

static default_t defaults[] =
{
    {"mouse_sensitivity",&mouseSensitivity, 5},
    {"sfx_volume",&snd_SfxVolume, 12},
    {"music_volume",&snd_MusicVolume, 8},
    {"show_messages",&showMessages, 1},
    {"screenblocks",&screenblocks, 9},
    {"detaillevel",&detailLevel, 0},
    {"usegamma",&usegamma, 1},
};

#define numdefaults ((int)(sizeof(defaults)/sizeof(defaults[0])))
char*	defaultfile;

/* control bindings (were read from the config file in the original) */
extern int key_right, key_left, key_up, key_down, key_strafeleft, key_straferight,
	   key_fire, key_use, key_strafe, key_speed;
extern int mousebfire, mousebstrafe, mousebforward, joybfire, joybstrafe, joybuse, joybspeed;

static void M_DefaultBindings (void)
{
    key_right = KEY_RIGHTARROW;  key_left = KEY_LEFTARROW;
    key_up = KEY_UPARROW;        key_down = KEY_DOWNARROW;
    key_strafeleft = ',';        key_straferight = '.';
    key_fire = KEY_RCTRL;        key_use = ' ';
    key_strafe = KEY_RALT;       key_speed = KEY_RSHIFT;
    mousebfire = 0; mousebstrafe = 1; mousebforward = 2;
    joybfire = 0; joybstrafe = 1; joybuse = 3; joybspeed = 2;
}

void M_SaveDefaults (void)
{
    int		i;
    FILE*	f = fopen (defaultfile, "w");

    if (!f)
	return;
    for (i=0 ; i<numdefaults ; i++)
	fprintf (f,"%s\t\t%i\n",defaults[i].name,*defaults[i].location);
    fclose (f);
}

void M_LoadDefaults (void)
{
    int		i;
    FILE*	f;
    char	def[80];
    int		parm;

    M_DefaultBindings ();
    for (i=0 ; i<numdefaults ; i++)
	*defaults[i].location = defaults[i].defaultvalue;

    defaultfile = basedefault;

    f = fopen (defaultfile, "r");
    if (f)
    {
	while (fscanf (f, "%79s %i", def, &parm) == 2)
	    for (i=0 ; i<numdefaults ; i++)
		if (!strcmp(def, defaults[i].name))
		{
		    *defaults[i].location = parm;
		    break;
		}
	fclose (f);
    }
}

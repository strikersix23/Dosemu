/* 
 * (C) Copyright 1992, ..., 2004 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

#include "config.h"

#include <SDL/SDL.h>

#include "emu.h"
#include "keyb_clients.h"
#include "keyboard.h"
#include "keysym_attributes.h"
#include "video.h"
#include "sdl.h"

static int use_move_key(t_keysym key)
{
	int result = FALSE;
        /* If it's some kind of function key move it
         * otherwise just make sure it gets pressed
         */
        if (is_keysym_function(key) ||
            is_keysym_dosemu_key(key) ||
            is_keypad_keysym(key) ||
            (key == KEY_TAB) ||
            (key == KEY_RETURN) ||
            (key == KEY_BKSP)) {
                result = TRUE;
        }
	return result;
}

static t_modifiers map_SDL_modifiers(SDLMod e_state)
{
        t_modifiers modifiers = 0;
        if (e_state & KMOD_SHIFT) {
                modifiers |= MODIFIER_SHIFT;
        }
        if (e_state & KMOD_CTRL) {
                modifiers |= MODIFIER_CTRL;
        }
        if (e_state & KMOD_LALT) {
                modifiers |= MODIFIER_ALT;
        }
        if (e_state & KMOD_RALT) {
                modifiers |= MODIFIER_ALTGR;
        }
        if (e_state & KMOD_CAPS) {
                modifiers |= MODIFIER_CAPS;
        }
        if (e_state & KMOD_NUM) {
                modifiers |= MODIFIER_NUM;
        }
        if (e_state & KMOD_MODE) {
                modifiers |= MODIFIER_SCR;
        }
#if 0
        if (e_state & X_mi.InsLockMask) {
                modifiers |= MODIFIER_INS;
        }
#endif
        return modifiers;
}

static void SDL_sync_shiftstate(Boolean make, SDLKey kc, SDLMod e_state)
{
	t_modifiers shiftstate = get_shiftstate();

	/* Check for modifiers released outside this window */
	if (!!(shiftstate & MODIFIER_SHIFT) != !!(e_state & KMOD_SHIFT)) {
		shiftstate ^= MODIFIER_SHIFT;
	}
	if (!!(shiftstate & MODIFIER_CTRL) != !!(e_state & KMOD_CTRL)) {
		shiftstate ^= MODIFIER_CTRL;
	}
	if (!!(shiftstate & MODIFIER_ALT) != !!(e_state & KMOD_LALT)) {
		shiftstate ^= MODIFIER_ALT;
	}
	if (!!(shiftstate & MODIFIER_ALTGR) != !!(e_state & KMOD_RALT)) {
		shiftstate ^= MODIFIER_ALTGR;
	}

	if (!!(shiftstate & MODIFIER_CAPS) != !!(e_state & KMOD_CAPS)
		&& (make || (kc != SDLK_CAPSLOCK))) {
		shiftstate ^= MODIFIER_CAPS;
	}
	if (!!(shiftstate & MODIFIER_NUM) != !!(e_state & KMOD_NUM)
		&& (make || (kc != SDLK_NUMLOCK))) {
		shiftstate ^= MODIFIER_NUM;
	}
	if (!!(shiftstate & MODIFIER_SCR) != !!(e_state & KMOD_MODE)
		&& (make || (kc != SDLK_SCROLLOCK))) {
		shiftstate ^= MODIFIER_SCR;
	}
#if 0
	if (!!(shiftstate & MODIFIER_INS) != !!(e_state & X_mi.InsLockMask)) {
		shiftstate ^= MODIFIER_INS;
	}
#endif
	set_shiftstate(shiftstate);
}

void SDL_process_key(SDL_KeyboardEvent keyevent)
{
	SDL_keysym keysym = keyevent.keysym;
	t_unicode key = keysym.unicode;
	t_modifiers modifiers = map_SDL_modifiers(keysym.mod);

	switch (keysym.sym) {
	  case SDLK_CAPSLOCK:
		key = KEY_CAPS;
		break;

	  case SDLK_NUMLOCK:
		key = KEY_NUM;
		break;
		
	  case SDLK_F1 ... SDLK_F10:
		key = (keysym.sym - SDLK_F1) + KEY_F1 ;
		break;
#define DOKEY(x) case SDLK_##x: key = KEY_##x; break;
	  DOKEY(F11)
	  DOKEY(F12)
	  DOKEY(HOME)
	  DOKEY(LEFT)
	  DOKEY(UP)
	  DOKEY(RIGHT)
	  DOKEY(DOWN)
	  DOKEY(END)
	  case SDLK_PAGEUP:
		key = KEY_PGUP;
		break;
	  case SDLK_PAGEDOWN:
		key = KEY_PGDN;
		break;
	  case SDLK_DELETE:
		key = KEY_DEL;
		break;
		
	  case SDLK_INSERT:
		key = KEY_INS;
		break;		
		
	  case SDLK_BACKSPACE:
		key = KEY_BKSP;
		break;
		
	  case SDLK_RETURN:
		key = KEY_RETURN;		
		break;		
		
	  case SDLK_TAB:
		key = KEY_TAB;
		break;
		
	  case SDLK_ESCAPE:
		key = KEY_ESC;
		break;

	  case SDLK_RSHIFT: key = KEY_R_SHIFT ; break;		
	  case SDLK_LSHIFT: key = KEY_L_SHIFT ; break;		
	  case SDLK_RCTRL: key = KEY_R_CTRL ; break;		
	  case SDLK_LCTRL: key = KEY_L_CTRL ; break;		
	  case SDLK_RMETA:
/*	  case SDLK_MODE: */
	  case SDLK_RALT: key = KEY_R_ALT ; break;		
	  case SDLK_LMETA:
	  case SDLK_LALT: key = KEY_L_ALT ; break;		

	  default: break;
	 }

	SDL_sync_shiftstate(keyevent.state==SDL_PRESSED, keysym.sym, keysym.mod);

        if (!use_move_key(key) || (move_key(keyevent.state == SDL_PRESSED, key) < 0)) {
                put_modified_symbol(keyevent.state==SDL_PRESSED, modifiers, key);
        }
}

static int probe_SDL_keyb(void)
{
	int result = FALSE;
	if (Video == &Video_SDL) {
		result = TRUE;
	}
	return result;
}

struct keyboard_client Keyboard_SDL =  {
	"SDL",                  /* name */
	probe_SDL_keyb,         /* probe */
	NULL,                   /* init */
	NULL,                   /* reset */
	NULL,                   /* close */
	NULL,                   /* run */       
	NULL                    /* set_leds */
};
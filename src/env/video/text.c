/* 
 * (C) Copyright 1992, ..., 2004 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/*
 * Debug level for the X interface.
 * 0 - normal / 1 - useful / 2 - too much
 */
#define DEBUG_X		0

/* "fine tuning" option for X_update_screen */
#define MAX_UNCHANGED	3

#define x_msg(x...) X_printf("X: " x)

#if DEBUG_X >= 1
#define x_deb(x...) X_printf("X: " x)
#else
#define x_deb(x...)
#endif

#if DEBUG_X >= 2
#define x_deb2(x...) X_printf("X: " x)
#else
#define x_deb2(x...)
#endif

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "emu.h"
#include "bios.h"
#include "video.h"
#include "memory.h"
#include "vgaemu.h"
#include "remap.h"
#include "vgatext.h"
#include "render.h"

#ifdef HAVE_UNICODE_TRANSLATION
#include "translate.h"
#endif

struct text_system * Text = NULL;
int use_bitmap_font = TRUE;
Boolean have_focus = FALSE;

static int prev_cursor_row = -1, prev_cursor_col = -1;
static ushort prev_cursor_shape = NO_CURSOR;
static int blink_state = 1;
static int blink_count = 8;

#if CONFIG_SELECTION
static int sel_start_row = -1, sel_end_row = -1, sel_start_col, sel_end_col;
static unsigned short *sel_start = NULL, *sel_end = NULL;
static u_char *sel_text = NULL;
static Boolean doing_selection = FALSE, visible_selection = FALSE;
#endif


/* Kludge for incorrect ASCII 0 char in vga font. */
#define XCHAR(w) (((u_char)CHAR(w)||use_bitmap_font)?(u_char)CHAR(w):(u_char)' ')

#if CONFIG_SELECTION
#define SEL_ACTIVE(w) (visible_selection && ((w) >= sel_start) && ((w) <= sel_end))
static inline Bit8u sel_attr(Bit8u a)
{
  /* adapted from Linux vgacon code */
  if (vga.mode_type == TEXT_MONO)
    a ^= ((a & 0x07) == 0x01) ? 0x70 : 0x77;
  else
    a = (a & 0x88) | ((a & 0x70) >> 4) | ((a & 0x07) << 4);
  return a;
}
#define XATTR(w) (SEL_ACTIVE(w) ? sel_attr(ATTR(w)) : ATTR(w))
#else
#define XATTR(w) (ATTR(w))
#endif

#define XREAD_WORD(w) ((XATTR(w)<<8)|XCHAR(w))

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int register_text_system(struct text_system *text_system)
{
  Text = text_system;
  return 1;
}

/*
 * Draw a text string.
 * The attribute is the VGA color/mono text attribute.
 */
static void draw_string(int x, int y, char *text, int len, Bit8u attr)
{
  x_deb2(
    "X_draw_string: %d chars at (%d, %d), attr = 0x%02x\n",
    len, x, y, (unsigned) attr
  );
  
  Text->Draw_string(x, y, text, len, attr);
  if(vga.mode_type == TEXT_MONO && (attr == 0x01 || attr == 0x09 || attr == 0x89)) {
    Text->Draw_line(x, y, len);
  }
}


/*
 * Restore a character cell (used to remove the cursor).
 * Do nothing in graphics modes.
 */
static void restore_cell(int x, int y)
{
  Bit16u *sp, *oldsp;
  u_char c;

  /* no hardware cursor emulation in graphics modes (erik@sjoerd) */
  if(vga.mode_class == GRAPH) return;

  if (use_bitmap_font) {
    li = vga.text_height;
    co = vga.scan_len / 2;
  }
  
  /* don't draw it if it's out of bounds */
  if(y < 0 || y >= li || x < 0 || x >= co) return;

  sp = screen_adr + y * co + x;
  oldsp = prev_screen + y * co + x;
  c = XCHAR(sp);

  *oldsp = XREAD_WORD(sp);
  draw_string(x, y, &c, 1, XATTR(sp));
}

/*
 * Draw the cursor (nothing in graphics modes, normal if we have focus,
 * rectangle otherwise).
 */
static void draw_cursor(int x, int y)
{  
  /* no hardware cursor emulation in graphics modes (erik@sjoerd) */
  if(vga.mode_class == GRAPH) return;

  /* don't draw it if it's out of bounds */
  if(cursor_row < 0 || cursor_row >= li) return;
  if(cursor_col < 0 || cursor_col >= co) return;
  if(blink_state || !have_focus)
    Text->Draw_cursor(cursor_col, cursor_row, XATTR(screen_adr + y * co + x),
		      CURSOR_START(cursor_shape), CURSOR_END(cursor_shape),
		      have_focus);
}

/*
 * Move cursor to a new position (and erase the old cursor).
 * Do nothing in graphics modes.
 */
static void redraw_cursor(void)
{
  if(prev_cursor_shape != NO_CURSOR)
    restore_cell(prev_cursor_col, prev_cursor_row);

  if(cursor_shape != NO_CURSOR)
    draw_cursor(cursor_col, cursor_row);

  Text->Update();

  prev_cursor_row = cursor_row;
  prev_cursor_col = cursor_col;
  prev_cursor_shape = cursor_shape;
}

RectArea draw_bitmap_cursor(int x, int y, Bit8u attr, int start, int end, Boolean focus)
{
  int cstart, cend; 
  int i,j;
  int fg = ATTR_FG(attr);
  int len = co * vga.char_width;
  unsigned char *  deb;

  deb = remap_obj.src_image + vga.char_width * x + len *(vga.char_height*  y);
  if (focus) {
    cstart = start * vga.char_height / 16;
    cend = end * vga.char_height / 16;
    deb += len * cstart;
    for (i = 0; i < cend - cstart + 1; i++) {
      for (j = 0; j < vga.char_width; j++) {
	*deb++ = fg;
      }
      deb += len - vga.char_width;
    }
  } else {
    /* Draw only a rectangle */
    for (j = 0; j < vga.char_width; j ++)
      *deb++ = fg;
    deb += len - vga.char_width;
    for (i = 0; i < vga.char_height - 2; i ++) { 
      *deb++ = fg;
      deb += vga.char_width - 2;
      *deb++ = fg;
      deb += len - vga.char_width;
    }
    for (j = 0; j < vga.char_width; j ++)
      *deb++ = fg;
  }
  return remap_obj.remap_rect(&remap_obj, vga.char_width * x, vga.char_height * y,
			      vga.char_width, vga.char_height);
}

void reset_redraw_text_screen(void)
{
  prev_cursor_shape = NO_CURSOR; redraw_cursor();

  /* Comment Eric: If prev_screen is too small, we must update */
  /* everything continuously anyway, sigh...                   */
  /* so we better cheat and clip co / li / ..., danger >:->.   */
  if (vga.scan_len * vga.text_height > 65535) {
    if (vga.scan_len > MAX_COLUMNS * 2) vga.scan_len = MAX_COLUMNS * 2;
    if (vga.text_width > MAX_COLUMNS  ) vga.text_width = MAX_COLUMNS;
    if (vga.text_height > MAX_LINES   ) vga.text_height = MAX_LINES;
  }
  if (2 * co * li > 65535) {
    if (co > MAX_COLUMNS) co = MAX_COLUMNS;
    if (li > MAX_LINES  ) li = MAX_LINES;
  }
  MEMCPY_2UNIX(prev_screen, screen_adr, vga.scan_len * vga.text_height);
}

/*
 * Reallocate color cells if a text color has changed. If no
 * free color cell is left, choose an approximate color.
 *
 * Note: Redraws the *entire* screen if at least one color has changed.
 */
static void refresh_text_palette(void)
{
  DAC_entry col[16];
  int j, k;

  if(vga.pixel_size > 4) {
    X_printf("X: refresh_text_palette: invalid color size - no updates made\n");
    return;
  }

  j = changed_vga_colors(col);

  for(k = 0; k < j; k++) {
    Text->SetPalette(col[k]);
    if (use_bitmap_font)
      remap_obj.palette_update(&remap_obj, col[k].index, vga.dac.bits,
			       col[k].r, col[k].g, col[k].b);
  }

  if(j) redraw_text_screen();
}

/*
 * Redraw the entire screen (in text modes). Used only for expose events.
 * It's graphics mode counterpart is a simple put_ximage() call
 * (cf. X_handle_events). Since we now use backing store in text modes,
 * this function will likely never be called (depending on X's configuration).
 *
 * Note: It redraws the *entire* screen.
 */
void redraw_text_screen()
{
  Bit16u *sp, *oldsp;
  u_char charbuff[MAX_COLUMNS], *bp;
  int x, y, start_x;
  Bit8u attr;

  if(vga.mode_class == GRAPH) {
    x_msg("X_redraw_text_screen: Text refresh in graphics video mode?\n");
    return;
  }
  x_msg("X_redraw_text_screen: all\n");

  if(vga.reconfig.display || vga.reconfig.mem) {
    if(vga.reconfig.display) {
      vga.reconfig.display = 0;
      Text->Resize_text_screen();
    }
    vga.reconfig.mem = 0;
    co = vga.text_width;
    li = vga.text_height;
  }

  if(co > MAX_COLUMNS) {
    x_msg("X_redraw_text_screen: unable to handle %d colums\n", co);
    return;
  }

  x_deb(
    "X_redraw_text_screen: mode 0x%x (%d x %d), screen_adr = 0x%x\n",
    vga.mode, co, li, (unsigned) screen_adr
  );

  /* sp = (Bit16u *) (vga.mem.base + vga.display_start); */

  sp = screen_adr;
  oldsp = prev_screen;

  for(y = 0; y < li; y++) {
    x = 0;
    do {	/* scan in a string of chars of the same attribute */
      bp = charbuff; start_x = x; attr = XATTR(sp);

      do {	/* conversion of string to X */
        *oldsp++ = XREAD_WORD(sp); *bp++ = XCHAR(sp);
        sp++; x++;
      } while(XATTR(sp) == attr && x < co);

      draw_string(start_x, y, charbuff, x - start_x, attr);
    } while(x < co);
    if (co * 2 < vga.scan_len) {
      sp += vga.scan_len / 2 - co;
      oldsp += vga.scan_len / 2 - co;
    }
  }

  reset_redraw_text_screen();
}

/*
 * Redraw the cursor if it's necessary.
 * Do nothing in graphics modes.
 */
void update_cursor(void)
{
  if(
    cursor_row != prev_cursor_row ||
    cursor_col != prev_cursor_col ||
    cursor_shape != prev_cursor_shape
  ) {
    redraw_cursor();
  }
}

/*
 * Blink the cursor. Called from SIGALRM handler.
 * Do nothing in graphics modes.
 */
void blink_cursor()
{
  /* no hardware cursor emulation in graphics modes (erik@sjoerd) */
  if(vga.mode_class == GRAPH) return;
  if(Text->Draw_cursor == NULL) return;

  if(!have_focus || --blink_count) return;

  blink_count = config.X_blinkrate;
  blink_state = !blink_state;

  if(cursor_shape != NO_CURSOR) {
    if(
      cursor_row != prev_cursor_row ||
      cursor_col != prev_cursor_col
    ) {
      restore_cell(prev_cursor_col, prev_cursor_row);
      prev_cursor_row = cursor_row;
      prev_cursor_col = cursor_col;
      prev_cursor_shape = cursor_shape;
    }

    if(blink_state)
      draw_cursor(cursor_col, cursor_row);
    else
      restore_cell(cursor_col, cursor_row);
  }
}

/*
 * Resize everything according to vga.*
 */
void resize_text_mapper(int image_mode)
{
  static char *text_canvas = NULL;

  /* need a remap obj for the font system even in text mode! */
  x_msg("X_setmode to text mode: Get remapper for Erics fonts\n");

  remap_done(&remap_obj);

  /* linear 1 byte per pixel */
  remap_obj = remap_init(MODE_PSEUDO_8, image_mode, remap_features);
  adjust_gamma(&remap_obj, config.X_gamma);

  text_canvas = remap_obj.src_image = realloc(text_canvas, 1 * vga.width * vga.height);
  if (remap_obj.src_image == NULL)
    error("X: cannot allocate text mode canvas for font simulation\n");
  remap_obj.src_resize(&remap_obj, vga.width, vga.height, 1 * vga.width);

  dirty_all_video_pages();
  /*
   * The new remap object does not yet know about our colors.
   * So we have to force an update. -- sw
   */
  dirty_all_vga_colors();

  vga.reconfig.mem =
    vga.reconfig.display =
    vga.reconfig.dac = 0;
}

RectArea convert_bitmap_string(int x, int y, unsigned char *text, int len,
			       Bit8u attr)
{
  unsigned src, height, xx, yy, cc, srcp, srcp2, bits;
  unsigned long fgX;
  unsigned long bgX;
  static int last_redrawn_line = -1;
  RectArea ra;
  ra.width = 0;

  if (y >= vga.text_height) return ra;                /* clip */
  if (x >= vga.text_width)  return ra;                /* clip */
  if (x+len > vga.text_width) len = vga.text_width - x;  /* clip */

  /* fgX = text_colors[ATTR_FG(attr)]; */ /* if no remapper used */
  /* bgX = text_colors[ATTR_BG(attr)]; */ /* if no remapper used */
  fgX = ATTR_FG(attr);
  bgX = ATTR_BG(attr);

  /* we could set fgX = bgX: vga.attr.data[0x10] & 0x08 enables  */
  /* blinking to be triggered by (attr & 0x80) - but the third   */
  /* condition would need to be periodically and having to redo  */
  /* all blinking chars again each time that they blink sucks.   */
  /* so we ALWAYS interpret blinking as bright background, which */
  /* is what also happens when not vga.attr.data[0x10] & 0x08... */
  /* An IDEA would be to have palette animation and use special  */
  /* colors for the bright-or-blinking background, although the  */
  /* official blink would be the foreground, not the background. */

  /* Eric: What type is our remap_obj.src_mode at this moment??? */
  /* not sure if I use the remap object at least roughly correct */
  /* basically, it is like two Ximages, linked by remapping...   */


  height = vga.char_height; /* not font_height - should start to */
                            /* remove font_height completely. It */
                            /* holds the X font's size...        */
  src = vga.seq.fontofs[(attr & 8) >> 3];

  if (y != last_redrawn_line) /* have some less output */
    X_printf(
	     "X_draw_string(x=%d y=%d len=%d attr=%d %dx%d @ 0x%04x)\n",
	     x, y, len, attr, vga.char_width, height, src);
  last_redrawn_line = y;

  if ( ((y+1) * height) > vga.height ) {
    v_printf("Tried to print below scanline %d (row %d)\n",
	     remap_obj.src_height, y);
    return ra;
  }
  if ( ((x+len) * vga.char_width) > vga.width ) {
    v_printf("Tried to print past right margin\n");
    v_printf("x=%d len=%d vga.char_width=%d width=%d\n",
	     x, len, vga.char_width, remap_obj.src_width);
    len = vga.width / vga.char_width - x;
  }

  /* would use vgaemu_xy2ofs, but not useable for US, NOW! */
  srcp = remap_obj.src_scan_len * y * height;
  srcp += x * vga.char_width;

  /* vgaemu -> vgaemu_put_char would edit the vga.mem.base[...] */
  /* but as vga memory is used as text buffer at this moment... */
  for (yy = 0; yy < height; yy++) {
    srcp2 = srcp;
    for (cc = 0; cc < len; cc++) {
      bits = vga.mem.base[0x20000 + src + (32 * (unsigned char)text[cc])];
      for (xx = 0; xx < 8; xx++) {
	remap_obj.src_image[srcp2++]
	  = (bits & 0x80) ? fgX : bgX;
	bits <<= 1;
      }
      if (vga.char_width >= 9) { /* copy 8th->9th for line gfx */
	/* (only if enabled by bit... */
	if ( (vga.attr.data[0x10] & 0x04) &&
	     ((text[cc] & 0xc0) == 0xc0) ) {
	  remap_obj.src_image[srcp2] = remap_obj.src_image[srcp2-1];
	  srcp2++;
	} else {             /* ...or fill with background */
	  remap_obj.src_image[srcp2++] = bgX;
	}
	srcp2 += (vga.char_width - 9);
      }   /* (pixel-x has reached on next char now) */
    }
    srcp += remap_obj.src_scan_len;      /* next line */
    src++;  /* globally shift to the next font row!!! */
  }

  return remap_obj.remap_rect(&remap_obj, vga.char_width * x, height * y,
			      vga.char_width * len, height);    
}

/*
 * Update the text screen.
 */
int update_text_screen(void)
{
  Bit16u *sp, *oldsp;
  u_char charbuff[MAX_COLUMNS], *bp;
  int x, y;	/* X and Y position of character being updated */
  int start_x, len, unchanged;
  Bit8u attr;

  static int yloop = -1;
  int lines;               /* Number of lines to redraw. */
  int numscan = 0;         /* Number of lines scanned. */
  int numdone = 0;         /* Number of lines actually updated. */

  if (use_bitmap_font) {
    li = vga.text_height;
    co = vga.text_width;
  }
  
  refresh_text_palette();

  if(vga.reconfig.display) {
    Text->Resize_text_screen();
    vga.reconfig.display = 0;
  }
  if(vga.reconfig.mem) {
    if (use_bitmap_font)
      remap_obj.src_resize(&remap_obj, vga.width, vga.height, vga.width);
    redraw_text_screen();
    vga.reconfig.mem = 0;
  }

  /* The following determines how many lines it should scan at once,
   * since this routine is being called by sig_alrm.  If the entire
   * screen changes, it often incurs considerable delay when this
   * routine updates the entire screen.  So the variable "lines"
   * contains the maximum number of lines to update at once in one
   * call to this routine.  This is set by the "updatelines" keyword
   * in /etc/dosemu.conf 
   */
	lines = config.X_updatelines;
	if (lines < 2) 
	  lines = 2;
	else if (lines > li)
	  lines = li;

  /* The highest priority is given to the current screen row for the
   * first iteration of the loop, for maximum typing response.  
   * If y is out of bounds, then give it an invalid value so that it
   * can be given a different value during the loop.
   */
	y = cursor_row;
	if ((y < 0) || (y >= li)) 
	  y = -1;

/*  X_printf("X: X_update_screen, co=%d, li=%d, yloop=%d, y=%d, lines=%d\n",
           co,li,yloop,y,lines);*/

  /* The following loop scans lines on the screen until the maximum number
   * of lines have been updated, or the entire screen has been scanned.
   */
	while ((numdone < lines) && (numscan < li)) 
	  {
    /* The following sets the row to be scanned and updated, if it is not
     * the first iteration of the loop, or y has an invalid value from
     * loop pre-initialization.
     */
	    if ((numscan > 0) || (y < 0)) 
	      {
		yloop = (yloop+1) % li;
		if (yloop == cursor_row)
		  yloop = (yloop+1) % li;
		y = yloop;
	      }
	    numscan++;
	    
	    sp = screen_adr + y*co;
	    oldsp = prev_screen + y*co;
	    if (use_bitmap_font || co * 2 < vga.scan_len) {
	      sp = screen_adr + y * vga.scan_len / 2;
	      oldsp = prev_screen + y * vga.scan_len / 2;
	    }

	    x=0;
	    do 
	      {
        /* find a non-matching character position */
		start_x = x;
		while (XREAD_WORD(sp)==*oldsp) {
		  sp++; oldsp++; x++;
		  if (x==co) {
		    if (start_x == 0)
		      goto chk_cursor;
		    else
		      goto line_done;
		  }
		}
/* now scan in a string of changed chars of the same attribute.
   To keep the number of X calls (and thus the overhead) low,
   we tolerate a few unchanged characters (up to MAX_UNCHANGED in 
   a row) in the 'changed' string. 
*/
		bp = charbuff;
		start_x=x;
		attr=XATTR(sp);
		unchanged=0;         /* counter for unchanged chars */
		
		while(1) 
		  {
		    *bp++=XCHAR(sp);
		    *oldsp++ = XREAD_WORD(sp);
		    sp++; 
		    x++;

		    if ((XATTR(sp) != attr) || (x == co))
		      break;
		    if (XREAD_WORD(sp) == *oldsp) {
		      if (unchanged > MAX_UNCHANGED) 
			break;
		      unchanged++;
		    }
		    else
		      unchanged=0;
		  } 
		len=x-start_x-unchanged;

                /* ok, we've got the string now send it to the X server */

                draw_string(start_x, y, charbuff, len, attr);

		if ((prev_cursor_row == y) && 
		    (prev_cursor_col >= start_x) && 
		    (prev_cursor_col < start_x+len))
		  {
		    prev_cursor_shape=NO_CURSOR;  
/* old cursor was overwritten */
		  }
	      } 
	    while(x<co);
line_done:
/* Increment the number of successfully updated lines counter */
	    numdone++;

chk_cursor:
/* update the cursor. We do this here to avoid the cursor 'running behind'
       when using a fast key-repeat.
*/
	    if (y == cursor_row)
	      update_cursor();
	  }
	Text->Update();

/*	X_printf("X: X_update_screen: %d lines updated\n",numdone);*/
	
	if (numdone) 
	  {
            if ((use_bitmap_font && numscan==vga.text_height) ||
                (!use_bitmap_font && numscan==li))
              return 1;     /* changed, entire screen updated */
            else
              return 2;     /* changed, part of screen updated */
	  }
	else 
	  {
/* The updates look a bit cleaner when reset to top of the screen
 * if nothing had changed on the screen in this call to screen_restore
 */
	    yloop = -1;
	    return(1);
	  }
      
  return 0;
}

/*
 * Set the text mode resolution.
 */
void set_textsize(int width, int height)
{
  v_printf("set_textsize: size = %d x %d\n", width, height);
  vga_emu_set_textsize(width, height);
  Text->Resize_text_screen();
}

void text_lose_focus()
{
  have_focus = FALSE;
  blink_state = TRUE;
  blink_count = config.X_blinkrate;
  redraw_cursor();   
}

void text_gain_focus()
{
  have_focus = TRUE;
  redraw_cursor();   
}

#if CONFIG_SELECTION

/*
 * Calculate sel_start and sel_end pointers from sel_start | end_col | row.
 */
static void calculate_selection(void)
{
  if (use_bitmap_font) {
    li = vga.text_height;
    co = vga.scan_len / 2;
  }
  if ((sel_end_row < sel_start_row) || 
    ((sel_end_row == sel_start_row) && (sel_end_col < sel_start_col)))
  {
    sel_start = screen_adr+sel_end_row*co+sel_end_col;
    sel_end = screen_adr+sel_start_row*co+sel_start_col-1;
  }
  else
  {
    sel_start = screen_adr+sel_start_row*co+sel_start_col;
    sel_end = screen_adr+sel_end_row*co+sel_end_col-1;
  }
}


/*
 * Clear visible part of selection (but not the selection text buffer).
 */
static void clear_visible_selection(void)
{
  sel_start_col = sel_start_row = sel_end_col = sel_end_row = 0;
  sel_start = sel_end = NULL;
  visible_selection = FALSE;
}


/*
 * Free the selection text buffer.
 */
void clear_selection_data(void)
{
  X_printf("X: Selection data cleared\n");
  if (sel_text != NULL)
  {
    free(sel_text);
    sel_text = NULL;
  }
  doing_selection = FALSE;
  clear_visible_selection();
}


/*
 * Check if we should clear selection.
 * Clear if cursor is in or before selected area.
*/
void clear_if_in_selection()
{
  if (!visible_selection)
    return;

  X_printf("X:clear check selection , cursor at %d %d\n",
	   cursor_col,cursor_row);
  if (((sel_start_row <= cursor_row)&&(cursor_row <= sel_end_row)&&
       (sel_start_col <= cursor_col)&&(cursor_col <= sel_end_col)) ||
      /* cursor either inside selectio */
       (( cursor_row <= sel_start_row)&&(cursor_col <= sel_start_col)))
      /* or infront of selection */
    {
      X_printf("X:selection clear, key-event!\n");
      clear_visible_selection(); 
    }
}


/*
 * Start the selection process (mouse button 1 pressed).
 */
void start_selection(int col, int row)
{
  sel_start_col = sel_end_col = col;
  sel_start_row = sel_end_row = row;
  calculate_selection();
  doing_selection = visible_selection = TRUE;
  X_printf("X:start selection , start %d %d, end %d %d\n",
	   sel_start_col,sel_start_row,sel_end_col,sel_end_row);
}


/*
 * Extend the selection (mouse motion).
 */
void extend_selection(int col, int row)
{
  if (!doing_selection)
    return;
  if ((sel_end_col == col) && (sel_end_row == row))
    return;
  sel_end_col = col;
  sel_end_row = row;
  calculate_selection();
  X_printf("X:extend selection , start %d %d, end %d %d\n",
	   sel_start_col,sel_start_row,sel_end_col,sel_end_row);
}


/*
 * Start extending the selection (mouse button 3 pressed).
 */
void start_extend_selection(int col, int row)
{
  /* Try to extend selection, visibility is handled by extend_selection*/
    doing_selection =  visible_selection = TRUE;  
    extend_selection(col, row);
  
}

#ifndef HAVE_UNICODE_TRANSLATION

static void save_selection(int col1, int row1, int col2, int row2)
{
  size_t i;
  int row, col, line_start_col, line_end_col;
  u_char *sel_text_latin;
  size_t sel_text_bytes;
  u_char *p;

  if (use_bitmap_font) {
    li = vga.text_height;
    co = vga.scan_len / 2;
  }

  sel_text_latin = sel_text = malloc((row2-row1+1)*(co+1)+2);
  
  /* Copy the text data. */
  for (row = row1; (row <= row2); row++)
  {
    line_start_col = ((row == row1) ? col1 : 0);
    line_end_col = ((row == row2) ? col2 : co-1);
    p = sel_text_latin;
    for (col = line_start_col; (col <= line_end_col); col++)
    {
      *p++ = XCHAR(screen_adr+row*co+col);
    }
    /* Remove end-of-line spaces and add a newline. */
    if (col == co)
    { 
      p--;
      while ((*p == ' ') && (p > sel_text_latin))
        p--;
      p++;
      *p++ = '\n';
    }
    
    sel_text_bytes = p - sel_text_latin;
    for (i=0; i<sel_text_bytes;i++)
      switch (sel_text_latin[i]) 
      {
      case 21 : /* � */
        sel_text_latin[i] = 0xa7;
        break;
      case 20 : /* � */
        sel_text_latin[i] = 0xb6;
        break;
      case 124 : /* � */
        sel_text_latin[i] = 0xa6;
        break;
      case 0x80 ... 0xff: 
        switch (config.term_charset) {
        case CHARSET_KOI8:
          sel_text_latin[i]=dos_to_koi8[sel_text_latin[i] - 0x80];
          break;
        case CHARSET_LATIN1:
          sel_text_latin[i]=dos_to_latin1[sel_text_latin[i] - 0x80];
          break;
        case CHARSET_LATIN2:
          sel_text_latin[i]=dos_to_latin2[sel_text_latin[i] - 0x80];
          break;
        case CHARSET_LATIN:
        default:
          sel_text_latin[i]=dos_to_latin[sel_text_latin[i] - 0x80];
          break;
        }
      }
    sel_text_latin += sel_text_bytes;
  }
  *sel_text_latin = '\0';
}

#else /* HAVE_UNICODE_TRANSLATION */

static void save_selection(int col1, int row1, int col2, int row2)
{
	int row, col, line_start_col, line_end_col;
	u_char *sel_text_dos, *sel_text_latin, *sel_text_ptr, *prev_sel_text_latin;
	size_t sel_space, sel_text_bytes;
	u_char *p;
        
	struct char_set_state paste_state;
	struct char_set_state video_state; /* must not have any... */

	struct char_set *paste_charset = trconfig.paste_charset;
	struct char_set *video_charset = trconfig.video_mem_charset;
  
	init_charset_state(&video_state, video_charset);
	init_charset_state(&paste_state, paste_charset);
	
	p = sel_text_dos = malloc(co);
	sel_space = (row2-row1+1)*(co+1)+102;
	sel_text_latin = sel_text = malloc(sel_space);
  
	/* Copy the text data. */
	for (row = row1; (row <= row2); row++)
	{
		prev_sel_text_latin = sel_text_latin;
		line_start_col = ((row == row1) ? col1 : 0);
		line_end_col = ((row == row2) ? col2 : co-1);
		p = sel_text_ptr = sel_text_dos;
		for (col = line_start_col; (col <= line_end_col); col++)
		{
			*p++ = XCHAR(screen_adr+row*co+col);
		}
		sel_text_bytes = line_end_col - line_start_col + 1;
		while(sel_text_bytes) {
			t_unicode symbol;
			size_t result;
			/* If we hit any run with what we have */
			result = charset_to_unicode(&video_state, &symbol,
						    sel_text_ptr, sel_text_bytes);
			if (result == -1) break;
			sel_text_bytes -= result;
			sel_text_ptr += result;
			result = unicode_to_charset(&paste_state, symbol,
						    sel_text_latin, sel_space);
			if (result == -1) break;
			sel_text_latin += result;
			sel_space -= result;
		}
		/* Remove end-of-line spaces and add a newline. */
		if (col == co)
		{ 
			sel_text_latin--;
			while ((*sel_text_latin == ' ') && (sel_text_latin > prev_sel_text_latin))
				sel_text_latin--;
			sel_text_latin++;
			*sel_text_latin++ = '\n';
		}
	}
	free(sel_text_dos);
	*sel_text_latin = '\0';
  
	cleanup_charset_state(&video_state);
	cleanup_charset_state(&paste_state);
}

#endif /* HAVE_UNICODE_TRANSLATION */

/*
 * Copy the selected text to sel_text, and inform the X server about it.
 */
static void save_selection_data(void)
{
  int col1, row1, col2, row2;

  if ((sel_end-sel_start) < 0)
  {
    visible_selection = FALSE;
    return;
  }
  row1 = (sel_start-screen_adr)/co;
  row2 = (sel_end-screen_adr)/co;
  col1 = (sel_start-screen_adr)%co;
  col2 = (sel_end-screen_adr)%co;
  
  /* Allocate space for text. */
  if (sel_text != NULL)
    free(sel_text);

  save_selection(col1, row1, col2, row2);
  
  v_printf("VGAEMU: Selection, %d,%d->%d,%d, size=%d\n", 
	   col1, row1, col2, row2, strlen(sel_text));

  if (strlen(sel_text) == 0)
    return;
}


/*
 * End of selection (button released).
 */
char *end_selection()
{
  if (!doing_selection)
    return NULL;
  doing_selection = FALSE;
  save_selection_data();
  return sel_text;
}

#endif /* CONFIG_SELECTION */
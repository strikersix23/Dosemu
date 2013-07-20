/*
 *  (C) Copyright 1992, ..., 2007 the "DOSEMU-Development-Team".
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2, as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

extern void vga_init_et4000(void);

extern u_char et4000_ext_video_port_in(ioport_t port);
extern void et4000_ext_video_port_out(ioport_t port, u_char value);

extern void et4000_set_bank_read(u_char bank);
extern void et4000_set_bank_write(u_char bank);

extern void et4000_save_ext_regs(u_char xregs[], u_short xregs16[]);
extern void et4000_restore_ext_regs(u_char xregs[], u_short xregs16[]);


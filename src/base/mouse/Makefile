#
# (C) Copyright 1992, ..., 2007 the "DOSEMU-Development-Team".
#
# for details see file COPYING.DOSEMU in the DOSEMU distribution
#

top_builddir=../../..
include $(top_builddir)/Makefile.conf

#
# This is the Makefile for the mouse-subdirectory of the DOS-emulator
# for Linux.
# $Id$


# The following variables may go into the Makfile in the parent-directory

DEFINES=-DCLEARDTR_SUPPORT
ALL_CPPFLAGS+=$(DEFINES)

# Insert all source- and header-files here.


CFILES = mouse.c mouseint.c mousevid.c gcursor.c
DEPENDS=$(CFILES:.c=.d)
HFILES = mousevid.h
ALL = $(CFILES) $(HFILES)
OBJS = mouse.o mouseint.o mousevid.o gcursor.o

all:  	lib

install:

include $(REALTOPDIR)/src/Makefile.common

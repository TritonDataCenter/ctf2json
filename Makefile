#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright (c) 2011, Joyent, Inc. All rights reserved.
# Copyright (c) 2011, Robert Mustacchi. All rights reserved.
#

#
# We support both gcc and sunwcc, because why not?
#

GCC=gcc
SUNWCC=/opt/SUNWspro/bin/c99
LD=/usr/lib/ld
CFLAGS=-m64
CPPFLAGS=-D_XOPEN_SOURCE=600 -D_LARGEFILE64_SOURCE
GCFLAGS=-Wall -Wextra -pedantic -std=c99 -Wno-unused-parameter
SCFLAGS=
LIBS=-lctf -lavl
PROGNAME=ctf2json
OBJS=ctf2json.o list.o
CSTYLE=tools/cstyle
LINT=/opt/SUNWspro/sunstudio12.1/bin/lint

all: $(PROGNAME)

$(PROGNAME): $(OBJS)
	$(GCC) $(CFLAGS) $(LIBS) $(OBJS) -o $(PROGNAME) 

ctf2json.o: ctf2json.c
	$(GCC) $(CFLAGS) $(CPPFLAGS) $(GCFLAGS) -o ctf2json.o -c ctf2json.c

list.o: list.c
	$(GCC) $(CFLAGS) $(CPPFLAGS) $(GCFLAGS) -o list.o -c list.c

clean:
	rm -f $(PROGNAME) *.o

sunwcc:
	$(SUNWCC) $(CFLAGS) $(CPPFLAGS) -o ctf2json.o -c ctf2json.c
	$(SUNWCC) $(CFLAGS) $(CPPFLAGS) -o list.o -c list.c
	$(SUNWCC) $(CFLAGS) $(LIBS) $(OBJS) -o $(PROGNAME) 

check:
	$(CSTYLE) -cpP ctf2json.c
	$(CSTYLE) -cpP list.c
	$(LINT) -uaxm $(CFLAGS) $(CPPFLAGS) -errtags=yes -s \
		-erroff=E_ASSIGN_NARROW_CONV -U__PRAGMA_REDEFINE_EXTNAME \
		-Xc99=%all -errsecurity=core -m -erroff=E_NAME_DEF_NOT_USED2 \
		-erroff=E_NAME_DECL_NOT_USED_DEF2 ctf2json.c

.PHONY: all sunwcc check

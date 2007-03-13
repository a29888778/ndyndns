/* chroot.h - include file for chroot.c

   (C) 2003-2007 Nicholas J. Kain <njk@aerifal.cx>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   version 2.1 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef __NJK_CHROOT_H_
#define __NJK_CHROOT_H_ 1
void update_chroot(char *path);
char *get_chroot(void);
int chroot_exists(void);
void wipe_chroot(void);
void imprison(char *path);
void drop_root(uid_t uid, gid_t gid);
#endif


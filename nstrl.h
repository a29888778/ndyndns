/* nstrl.h - header file for strlcpy/strlcat implementation
   Time-stamp: <2003-05-28 02:34:47 njk>
   
   (C) 2003 Nicholas J. Kain <njk@aerifal.cx>

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

#ifndef _NJK_HAVE_STRL_
#define _NJK_HAVE_STRL_ 1
size_t strlcpy (char *dest, char *src, size_t size);
size_t strlcat (char *dest, char *src, size_t size);
#endif


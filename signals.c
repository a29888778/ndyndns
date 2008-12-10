/* signals.c - abstracts signal handling
 *
 * (C) 2004-2008 Nicholas J. Kain <njkain at gmail dot com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include "log.h"

void hook_signal(int signum, void (*fn)(int), int flags) {
  struct sigaction new_action;

  new_action.sa_handler = fn;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = flags;

  if (sigaction(signum, &new_action, NULL)) {
    log_line("FATAL - failed to hook signal %i\n", signum);
    exit(EXIT_FAILURE);
  }
}

void disable_signal(int signum) {
  struct sigaction new_action;

  new_action.sa_handler = SIG_IGN;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;

  if (sigaction(signum, &new_action, NULL)) {
    log_line("FATAL - failed to ignore signal %i\n", signum);
    exit(EXIT_FAILURE);
  }
}

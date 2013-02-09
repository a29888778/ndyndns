/* (c) 2005-2013 Nicholas J. Kain <njkain at gmail dot com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>

#include "config.h"
#include "defines.h"
#include "dns_dyn.h"
#include "dns_helpers.h"
#include "log.h"
#include "util.h"
#include "strl.h"
#include "strlist.h"
#include "malloc.h"

dyndns_conf_t dyndns_conf;

void init_dyndns_conf()
{
    dyndns_conf.username = NULL;
    dyndns_conf.password = NULL;
    dyndns_conf.hostlist = NULL;
    dyndns_conf.mx = NULL;
    dyndns_conf.wildcard = WC_NOCHANGE;
    dyndns_conf.backmx = BMX_NOCHANGE;
    dyndns_conf.offline = OFFLINE_NO;
    dyndns_conf.system = SYSTEM_DYNDNS;
}

static void modify_dyn_hostip_in_list(dyndns_conf_t *conf, char *host, char *ip)
{
    hostdata_t *t;
    size_t len;
    char *buf;

    if (!conf || !host || !conf->hostlist)
        return;

    for (t = conf->hostlist; t && strcmp(t->host, host); t = t->next);

    if (!t)
        return; /* not found */

    free(t->ip);
    if (!ip) {
        t->ip = ip;
        return;
    }
    len = strlen(ip) + 1;
    buf = xmalloc(len);
    strlcpy(buf, ip, len);
    t->ip = buf;
}

static void modify_dyn_hostdate_in_list(dyndns_conf_t *conf, char *host,
                                        time_t time)
{
    hostdata_t *t;

    if (!conf || !host || !conf->hostlist)
        return;

    for (t = conf->hostlist; t && strcmp(t->host, host); t = t->next);

    if (!t)
        return; /* not found */

    t->date = time;
}

typedef struct {
    return_codes code;
    void *next;
} return_code_list_t;

static return_code_list_t *dd_return_list = NULL;
static strlist_t *dd_update_list = NULL;

static void add_to_return_code_list(return_codes name,
                                    return_code_list_t **list)
{
    return_code_list_t *item, *t;

    if (!list)
        return;

    item = xmalloc(sizeof (return_code_list_t));
    item->code = name;
    item->next = NULL;

    if (!*list) {
        *list = item;
        return;
    }
    t = *list;
    while (t) {
        if (t->next == NULL) {
            t->next = item;
            return;
        }
        t = t->next;
    }

    log_line("%s: failed to add item", __func__);
    free(item);
}

static void free_return_code_list(return_code_list_t *head)
{
    return_code_list_t *p = head, *q = NULL;

    while (p != NULL) {
        q = p;
        p = q->next;
        free(q);
    }
}

int get_return_code_list_arity(return_code_list_t *list)
{
    int i;
    return_code_list_t *c;

    for (c = list, i = 0; c != NULL; c = c->next, ++i);
    return i;
}

/* not really well documented, so here:
 * return from the server will be stored in a buffer
 * buffer will look like:
 good 1.12.123.9
 nochg 1.12.123.9
 nochg 1.12.123.9
 nochg 1.12.123.9
*/
static void decompose_buf_to_list(char *buf)
{
    char tok[MAX_BUF], *point = buf;
    size_t i;

    free_return_code_list(dd_return_list);
    dd_return_list = NULL;

    while (*point != '\0') {
        while (*point != '\0' && isspace(*point))
            point++;
        memset(tok, '\0', sizeof tok);

        /* fetch one token */
        i = 0;
        while (i < sizeof tok && *point != '\0' && !isspace(*point))
            tok[i++] = *(point++);

        if (strstr(tok, "badsys")) {
            add_to_return_code_list(RET_BADSYS, &dd_return_list);
            continue;
        }
        if (strstr(tok, "badagent")) {
            add_to_return_code_list(RET_BADAGENT, &dd_return_list);
            continue;
        }
        if (strstr(tok, "badauth")) {
            add_to_return_code_list(RET_BADAUTH, &dd_return_list);
            continue;
        }
        if (strstr(tok, "!donator")) {
            add_to_return_code_list(RET_NOTDONATOR, &dd_return_list);
            continue;
        }
        if (strstr(tok, "good")) {
            add_to_return_code_list(RET_GOOD, &dd_return_list);
            continue;
        }
        if (strstr(tok, "nochg")) {
            add_to_return_code_list(RET_NOCHG, &dd_return_list);
            continue;
        }
        if (strstr(tok, "notfqdn")) {
            add_to_return_code_list(RET_NOTFQDN, &dd_return_list);
            continue;
        }
        if (strstr(tok, "nohost")) {
            add_to_return_code_list(RET_NOHOST, &dd_return_list);
            continue;
        }
        if (strstr(tok, "!yours")) {
            add_to_return_code_list(RET_NOTYOURS, &dd_return_list);
            continue;
        }
        if (strstr(tok, "abuse")) {
            add_to_return_code_list(RET_ABUSE, &dd_return_list);
            continue;
        }
        if (strstr(tok, "numhost")) {
            add_to_return_code_list(RET_NUMHOST, &dd_return_list);
            continue;
        }
        if (strstr(tok, "dnserr")) {
            add_to_return_code_list(RET_DNSERR, &dd_return_list);
            continue;
        }
        if (strstr(tok, "911")) {
            add_to_return_code_list(RET_911, &dd_return_list);
            continue;
        }
    }
}

/* -1 indicates hard error, -2 soft error on hostname, 0 success */
static int postprocess_update(char *host, char *curip, return_codes retcode)
{
    int ret = -2;

    switch (retcode) {
        default:
            log_line("%s: FATAL: %s has invalid state", host, __func__);
            ret = -1;
            break;
        case RET_BADSYS:
            log_line("%s: [badsys] - FATAL: Should never happen!", host);
            break;
        case RET_BADAGENT:
            log_line("%s: [badagent] - FATAL: Client program is banned!", host);
            break;
        case RET_BADAUTH:
            log_line("%s: [badauth] - FATAL: Invalid username or password.", host);
            break;
        case RET_NOTDONATOR:
            log_line("%s: [!donator] - FATAL: Option requested that is only allowed to donating users (such as 'offline').", host);
            break;
        case RET_NOTFQDN:
            log_line("%s: [notfqdn] - FATAL: Hostname isn't a fully-qualified domain name (such as 'hostname.dyndns.org')'.", host);
            break;
        case RET_NOHOST:
            log_line("%s: [nohost] - FATAL: Hostname doesn't exist or wrong service type specified (dyndns, static, custom).", host);
            break;
        case RET_NOTYOURS:
            log_line("%s: [!yours] - FATAL: Hostname exists, but doesn't belong to your account.", host);
            break;
        case RET_ABUSE:
            log_line("%s: [abuse] - FATAL: Hostname is banned for abuse.", host);
            break;
        case RET_NUMHOST:
            log_line("%s: [numhost] - FATAL: Too many or too few hosts found.", host);
            break;
        case RET_DNSERR:
            log_line("%s: [dnserr] - FATAL: DNS error encountered by server.", host);
            break;
        case RET_911:
            log_line("%s: [911] - FATAL: Critical error on dyndns.org's hardware.  Check http://www.dyndns.org/news/status/ for details.", host);
            break;
            /* Don't hardfail, 'success' */
        case RET_GOOD:
            log_line("%s: [good] - Update successful.", host);
            write_dnsip(host, curip);
            write_dnsdate(host, clock_time());
            ret = 0;
            break;
        case RET_NOCHG:
            log_line("%s: [nochg] - Unnecessary update; further updates will be considered abusive.", host);
            write_dnsip(host, curip);
            write_dnsdate(host, clock_time());
            ret = 0;
            break;
    }
    return ret;
}

static void dyndns_update_ip(char *curip)
{
    int runonce = 0;
    char url[MAX_BUF];
    char unpwd[256];
    strlist_t *t;
    return_code_list_t *u;
    int ret;
    conn_data_t data;

    if (!dd_update_list || !curip)
        return;

    /* set up the authentication url */
    if (use_ssl)
        dyndns_curlbuf_cpy(url, "https", sizeof url);
    else
        dyndns_curlbuf_cpy(url, "http", sizeof url);
    dyndns_curlbuf_cat(url, "://members.dyndns.org/nic/update?", sizeof url);

    dyndns_curlbuf_cat(url, "system=", sizeof url);
    switch (dyndns_conf.system) {
    case SYSTEM_STATDNS: dyndns_curlbuf_cat(url, "statdns", sizeof url); break;
    case SYSTEM_CUSTOMDNS: dyndns_curlbuf_cat(url, "custom", sizeof url); break;
    default: dyndns_curlbuf_cat(url, "dyndns", sizeof url); break;
    }

    dyndns_curlbuf_cat(url, "&hostname=", sizeof url);
    for (t = dd_update_list, runonce = 0; t != NULL; t = t->next) {
        if (runonce)
            dyndns_curlbuf_cat(url, ",", sizeof url);
        runonce = 1;
        dyndns_curlbuf_cat(url, t->str, sizeof url);
    }

    dyndns_curlbuf_cat(url, "&myip=", sizeof url);
    dyndns_curlbuf_cat(url, curip, sizeof url);

    dyndns_curlbuf_cat(url, "&wildcard=", sizeof url);
    switch (dyndns_conf.wildcard) {
    case WC_YES: dyndns_curlbuf_cat(url, "ON", sizeof url); break;
    case WC_NO: dyndns_curlbuf_cat(url, "OFF", sizeof url); break;
    default: dyndns_curlbuf_cat(url, "NOCHG", sizeof url); break;
    }

    dyndns_curlbuf_cat(url, "&mx=", sizeof url);
    if (!dyndns_conf.mx)
        dyndns_curlbuf_cat(url, "NOCHG", sizeof url);
    else
        dyndns_curlbuf_cat(url, dyndns_conf.mx, sizeof url);

    dyndns_curlbuf_cat(url, "&backmx=", sizeof url);
    switch (dyndns_conf.backmx) {
    case BMX_YES: dyndns_curlbuf_cat(url, "YES", sizeof url); break;
    case BMX_NO: dyndns_curlbuf_cat(url, "NO", sizeof url); break;
    default: dyndns_curlbuf_cat(url, "NOCHG", sizeof url); break;
    }

    dyndns_curlbuf_cat(url, "&offline=", sizeof url);
    switch (dyndns_conf.offline) {
    case OFFLINE_YES: dyndns_curlbuf_cat(url, "YES", sizeof url); break;
    default: dyndns_curlbuf_cat(url, "NO", sizeof url); break;
    }

    /* set up username:password pair */
    dyndns_curlbuf_cpy(unpwd, dyndns_conf.username, sizeof unpwd);
    dyndns_curlbuf_cat(unpwd, ":", sizeof unpwd);
    dyndns_curlbuf_cat(unpwd, dyndns_conf.password, sizeof unpwd);

    data.buf = xmalloc(MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    memset(data.buf, '\0', MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    data.buflen = MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1;
    data.idx = 0;

    ret = dyndns_curl_send(url, &data, unpwd, true, use_ssl);
    if (ret > 0) {
        if (ret == 2) { /* Permanent error. */
            log_line("[%s] had a non-recoverable HTTP error.  Removing from updates.  Restart the daemon to re-enable updates.", t->str);
            remove_host_from_hostdata_list(&dyndns_conf.hostlist, t->str);
        }
        goto out;
    }

    decompose_buf_to_list(data.buf);
    if (get_strlist_arity(dd_update_list) !=
        get_return_code_list_arity(dd_return_list)) {
        log_line("list arity doesn't match, updates may be suspect");
    }

    for (t = dd_update_list, u = dd_return_list;
         t != NULL && u != NULL; t = t->next, u = u->next) {

        ret = postprocess_update(t->str, curip, u->code);
        switch (ret) {
            case -1:
            default:
                exit(EXIT_FAILURE);
                break;
            case -2:
                log_line("[%s] has a configuration problem.  Refusing to update until %s-dnserr is removed.", t->str, t->str);
                write_dnserr(t->str, ret);
                remove_host_from_hostdata_list(&dyndns_conf.hostlist, t->str);
                break;
            case 0:
                modify_dyn_hostdate_in_list(&dyndns_conf, t->str, clock_time());
                modify_dyn_hostip_in_list(&dyndns_conf, t->str, curip);
                break;
        }
    }
  out:
    free(data.buf);
}

#define DYN_REFRESH_INTERVAL (28*24*3600 + 60)
void dd_work(char *curip)
{
    free_strlist(dd_update_list);
    free_return_code_list(dd_return_list);
    dd_update_list = NULL;
    dd_return_list = NULL;

    for (hostdata_t *t = dyndns_conf.hostlist; t != NULL; t = t->next) {
        if (strcmp(curip, t->ip)) {
            log_line("adding for update [%s]", t->host);
            add_to_strlist(&dd_update_list, t->host);
            continue;
        }
        if (dyndns_conf.system == SYSTEM_DYNDNS &&
            clock_time() - t->date > DYN_REFRESH_INTERVAL) {
            log_line("adding for refresh [%s]", t->host);
            add_to_strlist(&dd_update_list, t->host);
        }
    }
    if (dd_update_list)
        dyndns_update_ip(curip);
}


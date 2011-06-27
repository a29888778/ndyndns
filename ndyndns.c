/* ndyndns.c - dynamic dns update daemon
 *
 * (C) 2005-2011 Nicholas J. Kain <njkain at gmail dot com>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>

#include <signal.h>
#include <errno.h>

#define _GNU_SOURCE
#include <getopt.h>

#include <curl/curl.h>

#include "defines.h"
#include "cfg.h"
#include "log.h"
#include "chroot.h"
#include "pidfile.h"
#include "signals.h"
#include "strl.h"
#include "linux.h"
#include "checkip.h"
#include "util.h"
#include "strlist.h"
#include "malloc.h"

static dyndns_conf_t dyndns_conf;
static namecheap_conf_t namecheap_conf;
static he_conf_t he_conf;

static char ifname[IFNAMSIZ] = "ppp0";
static char pidfile[MAX_PATH_LENGTH] = PID_FILE_DEFAULT;

static int update_interval = DEFAULT_UPDATE_INTERVAL;
static int use_ssl = 1;
static int update_from_remote = 0;
static int cfg_uid = 0, cfg_gid = 0;

typedef enum {
    RET_DO_NOTHING,
    RET_BADSYS,
    RET_BADAGENT,
    RET_BADAUTH,
    RET_NOTDONATOR,
    RET_GOOD,
    RET_NOCHG,
    RET_NOTFQDN,
    RET_NOHOST,
    RET_NOTYOURS,
    RET_ABUSE,
    RET_NUMHOST,
    RET_DNSERR,
    RET_911
} return_codes;

typedef struct {
    return_codes code;
    void *next;
} return_code_list_t;

static strlist_t *dd_update_list = NULL;
static return_code_list_t *dd_return_list = NULL;
static strlist_t *nc_update_list = NULL;
static strlist_t *he_update_list = NULL;

static volatile sig_atomic_t pending_exit;

static void sighandler(int sig) {
    sig = sig; /* silence warning */
    pending_exit = 1;
}

static void fix_signals(void) {
    disable_signal(SIGPIPE);
    disable_signal(SIGUSR1);
    disable_signal(SIGUSR2);
    disable_signal(SIGTSTP);
    disable_signal(SIGTTIN);
    disable_signal(SIGCHLD);
    disable_signal(SIGHUP);

    hook_signal(SIGINT, sighandler, 0);
    hook_signal(SIGTERM, sighandler, 0);
}

static void write_dnsfile(char *fn, char *cnts)
{
    int fd, written = 0, oldwritten, len;

    if (!fn || !cnts)
        suicide("FATAL - write_dnsfile: received NULL\n");

    fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

    if (fd == -1)
        suicide("FATAL - failed to open %s for write\n", fn);

    len = strlen(cnts);

    while (written < len) {
        oldwritten = written;
        written = write(fd, cnts + written, len - written);
        if (written == -1) {
            if (errno == EINTR) {
                written = oldwritten;
                continue;
            }
            suicide("FATAL - write() failed on %s\n", fn);
        }
    }

    fsync(fd);
    if (close(fd) == -1)
        suicide("error closing %s; possible corruption\n", fn);
}

static void write_dnsdate(char *host, time_t date)
{
    int len;
    char *file, buf[MAX_BUF];

    if (!host)
        suicide("FATAL - write_dnsdate: host is NULL\n");

    len = strlen(host) + strlen("-dnsdate") + 1;
    file = xmalloc(len);
    strlcpy(file, host, len);
    strlcat(file, "-dnsdate", len);
    buf[MAX_BUF - 1] = '\0';
    snprintf(buf, sizeof buf - 1, "%u", (unsigned int)date);

    write_dnsfile(file, buf);
    free(file);
}

/* assumes that if ip is non-NULL, it is valid */
static void write_dnsip(char *host, char *ip)
{
    int len;
    char *file, buf[MAX_BUF];

    if (!host)
        suicide("FATAL - write_dnsip: host is NULL\n");
    if (!ip)
        suicide("FATAL - write_dnsip: ip is NULL\n");

    len = strlen(host) + strlen("-dnsip") + 1;
    file = xmalloc(len);
    strlcpy(file, host, len);
    strlcat(file, "-dnsip", len);
    strlcpy(buf, ip, sizeof buf);

    write_dnsfile(file, buf);
    free(file);
}

/* assumes that if ip is non-NULL, it is valid */
static void write_dnserr(char *host, return_codes code)
{
    int len;
    char *file, buf[MAX_BUF], *error;

    if (!host)
        suicide("FATAL - write_dnserr: host is NULL\n");

    len = strlen(host) + strlen("-dnserr") + 1;
    file = xmalloc(len);
    strlcpy(file, host, len);
    strlcat(file, "-dnserr", len);

    switch (code) {
        case RET_NOTFQDN:
            error = "notfqdn";
            break;
        case RET_NOHOST:
            error = "nohost";
            break;
        case RET_NOTYOURS:
            error = "!yours";
            break;
        case RET_ABUSE:
            error = "abuse";
            break;
        default:
            error = "unknown";
            break;
    }
    strlcpy(buf, error, sizeof buf);

    write_dnsfile(file, buf);
    free(file);
}

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

    log_line("add_to_return_code_list: failed to add item\n");
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

/* Returns 0 on success, 1 on temporary error, terminates program on fatal */
static int update_ip_curl_errcheck(int val, char *cerr)
{
    switch (val) {
        case CURLE_OK:
            return 0;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_FAILED_INIT:
        case CURLE_URL_MALFORMAT:
        case CURLE_URL_MALFORMAT_USER:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_HTTP_RANGE_ERROR:
        case CURLE_HTTP_POST_ERROR:
        case CURLE_ABORTED_BY_CALLBACK:
        case CURLE_BAD_FUNCTION_ARGUMENT:
        case CURLE_BAD_CALLING_ORDER:
        case CURLE_BAD_PASSWORD_ENTERED:
        case CURLE_SSL_PEER_CERTIFICATE:
        case CURLE_SSL_ENGINE_NOTFOUND:
        case CURLE_SSL_ENGINE_SETFAILED:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT:
        case CURLE_BAD_CONTENT_ENCODING:
        case CURLE_SSL_ENGINE_INITFAILED:
        case CURLE_LOGIN_DENIED:
            suicide("Update failed.  cURL returned a fatal error: [%s].  Exiting.\n", cerr);
            break;
        case CURLE_OUT_OF_MEMORY:
        case CURLE_READ_ERROR:
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_RECV_ERROR:
            suicide("Update status unknown.  cURL returned a fatal error: [%s].  Exiting.\n", cerr);
            break;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEOUTED:
        case CURLE_HTTP_PORT_FAILED:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
            log_line("Temporary error connecting to host: [%s].  Queuing for retry.\n", cerr);
            return 1;
        default:
            log_line("cURL returned nonfatal error: [%s]\n", cerr);
            return 0;
    }
    return -1;
}

static void update_ip_buf_error(size_t len, size_t size)
{
    if (len > size)
        suicide("FATAL - config file would overflow a fixed buffer\n");
}

static void nc_update_host(char *host, char *curip)
{
    CURL *h;
    CURLcode ret;
    int len, hostname_size = 0, domain_size = 0;
    char url[MAX_BUF];
    char useragent[64];
    char curlerror[CURL_ERROR_SIZE];
    char *hostname = NULL, *domain = NULL, *p;
    conn_data_t data;

    if (!nc_update_list || !host || !curip)
        return;

    p = strrchr(host, '.');
    if (!p)
        return;
    p = strrchr(p+1, '.');
    if (!p) {
        domain_size = strlen(host) + 1;
        hostname_size = 2;
        hostname = alloca(hostname_size);
        hostname[0] = '@';
        hostname[1] = '\0';
        domain = host;
    } else {
        hostname_size = p - host + 1;
        domain_size = hostname + strlen(host) - p;
        hostname = alloca(hostname_size);
        domain = alloca(domain_size);
        strlcpy(hostname, host, hostname_size);
        strlcpy(domain, p+1, domain_size);
    }

    if (!hostname || !domain)
        return;

    /* set up the authentication url */
    if (use_ssl) {
        len = strlcpy(url, "https", sizeof url);
        update_ip_buf_error(len, sizeof url);
    } else {
        len = strlcpy(url, "http", sizeof url);
        update_ip_buf_error(len, sizeof url);
    }
    len = strlcat(url, "://dynamicdns.park-your-domain.com/update?", sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "host=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, hostname, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&domain=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, domain, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&password=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, namecheap_conf.password, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&ip=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, curip, sizeof url);
    update_ip_buf_error(len, sizeof url);

    /* set up useragent */
    len = strlcpy(useragent, "ndyndns/", sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);
    len = strlcat(useragent, NDYNDNS_VERSION, sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);

    data.buf = xmalloc(MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    memset(data.buf, '\0', MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    data.buflen = MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1;
    data.idx = 0;

    log_line("update url: [%s]\n", url);
    h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, useragent);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curlerror);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &data);
    if (use_ssl)
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, (long)0);
    ret = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (update_ip_curl_errcheck(ret, curlerror) == 1)
        goto out;

    log_line("response returned: [%s]\n", data.buf);
    if (strstr(data.buf, "<ErrCount>0")) {
        log_line(
            "%s: [good] - Update successful.\n", host);
        write_dnsip(host, curip);
        write_dnsdate(host, time(0));
        modify_nc_hostdate_in_list(&namecheap_conf, host, time(0));
        modify_nc_hostip_in_list(&namecheap_conf, host, curip);
    } else {
        log_line("%s: [fail] - Failed to update.\n", host);
    }

  out:
    free(data.buf);
}

static void nc_update_ip(char *curip)
{
    strlist_t *t;

    for (t = nc_update_list; t != NULL; t = t->next)
        nc_update_host(t->str, curip);
}

static void he_update_tunid(char *tunid, char *curip)
{
    CURL *h;
    CURLcode ret;
    int len;
    char url[MAX_BUF];
    char useragent[64];
    char curlerror[CURL_ERROR_SIZE];
    conn_data_t data;

    if (!tunid || !curip)
        return;

    /* set up the authentication url */
    if (use_ssl) {
        len = strlcpy(url, "https", sizeof url);
        update_ip_buf_error(len, sizeof url);
    } else {
        len = strlcpy(url, "http", sizeof url);
        update_ip_buf_error(len, sizeof url);
    }

    len = strlcat(url, "://ipv4.tunnelbroker.net/ipv4_end.php?ip=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, curip, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&pass=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, he_conf.passhash, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&apikey=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, he_conf.userid, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&tid=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, tunid, sizeof url);
    update_ip_buf_error(len, sizeof url);

    /* set up useragent */
    len = strlcpy(useragent, "ndyndns/", sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);
    len = strlcat(useragent, NDYNDNS_VERSION, sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);

    data.buf = xmalloc(MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    memset(data.buf, '\0', MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    data.buflen = MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1;
    data.idx = 0;

    log_line("update url: [%s]\n", url);
    h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, useragent);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curlerror);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &data);
    if (use_ssl)
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, (long)0);
    ret = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (update_ip_curl_errcheck(ret, curlerror) == 1)
        goto out;

    log_line("response returned: [%s]\n", data.buf);
    if (strstr(data.buf, "<ErrCount>0")) {
        log_line(
            "%s: [good] - Update successful.\n", tunid);
        write_dnsip(tunid, curip);
        write_dnsdate(tunid, time(0));
    } else {
        log_line("%s: [fail] - Failed to update.\n", tunid);
    }

  out:
    free(data.buf);
}

static void he_update_tuns(char *curip)
{
    strlist_t *t;

    for (t = he_conf.tunlist; t != NULL; t = t->next)
        he_update_tunid(t->str, curip);
}

static void he_update_host(char *host, char *password, char *curip)
{
    CURL *h;
    CURLcode ret;
    int len;
    char url[MAX_BUF];
    char useragent[64];
    char curlerror[CURL_ERROR_SIZE];
    conn_data_t data;

    if (!he_update_list || !host || !password || !curip)
        return;

    // If this is the host name that is associated with our tunnels,
    // then update all of the tunnels now.
    if (!strcmp(host, he_conf.hostassoc))
        he_update_tuns(curip);

    /* set up the authentication url */
    if (use_ssl) {
        len = strlcpy(url, "https://", sizeof url);
        update_ip_buf_error(len, sizeof url);
    } else {
        len = strlcpy(url, "http://", sizeof url);
        update_ip_buf_error(len, sizeof url);
    }

    len = strlcat(url, host, sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, ":", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, password, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "@dyn.dns.he.net/update?hostname=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, host, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&myip=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, curip, sizeof url);
    update_ip_buf_error(len, sizeof url);

    /* set up useragent */
    len = strlcpy(useragent, "ndyndns/", sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);
    len = strlcat(useragent, NDYNDNS_VERSION, sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);

    data.buf = xmalloc(MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    memset(data.buf, '\0', MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    data.buflen = MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1;
    data.idx = 0;

    log_line("update url: [%s]\n", url);
    h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, useragent);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curlerror);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &data);
    if (use_ssl)
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, (long)0);
    ret = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (update_ip_curl_errcheck(ret, curlerror) == 1)
        goto out;

    log_line("response returned: [%s]\n", data.buf);
    if (strstr(data.buf, "<ErrCount>0")) {
        log_line(
            "%s: [good] - Update successful.\n", host);
        write_dnsip(host, curip);
        write_dnsdate(host, time(0));
        modify_he_hostdate_in_list(&he_conf, host, time(0));
        modify_he_hostip_in_list(&he_conf, host, curip);
    } else {
        log_line("%s: [fail] - Failed to update.\n", host);
    }

  out:
    free(data.buf);
}

static void he_update_ip(char *curip)
{
    strlist_t *t;

    for (t = he_update_list; t != NULL; t = t->next) {
        char *host = t->str, *pass, *p;
        p = strchr(host, ':');
        if (!p)
            continue;
        *p = '\0';
        pass = p + 1;
        he_update_host(host, pass, curip);
        *p = ':';
    }
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
    int i;

    free_return_code_list(dd_return_list);
    dd_return_list = NULL;

    while (*point != '\0') {
        while (*point != '\0' && isspace(*point))
            point++;
        memset(tok, '\0', sizeof tok);

        /* fetch one token */
        i = 0;
        while (*point != '\0' && !isspace(*point))
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
    int ret = -1;

    switch (retcode) {
        default:
            log_line(
                "%s: FATAL: postprocess_update() has invalid state\n", host);
            break;
        case RET_BADSYS:
            log_line(
                "%s: [badsys] - FATAL: Should never happen!\n", host);
            break;
        case RET_BADAGENT:
            log_line(
                "%s: [badagent] - FATAL: Client program is banned!\n", host);
            break;
        case RET_BADAUTH:
            log_line(
                "%s: [badauth] - FATAL: Invalid username or password.\n", host);
            break;
        case RET_NOTDONATOR:
            log_line(
                "%s: [!donator] - FATAL: Option requested that is only allowed to donating users (such as 'offline').\n", host);
            break;
        case RET_NOTFQDN:
            log_line(
                "%s: [notfqdn] - FATAL: Hostname isn't a fully-qualified domain name (such as 'hostname.dyndns.org')'.\n", host);
            ret = -2;
            break;
        case RET_NOHOST:
            log_line(
                "%s: [nohost] - FATAL: Hostname doesn't exist or wrong service type specified (dyndns, static, custom).\n", host);
            ret = -2;
            break;
        case RET_NOTYOURS:
            log_line(
                "%s: [!yours] - FATAL: Hostname exists, but doesn't belong to your account.\n", host);
            ret = -2;
            break;
        case RET_ABUSE:
            log_line(
                "%s: [abuse] - FATAL: Hostname is banned for abuse.\n", host);
            ret = -2;
            break;
        case RET_NUMHOST:
            log_line(
                "%s: [numhost] - FATAL: Too many or too few hosts found.\n", host);
            break;
        case RET_DNSERR:
            log_line(
                "%s: [dnserr] - FATAL: DNS error encountered by server.\n", host);
            break;
        case RET_911:
            log_line(
                "%s: [911] - FATAL: Critical error on dyndns.org's hardware.  Check http://www.dyndns.org/news/status/ for details.\n", host);
            break;
            /* Don't hardfail, 'success' */
        case RET_GOOD:
            log_line(
                "%s: [good] - Update successful.\n", host);
            write_dnsip(host, curip);
            write_dnsdate(host, time(0));
            ret = 0;
            break;
        case RET_NOCHG:
            log_line(
                "%s: [nochg] - Unnecessary update; further updates will be considered abusive.\n", host);
            write_dnsip(host, curip);
            write_dnsdate(host, time(0));
            ret = 0;
            break;
    }
    return ret;
}

static void dyndns_update_ip(char *curip)
{
    CURL *h;
    CURLcode ret;
    int len, runonce = 0;
    char url[MAX_BUF];
    char tbuf[32];
    char unpwd[256];
    char useragent[64];
    char curlerror[CURL_ERROR_SIZE];
    strlist_t *t;
    return_code_list_t *u;
    int ret2;
    conn_data_t data;

    if (!dd_update_list || !curip)
        return;

    /* set up the authentication url */
    if (use_ssl) {
        len = strlcpy(url,
                      "https://members.dyndns.org/nic/update?", sizeof url);
        update_ip_buf_error(len, sizeof url);
    } else {
        len = strlcpy(url,
                      "http://members.dyndns.org/nic/update?", sizeof url);
        update_ip_buf_error(len, sizeof url);
    }

    switch (dyndns_conf.system) {
        case SYSTEM_STATDNS:
            strlcpy(tbuf, "statdns", sizeof tbuf);
            break;
        case SYSTEM_CUSTOMDNS:
            strlcpy(tbuf, "custom", sizeof tbuf);
            break;
        default:
            strlcpy(tbuf, "dyndns", sizeof tbuf);
            break;
    }
    len = strlcat(url, "system=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, tbuf, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&hostname=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    for (t = dd_update_list, runonce = 0; t != NULL; t = t->next) {
        if (runonce) {
            len = strlcat(url, ",", sizeof url);
            update_ip_buf_error(len, sizeof url);
        }
        runonce = 1;
        len = strlcat(url, t->str, sizeof url);
        update_ip_buf_error(len, sizeof url);
    }

    len = strlcat(url, "&myip=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, curip, sizeof url);
    update_ip_buf_error(len, sizeof url);

    switch (dyndns_conf.wildcard) {
        case WC_YES:
            strlcpy(tbuf, "ON", sizeof tbuf);
            break;
        case WC_NO:
            strlcpy(tbuf, "OFF", sizeof tbuf);
            break;
        default:
            strlcpy(tbuf, "NOCHG", sizeof tbuf);
            break;
    }
    len = strlcat(url, "&wildcard=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, tbuf, sizeof url);
    update_ip_buf_error(len, sizeof url);

    len = strlcat(url, "&mx=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    if (dyndns_conf.mx == NULL) {
        len = strlcat(url, "NOCHG", sizeof url);
        update_ip_buf_error(len, sizeof url);
    } else {
        len = strlcat(url, dyndns_conf.mx, sizeof url);
        update_ip_buf_error(len, sizeof url);
    }

    switch (dyndns_conf.backmx) {
        case BMX_YES:
            strlcpy(tbuf, "YES", sizeof tbuf);
            break;
        case BMX_NO:
            strlcpy(tbuf, "NO", sizeof tbuf);
            break;
        default:
            strlcpy(tbuf, "NOCHG", sizeof tbuf);
            break;
    }
    len = strlcat(url, "&backmx=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, tbuf, sizeof url);
    update_ip_buf_error(len, sizeof url);

    switch (dyndns_conf.offline) {
        case OFFLINE_YES:
            strlcpy(tbuf, "YES", sizeof tbuf);
            break;
        default:
            strlcpy(tbuf, "NO", sizeof tbuf);
            break;
    }
    len = strlcat(url, "&offline=", sizeof url);
    update_ip_buf_error(len, sizeof url);
    len = strlcat(url, tbuf, sizeof url);
    update_ip_buf_error(len, sizeof url);


    /* set up username:password pair */
    len = strlcpy(unpwd, dyndns_conf.username, sizeof unpwd);
    update_ip_buf_error(len, sizeof unpwd);
    len = strlcat(unpwd, ":", sizeof unpwd);
    update_ip_buf_error(len, sizeof unpwd);
    len = strlcat(unpwd, dyndns_conf.password, sizeof unpwd);
    update_ip_buf_error(len, sizeof unpwd);


    /* set up useragent */
    len = strlcpy(useragent, "ndyndns/", sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);
    len = strlcat(useragent, NDYNDNS_VERSION, sizeof useragent);
    update_ip_buf_error(len, sizeof useragent);

    data.buf = xmalloc(MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    memset(data.buf, '\0', MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1);
    data.buflen = MAX_CHUNKS * CURL_MAX_WRITE_SIZE + 1;
    data.idx = 0;

    log_line("update url: [%s]\n", url);
    h = curl_easy_init();
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERPWD, unpwd);
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(h, CURLOPT_USERAGENT, useragent);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curlerror);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &data);
    if (use_ssl)
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, (long)0);
    ret = curl_easy_perform(h);
    curl_easy_cleanup(h);

    if (update_ip_curl_errcheck(ret, curlerror) == 1)
        goto out;

    decompose_buf_to_list(data.buf);
    if (get_strlist_arity(dd_update_list) !=
        get_return_code_list_arity(dd_return_list)) {
        log_line("list arity doesn't match, updates may be suspect\n");
    }

    for (t = dd_update_list, u = dd_return_list;
         t != NULL && u != NULL; t = t->next, u = u->next) {

        ret2 = postprocess_update(t->str, curip, u->code);
        switch (ret2) {
            case -1:
            default:
                exit(EXIT_FAILURE);
                break;
            case -2:
                log_line("[%s] has a configuration problem.  Refusing to update until %s-dnserr is removed.\n", t->str, t->str);
                write_dnserr(t->str, ret2);
                remove_host_from_host_data_list(&dyndns_conf, t->str);
                break;
            case 0:
                modify_hostdate_in_list(&dyndns_conf, t->str, time(0));
                modify_hostip_in_list(&dyndns_conf, t->str, curip);
                break;
        }
    }
  out:
    free(data.buf);
}

static void do_work(void)
{
    char *curip = NULL;
    struct in_addr inr;
    host_data_t *t;
    hostpairs_t *tp;

    log_line("updating to interface: [%s]\n", ifname);

    while (1) {
        free(curip);

        if (pending_exit)
            exit(EXIT_SUCCESS);

        if (update_from_remote == 0) {
            curip = get_interface_ip(ifname);
        } else {
            curip = query_curip();
        }

        if (!curip)
            goto sleep;

        if (inet_aton(curip, &inr) == 0) {
            log_line(
                "%s has ip: [%s], which is invalid.  Sleeping.\n",
                ifname, curip);
            goto sleep;
        }

        free_strlist(dd_update_list);
        free_return_code_list(dd_return_list);
        dd_update_list = NULL;
        dd_return_list = NULL;

        for (t = dyndns_conf.hostlist; t != NULL; t = t->next) {
            if (strcmp(curip, t->ip)) {
                log_line("adding for update [%s]\n", t->host);
                add_to_strlist(&dd_update_list, t->host);
                continue;
            }
            if (dyndns_conf.system == SYSTEM_DYNDNS &&
                time(0) - t->date > REFRESH_INTERVAL) {
                log_line("adding for refresh [%s]\n", t->host);
                add_to_strlist(&dd_update_list, t->host);
            }
        }
        if (dd_update_list)
            dyndns_update_ip(curip);

        free_strlist(nc_update_list);
        nc_update_list = NULL;

        for (t = namecheap_conf.hostlist; t != NULL; t = t->next) {
            if (strcmp(curip, t->ip)) {
                log_line("adding for update [%s]\n", t->host);
                add_to_strlist(&nc_update_list, t->host);
                continue;
            }
        }
        if (nc_update_list)
            nc_update_ip(curip);

        free_strlist(he_update_list);
        he_update_list = NULL;

        for (tp = he_conf.hostpairs; tp != NULL; tp = tp->next) {
            if (strcmp(curip, tp->ip)) {
                size_t csiz = strlen(tp->host) + strlen(tp->password) + 1;
                char *tbuf = alloca(csiz);
                strlcpy(tbuf, tp->host, csiz);
                strlcat(tbuf, ":", csiz);
                strlcat(tbuf, tp->password, csiz);
                log_line("adding for update [%s]\n", tbuf);
                add_to_strlist(&he_update_list, tbuf);
                continue;
            }
        }
        if (he_update_list)
            he_update_ip(curip);

      sleep:
        sleep(update_interval);
    }
}

static int check_ssl(void)
{
    int t;
    curl_version_info_data *data;

    data = curl_version_info(CURLVERSION_NOW);

    t = data->features & CURL_VERSION_SSL;
    if (t) {
        log_line("curl has SSL support, using https.\n");
    } else {
        log_line("curl lacks SSL support, using http.\n");
    }
    return t;
}

void cfg_set_remote(void)
{
    update_from_remote = 1;
    update_interval = 600;
}

void cfg_set_detach(void)
{
    gflags_detach = 1;
}

void cfg_set_nodetach(void)
{
    gflags_detach = 0;
}

void cfg_set_quiet(void)
{
    gflags_quiet = 1;
}

void cfg_set_pidfile(char *pidfname)
{
    strlcpy(pidfile, pidfname, sizeof pidfile);
}

void cfg_set_user(char *username)
{
    int t;
    char *p;
    struct passwd *pws;

    t = (unsigned int) strtol(username, &p, 10);
    if (*p != '\0') {
        pws = getpwnam(username);
        if (pws) {
            cfg_uid = (int)pws->pw_uid;
            if (!cfg_gid)
                cfg_gid = (int)pws->pw_gid;
        } else suicide("FATAL - Invalid uid specified.\n");
    } else
        cfg_uid = t;
}

void cfg_set_group(char *groupname)
{
    int t;
    char *p;
    struct group *grp;

    t = (unsigned int) strtol(groupname, &p, 10);
    if (*p != '\0') {
        grp = getgrnam(groupname);
        if (grp) {
            cfg_gid = (int)grp->gr_gid;
        } else suicide("FATAL - Invalid gid specified.\n");
    } else
        cfg_gid = t;
}

void cfg_set_interface(char *interface)
{
    strlcpy(ifname, interface, sizeof ifname);
}

int main(int argc, char** argv)
{
    int c, read_cfg = 0;

    init_dyndns_conf(&dyndns_conf);
    init_namecheap_conf(&namecheap_conf);
    init_he_conf(&he_conf);

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"detach", 0, 0, 'd'},
            {"nodetach", 0, 0, 'n'},
            {"pidfile", 1, 0, 'p'},
            {"quiet", 0, 0, 'q'},
            {"chroot", 1, 0, 'c'},
            {"disable-chroot", 0, 0, 'x'},
            {"file", 1, 0, 'f'},
            {"cfg-stdin", 0, 0, 'F'},
            {"user", 1, 0, 'u'},
            {"group", 1, 0, 'g'},
            {"interface", 1, 0, 'i'},
            {"remote", 0, 0, 'r'},
            {"help", 0, 0, 'h'},
            {"version", 0, 0, 'v'},
            {0, 0, 0, 0}
        };

        c = getopt_long(argc, argv, "rdnp:qc:xf:Fu:g:i:hv", long_options, &option_index);
        if (c == -1) break;

        switch (c) {

            case 'h':
                printf("ndyndns %s, dyndns update client.  Licensed under GNU GPL.\n", NDYNDNS_VERSION);
                printf(
                    "Copyright (C) 2005-2011 Nicholas J. Kain\n"
                    "Usage: ndyndns [OPTIONS]\n"
                    "  -d, --detach                detach from TTY and daemonize\n"
                    "  -n, --nodetach              stay attached to TTY\n"
                    "  -q, --quiet                 don't print to std(out|err) or log\n");
                printf(
                    "  -c, --chroot                path where ndyndns should chroot\n"
                    "  -x, --disable-chroot        do not actually chroot (not recommended)\n"
                    "  -f, --file                  configuration file\n"
                    "  -F, --cfg-stdin             read configuration file from standard input\n"
                    "  -p, --pidfile               pidfile path\n");
                printf(
                    "  -u, --user                  user name that ndyndns should run as\n"
                    "  -g, --group                 group name that ndyndns should run as\n"
                    "  -i, --interface             interface ip to check (default: ppp0)\n"
                    "  -r, --remote                get ip from remote dyndns host (overrides -i)\n"
                    "  -h, --help                  print this help and exit\n"
                    "  -v, --version               print version and license info and exit\n");
                exit(EXIT_FAILURE);
                break;

            case 'v':
                printf(
                    "ndyndns %s Copyright (C) 2005-2011 Nicholas J. Kain\n"
                    "This program is free software: you can redistribute it and/or modify\n"
                    "it under the terms of the GNU General Public License as published by\n"
                    "the Free Software Foundation, either version 3 of the License, or\n"
                    "(at your option) any later version.\n\n", NDYNDNS_VERSION);
                printf(
                    "This program is distributed in the hope that it will be useful,\n"
                    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
                    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
                    "GNU General Public License for more details.\n\n"

                    "You should have received a copy of the GNU General Public License\n"
                    "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n");
                exit(EXIT_FAILURE);
                break;

            case 'r':
                cfg_set_remote();
                break;

            case 'd':
                cfg_set_detach();
                break;

            case 'n':
                cfg_set_nodetach();
                break;

            case 'q':
                cfg_set_quiet();
                break;

            case 'x':
                disable_chroot();
                break;

            case 'c':
                update_chroot(optarg);
                break;

            case 'f':
                if (read_cfg) {
                    log_line("FATAL: duplicate configuration file data specified");
                    exit(EXIT_FAILURE);
                } else {
                    read_cfg = 1;
                    if (parse_config(optarg, &dyndns_conf, &namecheap_conf,
                                     &he_conf) != 1)
                        suicide("FATAL: bad configuration data\n");
                }
                break;

            case 'F':
                if (read_cfg) {
                    log_line("ERROR: duplicate configuration file data specified");
                    exit(EXIT_FAILURE);
                } else {
                    read_cfg = 1;
                    if (parse_config(NULL, &dyndns_conf, &namecheap_conf,
                                     &he_conf) != 1)
                        suicide("FATAL: bad configuration data\n");
                }
                break;

            case 'p':
                cfg_set_pidfile(optarg);
                break;

            case 'u':
                cfg_set_user(optarg);
                break;

            case 'g':
                cfg_set_group(optarg);
                break;

            case 'i':
                cfg_set_interface(optarg);
                break;
        }
    }

    if (!read_cfg)
        suicide("FATAL - no configuration file, exiting.\n");

    if (chroot_enabled() && getuid())
        suicide("FATAL - I need root for chroot!\n");

    if (gflags_detach)
        if (daemon(0,0))
            suicide("FATAL - detaching fork failed\n");

    if (file_exists(pidfile, "w") == -1)
        exit(EXIT_FAILURE);
    write_pid(pidfile);

    umask(077);
    fix_signals();

    if (!chroot_exists())
        suicide("FATAL - No chroot path specified.  Refusing to run.\n");

    /* Note that failure cases are handled by called fns. */
    imprison(get_chroot());
    drop_root(cfg_uid, cfg_gid);

    /* Cover our tracks... */
    wipe_chroot();
    memset(pidfile, '\0', sizeof pidfile);

    curl_global_init(CURL_GLOBAL_ALL);
    use_ssl = check_ssl();

    do_work();

    exit(EXIT_SUCCESS);
}


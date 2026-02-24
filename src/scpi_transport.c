/**
 * scpi_transport.c
 *
 * POSIX TCP/IP implementation of the SCPI transport layer.
 *
 * How SCPI over TCP works:
 *   - The instrument listens on port 5025 by default.
 *   - Commands are plain ASCII strings terminated by a newline ('\n').
 *   - Queries (commands ending in '?') also produce a newline-terminated
 *     ASCII response that we read back immediately after sending.
 *   - There is no framing, no length prefix, no binary encoding — it is
 *     just a raw text stream, which makes a plain POSIX socket ideal.
 */

/*
 * _POSIX_C_SOURCE 200112L unlocks POSIX.1-2001 extensions that are not
 * part of the ISO C11 standard:
 *   - struct addrinfo, getaddrinfo(), freeaddrinfo(), gai_strerror()
 *   - SO_RCVTIMEO / SO_SNDTIMEO socket options
 * Without this, the compiler (in strict -std=c11 mode) hides those
 * declarations and the build fails with "incomplete type" errors.
 */
#define _POSIX_C_SOURCE 200112L

#include "scpi_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>       /* errno, strerror() */
#include <unistd.h>      /* close() */
#include <netdb.h>       /* getaddrinfo(), freeaddrinfo(), struct addrinfo */
#include <sys/types.h>
#include <sys/socket.h>  /* socket(), connect(), send(), recv(), setsockopt() */
#include <netinet/in.h>  /* IPPROTO_TCP */
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <arpa/inet.h>   /* inet_* helpers (not directly used, but good practice) */
#include <sys/time.h>    /* struct timeval — used for socket timeouts */

struct scpi_ctx {
    int  fd;          /* POSIX file descriptor for the open TCP socket       */
    int  dry_run;     /* 1 = print commands instead of sending them          */
    char host[256];   /* Hostname/IP stored so we can reconnect if needed     */
    int  port;        /* TCP port, also stored for reconnection               */
    char errbuf[256]; /* Human-readable description of the last error         */
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Save a human-readable error message into the context.
 * strerror(errno) converts the numeric errno value (e.g. ECONNREFUSED)
 * into a string like "Connection refused".
 */
static void ctx_set_error(scpi_ctx_t *ctx, const char *msg)
{
    if (ctx)
        snprintf(ctx->errbuf, sizeof(ctx->errbuf), "%s: %s",
                 msg, strerror(errno));
}

/*
 * Tell the kernel to abort a recv() or send() call automatically if it
 * has not completed within `seconds` seconds.  Without this, a network
 * problem could cause the program to hang forever waiting for the PSU.
 *
 * SO_RCVTIMEO applies to recv() / read().
 * SO_SNDTIMEO applies to send() / write().
 */
static int set_socket_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    return 0;
}

/*
 * Open (or re-open) the TCP socket to the instrument and store the new
 * file descriptor in ctx->fd.
 *
 * This is split out from scpi_connect() so that scpi_query() can call it
 * transparently when the instrument has closed the connection between
 * queries — a common behaviour in low-cost bench instruments that only
 * keep the socket open for one transaction at a time.
 */
static int ctx_open_socket(scpi_ctx_t *ctx)
{
    /* Close the old socket if one is still open. */
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", ctx->port);

    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(ctx->host, port_str, &hints, &res);
    if (rc != 0) {
        /* Use a temporary string for the gai error to avoid truncation
         * when host + gai_strerror together exceed errbuf's 256 bytes. */
        const char *gai_err = gai_strerror(rc);
        snprintf(ctx->errbuf, sizeof(ctx->errbuf), "getaddrinfo: %s", gai_err);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        ctx_set_error(ctx, "reconnect");
        return -1;
    }

    set_socket_timeout(fd, SCPI_TIMEOUT_SEC);
    ctx->fd = fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * Why is scpi_connect() longer than a simple connect() call?
 * ============================================================
 * The standard POSIX way to open a TCP connection is:
 *
 *   1. Resolve the host name / IP string into a binary address.
 *   2. Create a socket.
 *   3. Call connect().
 *
 * Step 1 is done with getaddrinfo().  That function is more complex than
 * a simple gethostbyname() call for a good reason:
 *
 *   - It handles BOTH IPv4 and IPv6 transparently (AF_UNSPEC).
 *   - It resolves hostnames ("mylab-psu.local") AND numeric IPs ("192.168.1.5").
 *   - It can return MULTIPLE candidate addresses for the same host
 *     (e.g. both an IPv4 and an IPv6 address).
 *
 * Because it may return a list, we must loop over each candidate address
 * and try to connect to it; we stop at the first one that works.  This is
 * the standard, portable pattern recommended by RFC 3493.
 *
 * In practice, for a bench PSU with a fixed IP the list will have exactly
 * one entry, but writing it correctly costs nothing extra.
 */
scpi_ctx_t *scpi_connect(const char *host, int port)
{
    if (!host) {
        fprintf(stderr, "scpi_connect: host must not be NULL\n");
        return NULL;
    }

    /* Use the default SCPI port if the caller did not specify one. */
    if (port <= 0)
        port = SCPI_DEFAULT_PORT;

    /*
     * Allocate the context first so ctx_open_socket() can use it.
     * calloc() zeroes all fields: fd will be 0, but we set it to -1
     * below to mean "no socket yet" before calling ctx_open_socket().
     */
    scpi_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd      = -1;
    ctx->dry_run = 0;
    ctx->port    = port;
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);

    /*
     * ctx_open_socket() contains the full getaddrinfo() + connect() logic.
     * It is also called automatically by scpi_query() when the instrument
     * closes the connection between queries (see below).
     */
    if (ctx_open_socket(ctx) < 0) {
        fprintf(stderr, "connect(%s:%d): %s\n", host, port, ctx->errbuf);
        free(ctx);
        return NULL;
    }

    return ctx;
}

/*
 * Dry-run context: no socket is opened.  scpi_send() and scpi_query()
 * will print what they would have sent instead of touching the network.
 * Useful to verify commands without a connected instrument.
 */
scpi_ctx_t *scpi_connect_dry_run(void)
{
    scpi_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->fd      = -1;
    ctx->dry_run = 1;
    return ctx;
}

void scpi_disconnect(scpi_ctx_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->fd >= 0)
        close(ctx->fd); /* sends TCP FIN, closes the connection gracefully */
    free(ctx);          /* release the heap memory for the context struct */
}

int scpi_send(scpi_ctx_t *ctx, const char *cmd)
{
    if (!ctx || !cmd)
        return -1;

    /*
     * Dry-run mode: print what would be sent and return success without
     * touching the network.
     */
    if (ctx->dry_run) {
        printf("[DRY-RUN] SEND:  %s\n", cmd);
        return 0;
    }

    /*
     * The SCPI standard (and the SPD1305X in particular) requires each
     * message to be terminated by a newline ('\n', ASCII 0x0A).
     * We build a new buffer cmd + '\n' rather than modifying the caller's
     * string (which might be a string literal in read-only memory).
     */
    size_t len = strlen(cmd);
    char *buf  = malloc(len + 2); /* +1 for '\n', +1 for safety null */
    if (!buf)
        return -1;

    memcpy(buf, cmd, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0'; /* not strictly needed for send(), but clean */

    /*
     * MSG_NOSIGNAL prevents the kernel from sending SIGPIPE to our process
     * if the remote end has already closed the connection.  Without it, the
     * process would be killed by an unhandled signal instead of returning -1.
     */
    ssize_t sent = send(ctx->fd, buf, len + 1, MSG_NOSIGNAL);
    free(buf);

    if (sent < 0) {
        ctx_set_error(ctx, "send");
        return -1;
    }
    return 0;
}

/*
 * Internal helper: send a query and read the response on the current socket.
 * Returns the number of bytes received (>=0) or -1 on error.
 */
static int do_query(scpi_ctx_t *ctx, const char *query,
                    char *resp_buf, size_t buf_size)
{
    /*
     * Dry-run mode: print the query and return a safe fake value.
     * "0.0000" is harmless for all callers that use strtod() on the result.
     */
    if (ctx->dry_run) {
        printf("[DRY-RUN] QUERY: %s\n", query);
        strncpy(resp_buf, "0.0000", buf_size - 1);
        resp_buf[buf_size - 1] = '\0';
        return (int)strlen(resp_buf);
    }

    if (scpi_send(ctx, query) < 0)
        return -1;

    /*
     * Read the response.  We leave one byte spare (buf_size - 1) so we
     * can always append a null terminator after the received data.
     */
    ssize_t n = recv(ctx->fd, resp_buf, buf_size - 1, 0);

    /*
     * n == 0 means the instrument closed the connection (TCP FIN) after
     * sending its last response — common in bench PSUs that only keep the
     * socket alive for one transaction.  Treat this the same as an error
     * so the caller knows to reconnect.
     */
    if (n <= 0) {
        ctx_set_error(ctx, n == 0 ? "connection closed by instrument" : "recv");
        resp_buf[0] = '\0';
        return -1;
    }

    /* Trim trailing whitespace / newlines for a clean string. */
    while (n > 0 && (resp_buf[n - 1] == '\n' ||
                     resp_buf[n - 1] == '\r' ||
                     resp_buf[n - 1] == ' '))
        n--;

    resp_buf[n] = '\0';
    return (int)n;
}

int scpi_query(scpi_ctx_t *ctx, const char *query,
               char *resp_buf, size_t buf_size)
{
    if (!ctx || !query || !resp_buf || buf_size == 0)
        return -1;

    /* First attempt on the current (possibly still-open) socket. */
    int n = do_query(ctx, query, resp_buf, buf_size);
    if (n >= 0)
        return n;

    /*
     * The query failed — most likely because the instrument closed the TCP
     * connection after its previous response (a common behaviour in low-cost
     * SCPI instruments).
     *
     * Reconnect once and retry.  If this also fails we give up and return -1
     * so the caller can report the error.
     */
    if (ctx_open_socket(ctx) < 0)
        return -1; /* reconnect itself failed */

    return do_query(ctx, query, resp_buf, buf_size);
}

const char *scpi_last_error(const scpi_ctx_t *ctx)
{
    if (!ctx)
        return "NULL context";
    return ctx->errbuf[0] ? ctx->errbuf : "(no error)";
}

/*
 * Internal helper for scpi_select_and_query: builds "setup_cmd\nquery\n"
 * as one buffer, sends it in a single write, then reads the response.
 * Returns byte count on success, -1 on error.
 */
static int do_select_and_query(scpi_ctx_t *ctx,
                               const char *setup_cmd, const char *query,
                               char *resp_buf, size_t buf_size)
{
    if (ctx->dry_run) {
        printf("[DRY-RUN] SEND:  %s\n", setup_cmd);
        printf("[DRY-RUN] QUERY: %s\n", query);
        strncpy(resp_buf, "0x0000", buf_size - 1);
        resp_buf[buf_size - 1] = '\0';
        return (int)strlen(resp_buf);
    }

    /*
     * Build a single buffer: "setup_cmd\nquery\n"
     * Sending both lines in one write() guarantees the instrument receives
     * them before it has a chance to close the connection after setup_cmd.
     */
    size_t s_len = strlen(setup_cmd);
    size_t q_len = strlen(query);
    size_t total = s_len + 1 + q_len + 1; /* each line gets a '\n' */

    char *buf = malloc(total + 1);
    if (!buf)
        return -1;

    memcpy(buf,                 setup_cmd, s_len);
    buf[s_len]                = '\n';
    memcpy(buf + s_len + 1,    query,     q_len);
    buf[s_len + 1 + q_len]    = '\n';
    buf[total]                = '\0';

    ssize_t sent = send(ctx->fd, buf, total, MSG_NOSIGNAL);
    free(buf);

    if (sent < 0) {
        ctx_set_error(ctx, "send (select_and_query)");
        return -1;
    }

    /* Read the single response that the query produces. */
    ssize_t n = recv(ctx->fd, resp_buf, buf_size - 1, 0);
    if (n <= 0) {
        ctx_set_error(ctx, n == 0 ? "connection closed by instrument" : "recv");
        resp_buf[0] = '\0';
        return -1;
    }

    while (n > 0 && (resp_buf[n-1] == '\n' ||
                     resp_buf[n-1] == '\r' ||
                     resp_buf[n-1] == ' '))
        n--;
    resp_buf[n] = '\0';
    return (int)n;
}

int scpi_select_and_query(scpi_ctx_t *ctx,
                          const char *setup_cmd, const char *query,
                          char *resp_buf, size_t buf_size)
{
    if (!ctx || !setup_cmd || !query || !resp_buf || buf_size == 0)
        return -1;

    /* First attempt on the current socket. */
    int n = do_select_and_query(ctx, setup_cmd, query, resp_buf, buf_size);
    if (n >= 0)
        return n;

    /* On failure reconnect once and retry. */
    if (ctx_open_socket(ctx) < 0)
        return -1;

    return do_select_and_query(ctx, setup_cmd, query, resp_buf, buf_size);
}

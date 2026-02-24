/**
 * scpi_transport.h
 *
 * Raw TCP/IP SCPI transport layer for the SPD1305X PSU.
 * Wraps a POSIX socket so upper layers can call scpi_send() /
 * scpi_query() without caring about the network details.
 */

#ifndef SCPI_TRANSPORT_H
#define SCPI_TRANSPORT_H

#include <stddef.h>

#define SCPI_DEFAULT_PORT    5025  /* IANA-assigned port for SCPI-RAW         */
#define SCPI_RECV_BUF_SIZE   4096  /* Max response length we ever expect       */
#define SCPI_TIMEOUT_SEC     5     /* Abort recv/send after this many seconds  */

/**
 * Opaque handle returned by scpi_connect().
 * Callers should treat it as a black-box pointer.
 */
typedef struct scpi_ctx scpi_ctx_t;

/**
 * Open a TCP connection to the instrument.
 *
 * @param host  IP address or hostname (e.g. "192.168.1.100")
 * @param port  TCP port (pass 0 to use SCPI_DEFAULT_PORT)
 * @return      Allocated context on success, NULL on failure.
 *              The caller must free it with scpi_disconnect().
 */
scpi_ctx_t *scpi_connect(const char *host, int port);

/**
 * Create a dry-run context: no network connection is made.
 * All scpi_send() calls will print "[DRY-RUN] SEND:  <cmd>" to stdout.
 * All scpi_query() calls will print "[DRY-RUN] QUERY: <cmd>" and
 * return the string "0.0000" as a fake response so driver code that
 * calls strtod() on the result does not crash.
 *
 * Useful for verifying which SCPI commands would be sent without
 * touching the instrument.
 */
scpi_ctx_t *scpi_connect_dry_run(void);

/**
 * Close the connection and free all resources.
 * Safe to call with ctx == NULL.
 */
void scpi_disconnect(scpi_ctx_t *ctx);

/**
 * Send a SCPI command that produces no response (e.g. "OUTPut CH1,ON").
 *
 * @return  0 on success, -1 on error.
 */
int scpi_send(scpi_ctx_t *ctx, const char *cmd);

/**
 * Send a SCPI query and receive the response (e.g. "*IDN?").
 *
 * @param ctx       Transport context
 * @param query     Query string (the trailing '?' is your responsibility)
 * @param resp_buf  Buffer to store the null-terminated response
 * @param buf_size  Size of resp_buf in bytes
 * @return          Number of bytes in response (>=0), or -1 on error.
 */
int scpi_query(scpi_ctx_t *ctx, const char *query,
               char *resp_buf, size_t buf_size);

/**
 * Send a setup command immediately followed by a query in a single TCP
 * write, then read the one response.
 *
 * This is needed when the instrument closes the connection after every
 * response: if setup_cmd and query were sent in separate calls, the
 * instrument would have closed the socket after setup_cmd (which itself
 * produces no response), so query would land on a new connection where
 * the channel selection has been forgotten.
 *
 * By packing both lines into one write() the instrument sees them
 * together before it closes anything.
 *
 * @param setup_cmd  A command that produces no response (e.g. "INSTrument CH1")
 * @param query      The query to send immediately after  (e.g. "SYSTem:STATus?")
 * @param resp_buf   Buffer for the response
 * @param buf_size   Size of resp_buf
 * @return           Number of bytes in response (>=0), or -1 on error.
 */
int scpi_select_and_query(scpi_ctx_t *ctx,
                          const char *setup_cmd, const char *query,
                          char *resp_buf, size_t buf_size);

/** Return the human-readable error string for the last operation. */
const char *scpi_last_error(const scpi_ctx_t *ctx);

#endif /* SCPI_TRANSPORT_H */

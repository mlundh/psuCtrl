/**
 * main.c  —  SPD PSU command-line controller (SPD1305X / SPD3303X-E)
 *
 * Design
 * ------
 * Each invocation of this program:
 *   1. Parses the command-line arguments.
 *   2. Opens a TCP connection to the SPD1305X.
 *   3. Executes exactly ONE command.
 *   4. Closes the connection and exits.
 *
 * This "one-shot" design keeps the code simple and makes it trivially
 * scriptable — you can call it from a shell script or a Makefile.
 *
 * Usage:
 *   psuCtrl <host> [port] <command> [args...]
 *
 * The port is optional.  If the second argument looks like a number in
 * the range 1-65535 it is treated as the port; otherwise it is treated
 * as the command.  Default port is 5025.
 *
 * Commands:
 *   idn                          Query instrument identity (*IDN?)
 *   status                       Print set-points + measurements for all channels
 *   meas     <ch>                Print measured voltage & current for a channel
 *   setvolt  <ch> <volts>        Set voltage   (CH1/CH2 only; 0–30 V)
 *   setcurr  <ch> <amps>         Set current limit (CH1/CH2: 0–5 A, CH3: 0–1 A)
 *   set      <ch> <volts> <amps> Set both voltage and current in one call
 *   on       <ch>                Enable channel output
 *   off      <ch>                Disable channel output
 *
 * Channel numbering:  1 = CH1,  2 = CH2,  3 = CH3
 *
 * Examples:
 *   psuCtrl 192.168.1.100 setvolt 1 12.5
 *   psuCtrl 192.168.1.100 5025 set 2 5.0 0.5
 *   psuCtrl 192.168.1.100 on 1
 *   psuCtrl 192.168.1.100 status
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "scpi_transport.h"
#include "spd_driver.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <host> [port] <command> [args...] [-n]\n"
        "\n"
        "  -n  Dry-run: print SCPI commands instead of sending them.\n"
        "      May appear anywhere on the command line.\n"
        "      No network connection is made; useful for scripting/debugging.\n"
        "\n"
        "Commands:\n"
        "  idn                          Query instrument identity\n"
        "  status                       All channels: set-points + measurements\n"
        "  meas    <ch>                 Measured voltage & current\n"
        "  setvolt <ch> <volts>         Set voltage\n"
        "  setcurr <ch> <amps>          Set current limit\n"
        "  set     <ch> <volts> <amps>  Set voltage and current\n"
        "  on      <ch>                 Enable output\n"
        "  off     <ch>                 Disable output\n"
        "\n"
        "Channel: 1=CH1  2=CH2\n",
        prog);
}

/*
 * strtol() converts a string to a long integer.
 * The `end` pointer is set to the first character that was NOT consumed.
 * If *end != '\0' after the call, the string had trailing garbage (e.g. "80x")
 * and we reject it.
 */
static int parse_port(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10); /* base 10 */
    if (*end != '\0' || v < 1 || v > 65535)
        return -1; /* not a valid port — caller will treat it as a command word */
    return (int)v;
}

static spd_channel_t parse_channel(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < 1 || v > SPD_CH_MAX) {
        fprintf(stderr, "Invalid channel '%s' (must be 1 or 2)\n", s);
        return (spd_channel_t)-1; /* sentinel: cast -1 to the enum type */
    }
    return (spd_channel_t)v;
}

/* strtod() is the floating-point equivalent of strtol(). */
static int parse_double(const char *s, double *out)
{
    char *end;
    *out = strtod(s, &end);
    if (*end != '\0') {
        fprintf(stderr, "Invalid numeric value '%s'\n", s);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                    */
/* ------------------------------------------------------------------ */

static int cmd_idn(scpi_ctx_t *ctx, spd_model_t model,
                   int argc, char **argv)
{
    (void)model; (void)argc; (void)argv;
    return spd_identify(ctx, NULL, 0);
}

static int cmd_status(scpi_ctx_t *ctx, spd_model_t model,
                      int argc, char **argv)
{
    (void)argc; (void)argv;
    spd_print_status(ctx, model);
    return 0;
}

static int cmd_meas(scpi_ctx_t *ctx, spd_model_t model,
                    int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "meas: missing channel argument\n");
        return -1;
    }

    (void)model;
    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    double v = spd_measure_voltage(ctx, ch);
    double a = spd_measure_current(ctx, ch);

    if (v < 0.0 || a < 0.0)
        return -1;

    printf("%s  %.4f V  %.4f A\n", spd_channel_name(ch), v, a);
    return 0;
}

static int cmd_setvolt(scpi_ctx_t *ctx, spd_model_t model,
                       int argc, char **argv)
{
    (void)model;
    if (argc < 2) {
        fprintf(stderr, "setvolt: usage: setvolt <ch> <volts>\n");
        return -1;
    }

    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    double volts;
    if (parse_double(argv[1], &volts) < 0)
        return -1;

    return spd_set_voltage(ctx, ch, volts);
}

static int cmd_setcurr(scpi_ctx_t *ctx, spd_model_t model,
                       int argc, char **argv)
{
    (void)model;
    if (argc < 2) {
        fprintf(stderr, "setcurr: usage: setcurr <ch> <amps>\n");
        return -1;
    }

    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    double amps;
    if (parse_double(argv[1], &amps) < 0)
        return -1;

    return spd_set_current(ctx, ch, amps);
}

static int cmd_set(scpi_ctx_t *ctx, spd_model_t model,
                   int argc, char **argv)
{
    (void)model;
    if (argc < 3) {
        fprintf(stderr, "set: usage: set <ch> <volts> <amps>\n");
        return -1;
    }

    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    double volts, amps;
    if (parse_double(argv[1], &volts) < 0 ||
        parse_double(argv[2], &amps)  < 0)
        return -1;

    return spd_set_channel(ctx, ch, volts, amps);
}

static int cmd_on(scpi_ctx_t *ctx, spd_model_t model,
                  int argc, char **argv)
{
    (void)model;
    if (argc < 1) {
        fprintf(stderr, "on: missing channel argument\n");
        return -1;
    }

    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    return spd_output_on(ctx, ch);
}

static int cmd_off(scpi_ctx_t *ctx, spd_model_t model,
                   int argc, char **argv)
{
    (void)model;
    if (argc < 1) {
        fprintf(stderr, "off: missing channel argument\n");
        return -1;
    }

    spd_channel_t ch = parse_channel(argv[0]);
    if ((int)ch < 0)
        return -1;

    return spd_output_off(ctx, ch);
}

/* ------------------------------------------------------------------ */
/* Command table                                                       */
/* ------------------------------------------------------------------ */

/*
 * Command dispatch table.
 *
 * Each entry maps a command name string (what the user types) to a
 * function pointer.  This avoids a long if-else chain and makes it
 * easy to add new commands: just add one line here and write the
 * corresponding cmd_xxx() function above.
 *
 * The function signature is:
 *   int cmd_xxx(scpi_ctx_t *ctx, int argc, char **argv)
 * where argc/argv are the arguments AFTER the command name itself.
 */
typedef struct {
    const char *name;
    int (*fn)(scpi_ctx_t *, spd_model_t, int argc, char **argv);
} command_t;

static const command_t commands[] = {
    { "idn",     cmd_idn     },
    { "status",  cmd_status  },
    { "meas",    cmd_meas    },
    { "setvolt", cmd_setvolt },
    { "setcurr", cmd_setcurr },
    { "set",     cmd_set     },
    { "on",      cmd_on      },
    { "off",     cmd_off     },
    { NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /*
     * Optional -n flag (dry-run): accepted anywhere on the command line.
     * We do a single pass over argv, copying every argument that is NOT
     * "-n" into a local array, then parse that compacted array as normal.
     * This lets the user place -n at the start, end, or anywhere in between.
     */
    int dry_run = 0;
    /* +1 so the array is never zero-length even when argc == 0 */
    char **args = argv;          /* default: work directly from argv */
    char **compacted = NULL;
    int   nargs = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            dry_run = 1;
            /* Build a compacted copy without the -n entry */
            compacted = malloc((size_t)argc * sizeof(char *));
            if (!compacted) {
                fprintf(stderr, "Out of memory\n");
                return EXIT_FAILURE;
            }
            int k = 0;
            for (int j = 0; j < argc; j++) {
                if (j != i)
                    compacted[k++] = argv[j];
            }
            nargs = k;
            args  = compacted;
            break;
        }
    }

    if (nargs < 3) {
        free(compacted);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int arg = 1; /* index into args[] — past the program name */
    const char *host = args[arg++];
    int port = 0;
    int cmd_idx = arg; /* argv index of the command word (may shift by 1 for port) */

    /*
     * Optional port detection:
     * If the next argument parses as a valid port number, treat it as the
     * port and advance cmd_idx past it.  Otherwise it IS the command word.
     */
    if (cmd_idx < nargs) {
        int p = parse_port(args[cmd_idx]);
        if (p > 0) {
            port = p;
            cmd_idx++;
        }
    }

    if (cmd_idx >= nargs) {
        free(compacted);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd_name = args[cmd_idx];
    /* Arguments for the command itself */
    int  cmd_argc = nargs - cmd_idx - 1;
    char **cmd_argv = args + cmd_idx + 1;

    /* Look up command */
    const command_t *cmd = NULL;
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            cmd = &commands[i];
            break;
        }
    }

    if (!cmd) {
        fprintf(stderr, "Unknown command '%s'\n\n", cmd_name);
        free(compacted);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Connect (or create a dry-run context with no real connection) */
    scpi_ctx_t *ctx;
    if (dry_run) {
        ctx = scpi_connect_dry_run();
    } else {
        ctx = scpi_connect(host, port);
    }
    if (!ctx) {
        fprintf(stderr, "Failed to connect to %s:%d\n",
                host, port ? port : SCPI_DEFAULT_PORT);
        free(compacted);
        return EXIT_FAILURE;
    }

    /*
     * Detect the instrument model from *IDN? so that status decoding
     * uses the correct SYSTem:STATus? bit layout for this PSU.
     * In dry-run mode there is no real connection, so skip detection.
     */
    spd_model_t model = dry_run ? SPD_MODEL_UNKNOWN
                                : spd_detect_model(ctx);

    /* Execute */
    int rc = cmd->fn(ctx, model, cmd_argc, cmd_argv);

    /* Disconnect */
    scpi_disconnect(ctx);
    free(compacted);

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

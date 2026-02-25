/**
 * spd_driver.c
 *
 * Unified driver for Siglent SPD1305X and SPD3303X-E PSUs.
 *
 * SCPI command reference
 * ----------------------
 *   *IDN?                         Identification (make/model/FW)
 *
 *   INSTrument <CH1|CH2>          Select the active channel for subsequent
 *                                 channel-sensitive bare queries such as
 *                                 VOLTage?, CURRent?, SYSTem:STATus?.
 *
 *   CH1:VOLTage <v>               Set programmed voltage (V) on CH1.
 *   CH1:VOLTage?                  Query programmed voltage on CH1.
 *   CH1:CURRent <a>               Set current limit (A) on CH1.
 *   CH1:CURRent?                  Query current limit on CH1.
 *   (same pattern for CH2:)
 *
 *   OUTPut <CH1|CH2>,ON|OFF       Enable or disable a channel output.
 *                                 NOTE: no space after the comma.
 *
 *   MEASure:VOLTage? <CH1|CH2>    Read the ACTUAL terminal voltage.
 *   MEASure:CURRent? <CH1|CH2>    Read the ACTUAL drawn current.
 *
 *   SYSTem:STATus?                Query instrument state (hex string).
 *
 * SYSTem:STATus? decoding
 * -----------------------
 *   SPD1305X:
 *     Channel must be selected with INSTrument first.
 *     Bit 4 (0x10) = selected channel output ON.
 *
 *   SPD3303X-E:
 *     Single query covers all channels; no INSTrument command needed.
 *     Bit 4 (0x10) = CH1 output ON.
 *     Bit 5 (0x20) = CH2 output ON.
 *
 * Connection behaviour
 * --------------------
 *   Both instruments close the TCP socket after sending a response.
 *   The transport layer auto-reconnects on the next query.
 *   When we need to issue a channel-select command and a query together,
 *   scpi_select_and_query() sends both lines in a single TCP write so
 *   the channel selection is in effect when the query is evaluated.
 *
 * Range validation
 * ----------------
 *   No client-side range checking.  Values are sent to the instrument
 *   verbatim; out-of-range rejection comes from the PSU.
 */

#include "spd_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

const char *spd_channel_name(spd_channel_t ch)
{
    switch (ch) {
        case SPD_CH1: return "CH1";
        case SPD_CH2: return "CH2";
        default:      return "???";
    }
}

/* ------------------------------------------------------------------ */
/* Model detection                                                     */
/* ------------------------------------------------------------------ */

spd_model_t spd_detect_model(scpi_ctx_t *ctx)
{
    char buf[SCPI_RECV_BUF_SIZE];

    if (scpi_query(ctx, "*IDN?", buf, sizeof(buf)) < 0)
        return SPD_MODEL_UNKNOWN;

    /*
     * The *IDN? response is comma-separated:
     *   Siglent Technologies, SPD3303X, ...
     *   Siglent, SPD1305X, ...
     *   Siglent, SPD1168X, ...    (another SPD1000X variant)
     *
     * Match on the model-family prefix; case-sensitive to avoid
     * false positives.
     */
    if (strstr(buf, "SPD3303"))
        return SPD_MODEL_3303XE;

    if (strstr(buf, "SPD1305") || strstr(buf, "SPD1168"))
        return SPD_MODEL_1305X;

    return SPD_MODEL_UNKNOWN;
}

const char *spd_model_name(spd_model_t model)
{
    switch (model) {
        case SPD_MODEL_1305X:   return "SPD1305X";
        case SPD_MODEL_3303XE:  return "SPD3303X-E";
        default:                return "Unknown";
    }
}

int spd_channel_count(spd_model_t model)
{
    /*
     * SPD1305X has only one controllable channel (CH1).
     * All other models (SPD3303X-E, and unknown as a safe default) have two.
     */
    return (model == SPD_MODEL_1305X) ? 1 : SPD_CH_MAX;
}

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

int spd_identify(scpi_ctx_t *ctx, char *resp_buf, size_t buf_size)
{
    char local[SCPI_RECV_BUF_SIZE];
    char *buf = (resp_buf && buf_size > 0) ? resp_buf : local;
    size_t sz  = (resp_buf && buf_size > 0) ? buf_size : sizeof(local);

    int n = scpi_query(ctx, "*IDN?", buf, sz);
    if (n < 0) {
        fprintf(stderr, "spd_identify failed: %s\n", scpi_last_error(ctx));
        return -1;
    }
    printf("IDN: %s\n", buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Voltage & current programming                                       */
/* ------------------------------------------------------------------ */

int spd_set_voltage(scpi_ctx_t *ctx, spd_channel_t ch, double volts)
{
    /*
     * The manual specifies the channel as a command prefix:
     *   CH1:VOLTage <value>
     * No client-side range check: send the value and let the instrument
     * reject it if it is out of range.
     */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s:VOLTage %.4f", spd_channel_name(ch), volts);
    if (scpi_send(ctx, cmd) < 0) {
        fprintf(stderr, "spd_set_voltage(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1;
    }
    return 0;
}

int spd_set_current(scpi_ctx_t *ctx, spd_channel_t ch, double amps)
{
    /* Same channel-prefix pattern: CH1:CURRent <value> */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s:CURRent %.4f", spd_channel_name(ch), amps);
    if (scpi_send(ctx, cmd) < 0) {
        fprintf(stderr, "spd_set_current(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1;
    }
    return 0;
}

int spd_set_channel(scpi_ctx_t *ctx, spd_channel_t ch,
                    double volts, double amps)
{
    if (spd_set_voltage(ctx, ch, volts) < 0)
        return -1;
    return spd_set_current(ctx, ch, amps);
}

/* ------------------------------------------------------------------ */
/* Output enable / disable                                             */
/* ------------------------------------------------------------------ */

int spd_output_on(scpi_ctx_t *ctx, spd_channel_t ch)
{
    char cmd[64];
    /* Manual format: OUTPut <CH>,ON  (no space after the comma) */
    snprintf(cmd, sizeof(cmd), "OUTPut %s,ON", spd_channel_name(ch));
    if (scpi_send(ctx, cmd) < 0) {
        fprintf(stderr, "spd_output_on(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1;
    }
    return 0;
}

int spd_output_off(scpi_ctx_t *ctx, spd_channel_t ch)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "OUTPut %s,OFF", spd_channel_name(ch));
    if (scpi_send(ctx, cmd) < 0) {
        fprintf(stderr, "spd_output_off(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Measurements                                                        */
/* ------------------------------------------------------------------ */

double spd_measure_voltage(scpi_ctx_t *ctx, spd_channel_t ch)
{
    char query[64], resp[64];
    snprintf(query, sizeof(query), "MEASure:VOLTage? %s", spd_channel_name(ch));

    if (scpi_query(ctx, query, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "spd_measure_voltage(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1.0;
    }
    return strtod(resp, NULL);
}

double spd_measure_current(scpi_ctx_t *ctx, spd_channel_t ch)
{
    char query[64], resp[64];
    snprintf(query, sizeof(query), "MEASure:CURRent? %s", spd_channel_name(ch));

    if (scpi_query(ctx, query, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "spd_measure_current(%s) failed: %s\n",
                spd_channel_name(ch), scpi_last_error(ctx));
        return -1.0;
    }
    return strtod(resp, NULL);
}

/* ------------------------------------------------------------------ */
/* Full channel state snapshot                                         */
/* ------------------------------------------------------------------ */

int spd_get_channel_state(scpi_ctx_t *ctx, spd_model_t model,
                          spd_channel_t ch, spd_channel_state_t *out)
{
    if (!out)
        return -1;

    out->ch = ch;

    char resp[64];
    char inst_cmd[64];

    /*
     * Build the channel-select command once; it is reused for every
     * scpi_select_and_query() call below.
     */
    snprintf(inst_cmd, sizeof(inst_cmd), "INSTrument %s", spd_channel_name(ch));

    /* ------------------------------------------------------------------
     * Programmed set-points.
     *
     * Both models: pair INSTrument + VOLTage? / CURRent? in a single TCP
     * write so the channel selection is guaranteed to be in effect when
     * the query is evaluated (the instruments close the socket after each
     * response, which would otherwise drop the selection on reconnect).
     * ------------------------------------------------------------------ */
    if (scpi_select_and_query(ctx, inst_cmd, "VOLTage?", resp, sizeof(resp)) < 0)
        return -1;
    out->set_voltage = strtod(resp, NULL);

    if (scpi_select_and_query(ctx, inst_cmd, "CURRent?", resp, sizeof(resp)) < 0)
        return -1;
    out->set_current = strtod(resp, NULL);

    /* ------------------------------------------------------------------
     * Output enable state via SYSTem:STATus?
     *
     * The two models decode the status word differently:
     *
     *   SPD3303X-E — a single query returns status for ALL channels.
     *     Bit 4 (0x10) = CH1 output ON.
     *     Bit 5 (0x20) = CH2 output ON.
     *     No INSTrument command needed.
     *
     *   SPD1305X (and unknown fallback) — INSTrument selects the channel;
     *     the status word then reflects only that channel.
     *     Bit 4 (0x10) = selected channel output ON.
     *     Must use scpi_select_and_query() to keep them in one TCP write.
     * ------------------------------------------------------------------ */
    if (model == SPD_MODEL_3303XE) {
        if (scpi_query(ctx, "SYSTem:STATus?", resp, sizeof(resp)) < 0)
            return -1;
        long status = strtol(resp, NULL, 0); /* base 0: auto-detect 0x prefix */
        out->output_on = (ch == SPD_CH1) ? ((status & 0x10) ? 1 : 0)
                                         : ((status & 0x20) ? 1 : 0);
    } else {
        /* SPD1305X or unknown — select + query in one TCP write */
        if (scpi_select_and_query(ctx, inst_cmd, "SYSTem:STATus?",
                                  resp, sizeof(resp)) < 0)
            return -1;
        long status = strtol(resp, NULL, 0);
        out->output_on = (status & 0x10) ? 1 : 0;
    }

    /* ------------------------------------------------------------------
     * Live measurements (actual terminal voltage / current).
     * ------------------------------------------------------------------ */
    out->meas_voltage = spd_measure_voltage(ctx, ch);
    out->meas_current = spd_measure_current(ctx, ch);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Status table                                                        */
/* ------------------------------------------------------------------ */

void spd_print_status(scpi_ctx_t *ctx, spd_model_t model)
{
    printf("\nModel: %s\n", spd_model_name(model));
    printf("┌─────────┬──────────────────────┬──────────────────────┬────────┐\n");
    printf("│ Channel │ Set V / Set A        │ Meas V / Meas A      │ Output │\n");
    printf("├─────────┼──────────────────────┼──────────────────────┼────────┤\n");

    int ch_max = spd_channel_count(model);
    for (int i = SPD_CH1; i <= ch_max; i++) {
        spd_channel_state_t st;
        if (spd_get_channel_state(ctx, model, (spd_channel_t)i, &st) < 0) {
            printf("│ %-7s │ %-20s │ %-20s │ %-6s │\n",
                   spd_channel_name((spd_channel_t)i),
                   "ERROR", "ERROR", "?");
        } else {
            char set_str[32], meas_str[32];
            /* %7.4f = 7 chars, " V / " = 5, %6.4f = 6, " A" = 2 → 20 chars total */
            snprintf(set_str,  sizeof(set_str),  "%7.4f V / %6.4f A",
                     st.set_voltage,  st.set_current);
            snprintf(meas_str, sizeof(meas_str), "%7.4f V / %6.4f A",
                     st.meas_voltage, st.meas_current);
            printf("│ %-7s │ %-20s │ %-20s │ %-3s    │\n",
                   spd_channel_name((spd_channel_t)i),
                   set_str, meas_str,
                   st.output_on ? "ON" : "OFF");
        }
    }

    printf("└─────────┴──────────────────────┴──────────────────────┴────────┘\n");
    printf("\n");
}

/**
 * spd_driver.h
 *
 * Unified driver for Siglent SPD series PSUs.
 *
 * Supported models
 * ----------------
 *   SPD1305X   — CH1/CH2, 0–30 V each.  CH1 and CH2 independent.
 *   SPD3303X-E — CH1/CH2, 0–30 V each.  CH1 and CH2 independent.
 *
 * Model is detected automatically by querying *IDN? after connecting.
 *
 * SCPI commands (both models share the same command set)
 * ------------------------------------------------------
 *   *IDN?                    Identification query
 *   CH1:VOLTage <v>          Set programmed voltage (V)
 *   CH1:CURRent <a>          Set current limit (A)
 *   OUTPut CH1,ON|OFF        Enable / disable output
 *   MEASure:VOLTage? CH1     Actual terminal voltage
 *   MEASure:CURRent? CH1     Actual drawn current
 *   INSTrument CH1           Select channel for subsequent bare queries
 *   VOLTage?                 Query programmed voltage of selected channel
 *   CURRent?                 Query programmed current limit of selected channel
 *   SYSTem:STATus?           Hex status word
 *
 * SYSTem:STATus? bit layout
 * -------------------------
 *   Both models:
 *     Bit 0   CH1 CV(0)/CC(1) mode
 *     Bit 1   CH2 CV(0)/CC(1) mode
 *
 *   SPD1305X — channel must be selected first with INSTrument:
 *     Bit 4 (0x10)  selected channel output ON
 *
 *   SPD3303X-E — single query covers all channels:
 *     Bit 4 (0x10)  CH1 output ON
 *     Bit 5 (0x20)  CH2 output ON
 *
 * No client-side range validation is performed.  Values are passed
 * straight to the instrument; any out-of-range rejection comes from
 * the PSU itself.
 *
 * All functions return 0 on success, -1 on error unless the return
 * type is annotated differently.
 */

#ifndef SPD_DRIVER_H
#define SPD_DRIVER_H

#include "scpi_transport.h"

/* ------------------------------------------------------------------ */
/* Model identification                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    SPD_MODEL_UNKNOWN = 0,  /**< Could not identify model from *IDN?  */
    SPD_MODEL_1305X,        /**< Siglent SPD1305X  (SPD1000X series)  */
    SPD_MODEL_3303XE,       /**< Siglent SPD3303X-E (SPD3000X series) */
} spd_model_t;

/**
 * Query *IDN? and return the detected model.
 * Returns SPD_MODEL_UNKNOWN if the response is unrecognised or the
 * query fails.
 */
spd_model_t spd_detect_model(scpi_ctx_t *ctx);

/** Return a human-readable model name string ("SPD1305X", etc.). */
const char *spd_model_name(spd_model_t model);

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/** Channel identifiers — the instrument uses 1-based numbering.     */
typedef enum {
    SPD_CH1    = 1,
    SPD_CH2    = 2,
    SPD_CH_MAX = 2,
} spd_channel_t;

/** Snapshot of one channel's complete state.                         */
typedef struct {
    spd_channel_t ch;
    double        set_voltage;   /**< Programmed voltage (V)          */
    double        set_current;   /**< Programmed current limit (A)    */
    double        meas_voltage;  /**< Measured actual voltage (V)     */
    double        meas_current;  /**< Measured actual current (A)     */
    int           output_on;     /**< 1 = output enabled, 0 = off     */
} spd_channel_state_t;

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

/**
 * Query *IDN? and print the result.
 * @param resp_buf  Buffer to store the response (may be NULL).
 * @param buf_size  Size of resp_buf.
 */
int spd_identify(scpi_ctx_t *ctx, char *resp_buf, size_t buf_size);

/* ------------------------------------------------------------------ */
/* Voltage & current programming                                       */
/* ------------------------------------------------------------------ */

int spd_set_voltage(scpi_ctx_t *ctx, spd_channel_t ch, double volts);
int spd_set_current(scpi_ctx_t *ctx, spd_channel_t ch, double amps);

/** Convenience: set both voltage and current in one call.            */
int spd_set_channel(scpi_ctx_t *ctx, spd_channel_t ch,
                    double volts, double amps);

/* ------------------------------------------------------------------ */
/* Output enable / disable                                             */
/* ------------------------------------------------------------------ */

int spd_output_on (scpi_ctx_t *ctx, spd_channel_t ch);
int spd_output_off(scpi_ctx_t *ctx, spd_channel_t ch);

/* ------------------------------------------------------------------ */
/* Measurements                                                        */
/* ------------------------------------------------------------------ */

/**
 * Read the actual (measured) voltage on a channel.
 * @return measured value in volts, or -1.0 on error.
 */
double spd_measure_voltage(scpi_ctx_t *ctx, spd_channel_t ch);

/**
 * Read the actual (measured) current on a channel.
 * @return measured value in amps, or -1.0 on error.
 */
double spd_measure_current(scpi_ctx_t *ctx, spd_channel_t ch);

/* ------------------------------------------------------------------ */
/* Full channel state snapshot                                         */
/* ------------------------------------------------------------------ */

/**
 * Populate a spd_channel_state_t with all settings and measurements
 * for the given channel.  The model parameter controls how
 * SYSTem:STATus? is decoded.
 */
int spd_get_channel_state(scpi_ctx_t *ctx, spd_model_t model,
                          spd_channel_t ch, spd_channel_state_t *out);

/**
 * Print a formatted status table for all channels to stdout.
 */
void spd_print_status(scpi_ctx_t *ctx, spd_model_t model);

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

/** Return "CH1" or "CH2" for a channel enum value. */
const char *spd_channel_name(spd_channel_t ch);

#endif /* SPD_DRIVER_H */

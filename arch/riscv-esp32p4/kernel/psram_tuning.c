/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: PSRAM read-sampling calibration, so the bus can run at 200 MHz.
*/

/*
 * Why this exists at all.
 *
 * At 20 MHz a double-transfer-rate read has so much margin that the strobe
 * can be sampled anywhere in the window and still land on valid data.  At
 * 200 MHz it cannot: the valid window is a fraction of a nanosecond wide and
 * where it sits depends on the board, the chip, the temperature and the
 * supply.  There is no correct constant to write, only a correct measurement
 * to make, and the measurement has to be made on every boot.
 *
 * The method is the one ESP-IDF uses, because it is the one the hardware's
 * two knobs allow.  A known block is written at 20 MHz, where the write is
 * trustworthy.  The clock is raised.  The block is read back with each
 * candidate setting and compared.  Candidates that reproduce it exactly form
 * one contiguous window, and the middle of that window is the setting with
 * the most margin on both sides.
 *
 * Two stages, because the knobs are not equivalent.  The DQS phase is a
 * coarse choice of four fixed shifts and one read separates them.  The delay
 * lines are fine, thirty-one steps of relative delay, and a candidate that
 * passes once and fails on the hundredth read is exactly the one that must
 * not be chosen; so each is read a hundred times and only a candidate that
 * never failed counts.
 *
 * Everything here runs from SRAM and does not print.  It reconfigures the
 * path the instruction cache is fetched through, so code that lived in flash
 * would stop existing halfway through.  The caller prints afterwards from the
 * result structure.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "psram.h"

/*
 * The reference block.
 *
 * What matters is transition density, not the particular bytes: a pattern
 * that holds a line steady tells you nothing about when that line is safe to
 * sample.  So the block is eight structured words that catch gross failures -
 * all ones, all zeros, both alternating patterns on the sixteen-bit bus - and
 * twenty-four words from a maximal-length 32-bit LFSR, which gives every line
 * a dense and uncorrelated transition sequence.
 *
 * Generated rather than tabulated, which keeps it short, makes the intent
 * legible and avoids carrying a table out of another project.
 */
P4_SRAMCODE void krnPSRAMTuneReference(uint32_t *words, uint32_t count)
{
    P4_SRAMRODATA static const uint32_t structured[8] = {
        0x00000000UL, 0xFFFFFFFFUL, 0xAAAAAAAAUL, 0x55555555UL,
        0x0000FFFFUL, 0xFFFF0000UL, 0x00FF00FFUL, 0xFF00FF00UL
    };
    uint32_t lfsr = 0xACE1BEEFUL;
    uint32_t i;

    for (i = 0; i < count; ++i)
    {
        if (i < 8)
        {
            words[i] = structured[i];
            continue;
        }

        /* x^32 + x^22 + x^2 + x^1 + 1, taps as the usual Galois form */
        lfsr = (lfsr >> 1) ^ (uint32_t)(-(int32_t)(lfsr & 1u) & 0xEDB88320UL);
        words[i] = lfsr;
    }
}

/* The four phase codes are the field's own encoding, 0 to 3 for 67.5, 78.75,
   90 and 101.25 degrees, so the index is the value. */
P4_SRAMCODE static void p4_set_dqs_phase(unsigned int phase)
{
    unsigned long v;

    v = p4_r32(P4_MSPI_PSRAM_DQS0) & ~P4_PSRAM_DQS_PHASE_MASK;
    p4_w32(P4_MSPI_PSRAM_DQS0,
           v | ((unsigned long)phase << P4_PSRAM_DQS_PHASE_SHIFT));
    v = p4_r32(P4_MSPI_PSRAM_DQS1) & ~P4_PSRAM_DQS_PHASE_MASK;
    p4_w32(P4_MSPI_PSRAM_DQS1,
           v | ((unsigned long)phase << P4_PSRAM_DQS_PHASE_SHIFT));
}

/*
 * One delay-line setting: every data pin gets one value, both strobes the
 * other.
 *
 * The strobe register carries two delays, for the rising and the falling
 * edge, and both take the same value: a DTR read samples on both and there is
 * no reason to want them apart.  The data value goes to all eighteen
 * non-strobe pins, clock and chip select included, which is what ESP-IDF
 * does; delaying the data lines relative to the strobe and delaying the clock
 * relative to the strobe are the same shift seen from either end.
 */
P4_SRAMCODE static void p4_set_delaylines(unsigned int data_delay,
                                          unsigned int dqs_delay)
{
    unsigned long v;
    unsigned int i;

    for (i = 0; i < P4_MSPI_PSRAM_GRP0_COUNT; ++i)
    {
        v = p4_r32(P4_MSPI_PSRAM_GRP0(i)) & ~P4_PSRAM_PIN_DLC_MASK;
        p4_w32(P4_MSPI_PSRAM_GRP0(i),
               v | ((unsigned long)data_delay << P4_PSRAM_PIN_DLC_SHIFT));
    }
    for (i = 0; i < P4_MSPI_PSRAM_GRP1_COUNT; ++i)
    {
        v = p4_r32(P4_MSPI_PSRAM_GRP1(i)) & ~P4_PSRAM_PIN_DLC_MASK;
        p4_w32(P4_MSPI_PSRAM_GRP1(i),
               v | ((unsigned long)data_delay << P4_PSRAM_PIN_DLC_SHIFT));
    }

    v = p4_r32(P4_MSPI_PSRAM_DQS0)
        & ~(P4_PSRAM_DQS_DLY90_MASK | P4_PSRAM_DQS_DLY270_MASK);
    p4_w32(P4_MSPI_PSRAM_DQS0,
           v | ((unsigned long)dqs_delay << P4_PSRAM_DQS_DLY90_SHIFT)
             | ((unsigned long)dqs_delay << P4_PSRAM_DQS_DLY270_SHIFT));
    v = p4_r32(P4_MSPI_PSRAM_DQS1)
        & ~(P4_PSRAM_DQS_DLY90_MASK | P4_PSRAM_DQS_DLY270_MASK);
    p4_w32(P4_MSPI_PSRAM_DQS1,
           v | ((unsigned long)dqs_delay << P4_PSRAM_DQS_DLY90_SHIFT)
             | ((unsigned long)dqs_delay << P4_PSRAM_DQS_DLY270_SHIFT));
}

/*
 * Neutral sampling: phase 0 and no delay anywhere.
 *
 * This is the state the bus runs at 20 MHz in, and the state to return to
 * before dropping back after a failed calibration.  Leaving a delay line set
 * while the clock falls is not dangerous, but it is a difference between the
 * fallback path and a plain slow boot, and the fallback has to be the plain
 * thing.
 */
P4_SRAMCODE void krnPSRAMTuningClear(void)
{
    p4_set_dqs_phase(0);
    p4_set_delaylines(0, 0);
}

/*
 * Candidate n of the relative-delay axis.
 *
 * Thirty-one steps: n below 15 delays the strobe by 15 - n, n above 15 delays
 * the data by n - 15, and 15 is neither.  So the axis runs from "strobe as
 * late as it goes" through zero to "data as late as it goes", monotonically,
 * which is what makes a run of passing candidates contiguous and its middle
 * meaningful.
 */
P4_SRAMCODE static void p4_delay_candidate(unsigned int n,
                                           unsigned int *data_delay,
                                           unsigned int *dqs_delay)
{
    if (n < 15)
    {
        *data_delay = 0;
        *dqs_delay = 15 - n;
    }
    else
    {
        *data_delay = n - 15;
        *dqs_delay = 0;
    }
}

/* Does the block still read back exactly?  tries reads, all of which have to
   match; the first failure is enough to reject the candidate. */
P4_SRAMCODE static int p4_candidate_holds(const uint32_t *reference,
                                          uint32_t *scratch,
                                          unsigned int tries)
{
    unsigned int t, i;

    for (t = 0; t < tries; ++t)
    {
        for (i = 0; i < P4_PSRAM_TUNE_WORDS; ++i)
            scratch[i] = 0;

        krnPSRAMBlockRead(P4_PSRAM_TUNE_ADDR, scratch, P4_PSRAM_TUNE_WORDS);

        for (i = 0; i < P4_PSRAM_TUNE_WORDS; ++i)
            if (scratch[i] != reference[i])
                return 0;
    }
    return 1;
}

/*
 * The longest run of set bits in a mask, as length and end index.
 *
 * Returned as the end rather than the start because the caller wants the
 * middle, and end minus half the length is the middle whether the run is odd
 * or even; deriving it from the start needs the same arithmetic with one more
 * chance to be off by one.
 */
P4_SRAMCODE static void p4_longest_run(unsigned long mask, unsigned int count,
                                       unsigned int *out_len,
                                       unsigned int *out_end)
{
    unsigned int best = 0, best_end = 0, run = 0, i;

    for (i = 0; i < count; ++i)
    {
        if (mask & (1UL << i))
        {
            ++run;
            if (run > best)
            {
                best = run;
                best_end = i;
            }
        }
        else
            run = 0;
    }

    *out_len = best;
    *out_end = best_end;
}

/*
 * The calibration.
 *
 * Returns non-zero and leaves the bus at fast_hz with the chosen sampling if
 * both stages found a window.  Returns zero with the caller's structure
 * filled in far enough to say which stage failed and what it saw; the caller
 * owns the decision to drop back, because dropping back is a policy and this
 * is a measurement.
 */
P4_SRAMCODE int krnPSRAMTune(struct P4PSRAMTuning *out, unsigned long fast_hz)
{
    P4_SRAMDATA static uint32_t reference[P4_PSRAM_TUNE_WORDS];
    P4_SRAMDATA static uint32_t scratch[P4_PSRAM_TUNE_WORDS];
    unsigned int len, end, i, data_delay, dqs_delay;

    out->tuned = 0;
    out->phase = 0;
    out->phase_pass = 0;
    out->phase_window = 0;
    out->delay_index = 0;
    out->delay_window = 0;
    out->delay_pass = 0;
    out->data_delay = 0;
    out->dqs_delay = 0;

    /* The reference goes in at the clock that has never failed, with neutral
       sampling, which is what makes it a reference. */
    krnPSRAMTuningClear();
    if (!krnPSRAMClockSet(20000000UL))
        return 0;
    krnPSRAMTuneReference(reference, P4_PSRAM_TUNE_WORDS);
    krnPSRAMBlockWrite(P4_PSRAM_TUNE_ADDR, reference, P4_PSRAM_TUNE_WORDS);

    /* Confirm it went in.  A reference that was never stored would make every
       candidate below fail identically and look like a hardware limit. */
    if (!p4_candidate_holds(reference, scratch, 1))
        return 0;

    if (!krnPSRAMClockSet(fast_hz))
        return 0;

    /* Stage one, the phase. */
    for (i = 0; i < P4_PSRAM_PHASE_COUNT; ++i)
    {
        p4_set_dqs_phase(i);
        p4_set_delaylines(0, 0);
        if (p4_candidate_holds(reference, scratch, P4_PSRAM_PHASE_TRIES))
            out->phase_pass |= (unsigned char)(1u << i);
    }

    p4_longest_run(out->phase_pass, P4_PSRAM_PHASE_COUNT, &len, &end);
    out->phase_window = (unsigned char)len;
    if (len == 0)
        return 0;

    /*
     * The first of the run, not the middle.
     *
     * Four coarse steps do not give a middle worth the name, and the four are
     * ordered by increasing shift, so the first that works is the one with the
     * most room left before the window closes on the far side.  ESP-IDF makes
     * the same choice.
     */
    out->phase = (unsigned char)(end - len + 1);
    p4_set_dqs_phase(out->phase);

    /* Stage two, the delay lines, with the phase now fixed. */
    for (i = 0; i < P4_PSRAM_DELAY_COUNT; ++i)
    {
        p4_delay_candidate(i, &data_delay, &dqs_delay);
        p4_set_delaylines(data_delay, dqs_delay);
        if (p4_candidate_holds(reference, scratch, P4_PSRAM_DELAY_TRIES))
            out->delay_pass |= (1UL << i);
    }

    p4_longest_run(out->delay_pass, P4_PSRAM_DELAY_COUNT, &len, &end);
    out->delay_window = (unsigned char)len;

    /*
     * A window of one is a coincidence, not a margin.  Refusing it here is
     * the difference between a boot that runs fast and a boot that runs fast
     * until the room warms up.
     */
    if (len < 2)
    {
        krnPSRAMTuningClear();
        return 0;
    }

    out->delay_index = (unsigned char)(end - len / 2);
    p4_delay_candidate(out->delay_index, &data_delay, &dqs_delay);
    out->data_delay = (unsigned char)data_delay;
    out->dqs_delay = (unsigned char)dqs_delay;
    p4_set_delaylines(data_delay, dqs_delay);

    /* And the chosen setting has to hold, which is not implied: it was
       measured, but the register writes since then were not. */
    if (!p4_candidate_holds(reference, scratch, P4_PSRAM_DELAY_TRIES))
    {
        krnPSRAMTuningClear();
        return 0;
    }

    out->tuned = 1;
    return 1;
}

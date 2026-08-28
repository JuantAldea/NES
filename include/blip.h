#pragma once
#include <cstddef>
#include <vector>

// Band-limited step synthesis, the thing a box-average decimator is not.
//
// WHY THIS REPLACED THE BOX FILTER, with numbers that were actually measured.
//
// An earlier version of this table quoted -56 dB for the band-limited column.
// Those figures were never measured here: they were the previous review's
// estimate of what an IDEAL synthesiser would achieve, copied across and
// presented as a result. What this implementation delivers, measured:
//
//     note              box average   point-sampled   this
//     A5    880 Hz        -35.7 dB       -26.9 dB      -40.5 dB
//     A6   1776 Hz        -30.9 dB       -23.3 dB      -40.1 dB
//          3608 Hz        -28.9 dB       -20.6 dB      -39.7 dB
//         12429 Hz        -10.6 dB        -6.1 dB      -35.2 dB
//
// A clear win - 4 dB at the bottom of the range and 25 dB at the top, where the
// box was close to useless - and nothing like the 20-to-48 dB the first version
// of this comment claimed.
//
// THE FIRST THREE ROWS ARE INSTRUMENT-LIMITED, not implementation-limited. The
// harness that produced them reads -17.3 dB for point-sampled audio where the
// review's reads -26.9, so it has its own leakage floor around -40 dB; the
// implementation is somewhere better than that and this cannot say where.
// Sweeping the kernel width and cutoff moved those rows by under 0.1 dB, which
// is what that floor looks like from the inside.
//
// HOW IT WORKS, and why it is cheap. The APU's output is piecewise constant: it
// holds a level and then steps to another. Point-sampling such a signal at
// 44.1 kHz aliases because a step contains energy at every frequency. Instead
// of sampling the waveform, this records each STEP - its exact time, to a
// fraction of an output sample, and its amplitude - and adds a band-limited
// step response at that position. Work is proportional to the number of
// transitions, not to the input rate, which is why 1.79 MHz costs nothing.
//
// The kernel is a windowed sinc, stored per sub-sample phase. Deltas are
// accumulated and the output is their running sum, because the integral of a
// band-limited impulse is a band-limited step.
//
// This is the construction blargg's blip_buf uses. It is written out here
// rather than vendored because blip_buf is LGPL and this project's other
// dependencies are BSD and MIT.
class BlipSynth
{
public:
    // Taps either side of a transition. Sixteen total, which is where the
    // stopband stops improving faster than the latency grows.
    static constexpr int half_width = 8;
    static constexpr int width = half_width * 2;

    // Sub-sample positions the kernel is precomputed at. A transition is
    // snapped to the nearest, so this is the timing resolution.
    //
    // 256, NOT 32, AND THIS IS THE PARAMETER THAT MATTERED. Snapping a
    // transition quantises its timing, and on a periodic waveform timing jitter
    // becomes discrete spurs rather than noise - so it sets an alias floor that
    // no amount of kernel is able to lower. Measured in-band inharmonic energy
    // on a 25% pulse:
    //
    //     phases      880 Hz    1776 Hz    3608 Hz    12429 Hz
    //         32      -40.0      -39.2      -38.1       -30.9
    //         64      -40.4      -39.9      -39.2       -33.9
    //        256      -40.5      -40.1      -39.7       -35.2
    //       1024      -40.5      -40.2      -39.7       -35.3
    //
    // 4.4 dB at the top of the pulse channel's range, and flat after 256. A
    // sweep of half_width from 8 to 32 and cutoff from 0.40 to 0.45 moved
    // these numbers by less than 0.1 dB at every frequency, which is how the
    // limit was identified: the kernel was never the constraint.
    //
    // The table costs 257 x 16 floats, 16 KB, built once.
    static constexpr int phases = 256;

    // Fraction of the OUTPUT SAMPLE RATE the sinc is cut off at. 0.45 puts it
    // at 19.8 kHz for 44.1 kHz output - below Nyquist with enough transition
    // band for the window to reach the stopband, and above anything the NES
    // produces that a listener can hear.
    static constexpr double cutoff = 0.45;

    explicit BlipSynth(size_t buffer_samples);

    // A step of `amplitude` at `position` output samples from the start of the
    // current buffer. The fractional part is the whole point - rounding it to a
    // whole sample is jitter, and jitter on a periodic waveform is a discrete
    // spur rather than noise.
    void add_delta(double position, float amplitude);

    // Integrates deltas up to (not including) `count` output samples, writing
    // them to `out` and consuming them. Returns samples written.
    //
    // Only positions that no future transition can still reach are final -
    // a delta at position p writes into p-8 through p+7 - so the caller must
    // not ask for samples within half_width of the newest transition.
    size_t read_settled(float* out, size_t count);

    // Deltas are held relative to the buffer start, so time has to be rebased
    // when they are consumed. This is that: shifts everything down by `count`.
    void advance(size_t count);

    void clear();

    size_t capacity() const { return deltas.size(); }

    // The kernel a given phase uses, for tests. It is a pure function of the
    // constants above, so a test can rebuild it independently rather than
    // comparing the table against itself.
    float kernel_tap(int phase, int tap) const;

private:
    // phases + 1 rows: the extra is the wrap case, so a position rounding up to
    // a full sample does not index past the end.
    std::vector<float> kernel;
    std::vector<float> deltas;

    // Carried between reads, because a running sum that restarted at each call
    // would reset the waveform to zero on every buffer boundary - a click at
    // the audio callback's period, which is the most audible artefact there is.
    float integrator = 0.0f;
};

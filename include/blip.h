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
// A clear win, and nothing like the 20-to-48 dB the first version of this
// comment claimed.
//
// TWO CAVEATS ON THAT TABLE. The box and point-sampled columns were measured
// through the OLD arrangement, with the filters at the input rate before
// decimation; the last column is the new one. They are not the same experiment,
// and the row-wise differences are taken across it.
//
// And these rows are INSTRUMENT-limited. An independent harness puts this
// implementation at -44.6 / -42.4 / -43.8 / -36.3, better everywhere. The
// evidence for that floor is the suspicious flatness of the first three - a
// real response does not sit within 1 dB across three octaves - and NOT, as an
// earlier version of this comment argued, that the harness disagrees with the
// review's point-sampled figure. It does disagree, but a third instrument sides
// with the harness, so that comparison was never evidence of anything.
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
    // Taps either side of a transition. THIRTY-TWO total, and this is the
    // parameter that actually binds - the opposite of what the first version of
    // this file concluded.
    //
    // Measured through the deployed chain, 25% duty pulse, in-band inharmonic
    // energy over harmonic:
    //
    //     half_width      880 Hz   1776 Hz   3608 Hz   12429 Hz
    //              8      -62.7     -59.6     -59.8     -44.7
    //             16      -67.3     -64.5     -61.0     -53.4
    //
    // 8.7 dB at the top of the pulse channel's range, for +8 output samples of
    // latency (0.18 ms) and no measurable passband change. It also moves the
    // chain's -3 dB point from 13908 Hz to 13982 Hz - CLOSER to the 14000 the
    // filter is built from.
    //
    // A Blackman window's transition band is about 5.5/width, so at 16 taps it
    // spans 0.34 normalised and the stopband does not begin until 0.62. The
    // second harmonic of a 12429 Hz tone lands at 0.564 - inside that
    // transition, and barely rejected. Doubling the width moves it out.
    static constexpr int half_width = 16;
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
    //         32      -44.0      -41.7      -41.6       -32.0
    //         64      -45.4      -43.4      -47.1       -35.2
    //        256      -46.0      -44.2      -55.3       -36.8
    //       1024      -46.0      -44.2      -57.1       -36.9
    //
    // About 5 dB at the top of the range, and flat after 256 THERE - a further
    // 0.1 dB. NOT flat everywhere: 3608 Hz gains another 1.9 dB past 256, which
    // a floored measurement of this same table reported as 0.0. The 256-phase
    // grid is already 6.3x finer than the input clock's own 1/40.58 spacing,
    // which bounds what remains past about 128. A
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

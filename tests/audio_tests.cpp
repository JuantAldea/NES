// The audio output path: the NES's analogue filter chain, decimation to a sound
// card's rate, and the buffer between the emulation and the audio callback.
//
// UNLIKE THE APU, SOME OF THIS IS OURS RATHER THAN THE HARDWARE'S, and the
// tests are split along that line. The filter corners are measured hardware
// values and are asserted against the transfer functions they imply. The
// resampler and the ring buffer are engineering choices, and what is asserted
// about them is internal consistency - sample counts that add up, data that
// comes back in the order it went in - not fidelity to a chip.
//
// The DC case is the one that ties back to the APU. Phase 3 established that an
// idle NES emits a constant offset, because the triangle's sequencer freezes
// holding a step rather than muting. That offset is exactly what the 90 Hz
// high-pass is there to remove, so "silence eventually reads as silence" is a
// property of the two halves together and is tested as one.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "../include/audio.h"
#include "../include/bus.h"
#include "gtest/gtest.h"

namespace tests
{
namespace audio
{

// Peak absolute value of the last `tail` samples, which is how a steady-state
// gain is read off a filter without assuming anything about phase.
float peak_of_tail(const std::vector<float>& samples, const size_t tail)
{
    float peak = 0.0f;
    const size_t start = samples.size() > tail ? samples.size() - tail : 0;
    for (size_t i = start; i < samples.size(); ++i) {
        peak = std::max(peak, std::fabs(samples[i]));
    }
    return peak;
}

// The filter's exact magnitude response, evaluated from its coefficients.
//
// NOT MEASURED FROM A SAMPLED SINE near the corner, which is what the peak
// finder below does and why it cannot be used there. At 14 kHz against a
// 44.1 kHz rate there are 3.15 samples per period, so the largest SAMPLE of a
// unit sine is well under 1.0 and the measured gain reads low - 0.7027 against
// the true 0.7071, an 0.63% error in the instrument that looks exactly like an
// 0.63% error in the filter.
double magnitude_at(const FirstOrderFilter& filter, const double hz, const double rate)
{
    const double w = 2.0 * 3.14159265358979 * hz / rate;
    const double b0 = filter.coefficient();
    const double b1 = filter.feedforward_1();
    const double a1 = filter.feedback_1();

    const double num_re = b0 + b1 * std::cos(w);
    const double num_im = -b1 * std::sin(w);
    const double den_re = 1.0 + a1 * std::cos(w);
    const double den_im = -a1 * std::sin(w);

    return std::sqrt((num_re * num_re + num_im * num_im) / (den_re * den_re + den_im * den_im));
}

// Runs a sine of `hz` through one filter at `rate` and returns its steady-state
// amplitude, having discarded the transient. Valid only well below Nyquist -
// see magnitude_at above.
float steady_state_gain(FirstOrderFilter& filter, const float hz, const float rate, const int cycles = 200)
{
    const int samples = static_cast<int>((rate / hz) * cycles);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / rate;
        out.push_back(filter.process(std::sin(2.0f * 3.14159265358979f * hz * t)));
    }
    return peak_of_tail(out, static_cast<size_t>(rate / hz) * 4);
}

// A first-order filter is 1/sqrt(2) - about 0.7071 - at its corner. That is
// what "corner frequency" MEANS, so this is the assertion that pins each
// coefficient to a frequency rather than to itself.
//
// AT 44.1 kHz, WHICH IS WHERE THEY NOW RUN. Band-limited synthesis moved the
// chain to the output rate, and that is what forced the bilinear transform:
// 14 kHz at 44.1 kHz is fc/fs = 0.317, where the two earlier discretisations
// are not close to -3dB. Measured, before the change:
//
//   naive RC,            fc/fs 0.0227     0.6835    3.3% low
//   impulse invariant,   fc/fs 0.0078     0.6899    2.4% low
//
// Bilinear with prewarping is exact at the corner by construction, at any
// ratio, so the tolerance here is float noise rather than a discretisation
// budget.
GTEST_TEST(testAudio, each_filter_is_minus_three_db_at_its_corner)
{
    const float rate = 44100.0f;
    for (const float corner : {90.0f, 440.0f, 14000.0f}) {
        const FirstOrderFilter low{FirstOrderFilter::Kind::low_pass, corner, rate};
        EXPECT_NEAR(0.70710678, magnitude_at(low, corner, rate), 1e-6) << "low-pass at " << corner;

        const FirstOrderFilter high{FirstOrderFilter::Kind::high_pass, corner, rate};
        EXPECT_NEAR(0.70710678, magnitude_at(high, corner, rate), 1e-6) << "high-pass at " << corner;
    }
}

GTEST_TEST(testAudio, a_high_pass_passes_above_its_corner_and_stops_below)
{
    const float rate = 44100.0f;
    FirstOrderFilter filter{FirstOrderFilter::Kind::high_pass, 440.0f, rate};

    const float well_above = steady_state_gain(filter, 4400.0f, rate);
    filter.reset();
    const float well_below = steady_state_gain(filter, 44.0f, rate);

    EXPECT_GT(well_above, 0.95f) << "a decade above the corner should pass nearly intact";
    EXPECT_LT(well_below, 0.15f) << "a decade below should be down about 20dB";
}

GTEST_TEST(testAudio, a_low_pass_passes_below_its_corner_and_stops_above)
{
    const float rate = 1789773.0f;
    FirstOrderFilter filter{FirstOrderFilter::Kind::low_pass, 14000.0f, rate};

    const float well_below = steady_state_gain(filter, 1400.0f, rate);
    filter.reset();
    const float well_above = steady_state_gain(filter, 140000.0f, rate);

    EXPECT_GT(well_below, 0.95f);
    EXPECT_LT(well_above, 0.15f);
}

// THE TWO ALPHAS ARE DIFFERENT QUANTITIES, and computing one with the other's
// formula is the mistake this pins. For the same corner and rate, the
// high-pass coefficient tends to 1 and the low-pass one to 0.
//
// Their summing to 1 is an ALGEBRAIC IDENTITY, not a measurement:
// RC/(RC+dt) + dt/(RC+dt) = 1 for every corner and every rate. Stronger than
// it looks - both sections share the pole, so H_lp(z) + H_hp(z) = 1 exactly and
// they are a complementary pair. That also means the third assertion below
// cannot fail unless the constructor stops using these two formulas, which the
// first two already catch. Kept for what it documents, not for what it tests.
GTEST_TEST(testAudio, the_two_filter_kinds_do_not_share_a_coefficient)
{
    const float rate = 1789773.0f;
    FirstOrderFilter high{FirstOrderFilter::Kind::high_pass, 90.0f, rate};
    FirstOrderFilter low{FirstOrderFilter::Kind::low_pass, 90.0f, rate};

    EXPECT_GT(high.coefficient(), 0.999f) << "a 90Hz high-pass at 1.79MHz is very nearly all-pass per sample";
    EXPECT_LT(low.coefficient(), 0.001f);
    EXPECT_NEAR(1.0f, high.coefficient() + low.coefficient(), 1e-5f) << "the two alphas are complements";
}

// A CONSTANT INPUT HAS NO TRANSITIONS, so band-limited synthesis emits nothing
// at all. This is stronger than the DC-removal the box filter needed the
// high-passes for: there is no DC to remove, because a level that never changes
// never enters the signal.
//
// It also means no startup click. The first push establishes the level rather
// than stepping to it from zero, which is right - an emulator beginning to run
// is not an edge on the hardware.
GTEST_TEST(testAudio, a_constant_input_produces_silence)
{
    AudioSampler sampler;
    for (int i = 0; i < 400000; ++i) {
        sampler.push(0.25f);
    }

    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 8000u) << "samples must still be produced, just silent ones";
    sampler.read(out.data(), out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(0.0f, out[i]) << "a level that never changes cannot produce output, at " << i;
    }
}

// AND A STEP DECAYS AWAY, which is what the high-passes are for. This is the
// property the previous "constant input decays to nothing" test was really
// checking - under the box filter a constant WAS a step, because the filter saw
// the transition from its own zero initial state.
GTEST_TEST(testAudio, a_step_decays_away)
{
    AudioSampler sampler;
    for (int i = 0; i < 100000; ++i) {
        sampler.push(0.0f);
    }
    std::vector<float> discard(sampler.available());
    sampler.read(discard.data(), discard.size());

    for (int i = 0; i < 400000; ++i) {
        sampler.push(0.25f);
    }
    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 8000u);
    sampler.read(out.data(), out.size());

    float peak = 0.0f;
    for (const float v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    EXPECT_GT(peak, 0.05f) << "the step itself must be audible";
    EXPECT_LT(std::fabs(out.back()), 1e-3f) << "and must not still be there nine seconds later";
}

// The same through the real APU. MEASURED: an idle APU emits exactly one
// distinct value, 0.246412, so under band-limited synthesis it produces
// literal silence rather than a decaying offset - the constant never steps.
GTEST_TEST(testAudio, an_idle_console_is_silent)
{
    Bus console;
    AudioSampler sampler;

    ASSERT_GT(console.apu.mixer_output(), 0.2f) << "an idle APU sits at a DC level - the triangle holds a step";

    for (int i = 0; i < 400000; ++i) {
        console.apu.clock();
        sampler.push(console.apu.mixer_output());
    }

    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 8000u);
    sampler.read(out.data(), out.size());
    for (const float v : out) {
        ASSERT_EQ(0.0f, v) << "an unchanging level produces no transitions and therefore no sound";
    }
}

// The rate arithmetic. 1789773 / 44100 is 40.58 input samples per output one,
// and the fraction is the point: truncating to 40 runs the audio 1.4% fast,
// about a quarter of a semitone, which is audible and would not fail any test
// that only checked "some samples came out".
GTEST_TEST(testAudio, the_output_rate_is_the_requested_one_including_the_fraction)
{
    AudioSampler sampler;
    const int pushes = 1789773;  // one second of CPU cycles
    for (int i = 0; i < pushes; ++i) {
        sampler.push(0.0f);
    }

    // Half a second of ring, so a full second overflows; the produced count is
    // what was read plus what was dropped.
    // The synthesiser holds back the samples its kernel can still reach, so a
    // second of input yields a second of output minus that tail - measured 68
    // samples out of 44100. That is a settling window, not a rate error, and
    // the distinction matters: truncating 40.58 to 40 would be 644 samples off
    // and is still nowhere near this tolerance.
    const uint64_t produced = sampler.available() + sampler.dropped();
    EXPECT_NEAR(44100.0, static_cast<double>(produced), 150.0) << "one second of input must be one second of output";
}

GTEST_TEST(testAudio, a_different_output_rate_changes_the_count_proportionally)
{
    AudioSampler sampler{48000.0f};
    for (int i = 0; i < 1789773; ++i) {
        sampler.push(0.0f);
    }
    const uint64_t produced = sampler.available() + sampler.dropped();
    EXPECT_NEAR(48000.0, static_cast<double>(produced), 150.0);
}

// --- the synthesiser, on its own --------------------------------------------

// A UNIT STEP MUST COME BACK AS A UNIT STEP. Every phase's kernel is normalised
// so its taps sum to 1, because that sum IS the height of the step produced for
// a unit transition. Un-normalised, a waveform's amplitude would depend on
// where between two output samples each edge happened to fall - an amplitude
// modulation at the beat between the waveform and the sample rate, which sounds
// like a wobble and looks like nothing in the code.
//
// Tested on BlipSynth directly. A first attempt measured this through
// AudioSampler and failed at 0.44 instead of 0.5, because at 44.1 kHz the
// 440 Hz high-pass has a time constant of about 16 output samples - the same
// timescale as the step's rise. The comment claiming the corners were "far
// below the test's timescale" was simply wrong. The filters have their own
// tests; this one is about the kernel.
GTEST_TEST(testAudio, every_phase_reconstructs_a_unit_step_at_unit_height)
{
    for (int phase = 0; phase <= BlipSynth::phases; ++phase) {
        double sum = 0.0;
        for (int tap = 0; tap < BlipSynth::width; ++tap) {
            sum += BlipSynth{256}.kernel_tap(phase, tap);
        }
        EXPECT_NEAR(1.0, sum, 1e-5) << "phase " << phase << " does not sum to 1";
    }
}

GTEST_TEST(testAudio, a_step_settles_at_the_amplitude_it_was_given)
{
    // Every sub-sample position, because the whole reason for the phase table
    // is that a transition between samples must not change the amplitude.
    for (int phase = 0; phase <= BlipSynth::phases; ++phase) {
        BlipSynth synth{256};
        const double position = 40.0 + static_cast<double>(phase) / BlipSynth::phases;
        synth.add_delta(position, 0.75f);

        std::vector<float> out(200);
        const size_t got = synth.read_settled(out.data(), out.size());
        ASSERT_GT(got, 100u);

        EXPECT_NEAR(0.75f, out[got - 1], 1e-4f) << "phase " << phase << " settles at the wrong height";
        EXPECT_NEAR(0.0f, out[0], 1e-4f) << "phase " << phase << " is not flat before the step";
    }
}

// Two steps that cancel must leave silence, which no single-step test shows.
GTEST_TEST(testAudio, opposing_steps_cancel_exactly)
{
    BlipSynth synth{256};
    synth.add_delta(40.0, 0.6f);
    synth.add_delta(80.5, -0.6f);

    std::vector<float> out(200);
    const size_t got = synth.read_settled(out.data(), out.size());
    ASSERT_GT(got, 120u);
    EXPECT_NEAR(0.0f, out[got - 1], 1e-4f) << "a step and its inverse must return to where they started";
}

// THE POINT OF THE REWRITE: energy above the output Nyquist must not fold back.
//
// A 25% DUTY PULSE, and the duty cycle is the point. This test used a 50%
// square, which is the one duty with NO EVEN HARMONICS - and so the one
// waveform structurally incapable of seeing the defect that hid behind it.
// Measured, same chain, 12429 Hz: halving the kernel width moves a 50% square
// by 0.03 dB and a 25% pulse by 8.7 dB. The NES pulse channel offers
// 12.5/25/50/75%, and three of those four have even harmonics.
//
// At 12429 Hz - the highest note the channel reaches - the harmonics sit at
// 24.9 kHz, 37 kHz and beyond, all above the 22.05 kHz Nyquist. Point-sampling
// folds them into the audible band; the synthesiser should not.
//
// Measured as the energy in the output that is NOT at the fundamental, relative
// to the fundamental.
GTEST_TEST(testAudio, a_high_square_wave_does_not_fold_energy_into_the_audible_band)
{
    const float input_rate = AudioSampler::ntsc_cpu_hz;
    AudioSampler sampler{44100.0f, input_rate};

    // A square wave by direct construction, so this tests the sampler and not
    // the APU: half a period high, half low, at 12429 Hz.
    const double period = input_rate / 12429.0;
    for (int i = 0; i < 900000; ++i) {
        const double phase = std::fmod(static_cast<double>(i), period);
        sampler.push(phase < period * 0.25 ? 0.5f : -0.5f);
    }

    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 8000u);
    sampler.read(out.data(), out.size());

    // Skip the transient, then measure total energy and the energy at the
    // fundamental via a single-bin Goertzel-style projection.
    const size_t skip = 2000;
    const double f = 12429.0 / 44100.0;
    double total = 0.0;
    double re = 0.0;
    double im = 0.0;
    for (size_t i = skip; i < out.size(); ++i) {
        const double v = out[i];
        const double t = 2.0 * 3.14159265358979 * f * static_cast<double>(i - skip);
        total += v * v;
        re += v * std::cos(t);
        im += v * std::sin(t);
    }
    const double n = static_cast<double>(out.size() - skip);
    const double fundamental = 2.0 * (re * re + im * im) / n;
    const double other = std::max(total - fundamental, 1e-30);

    // Everything above the fundamental in this signal is either a harmonic
    // above Nyquist - which must have been rejected - or an alias of one.
    const double ratio_db = 10.0 * std::log10(other / fundamental);
    // -32, NOT -25. Under THIS test's metric the box decimator this replaced
    // scores -28.7 dB, so a -25 bound passed with the very implementation the
    // rewrite existed to remove - it discriminated against point-sampling only.
    // The -10.6 dB the old comment quoted came from a different metric (25%
    // duty, harmonic-versus-inharmonic FFT) and did not apply here at all.
    // Measured margin at -32: about 4 dB.
    EXPECT_LT(ratio_db, -32.0) << "non-fundamental energy is " << ratio_db
                               << " dB; the box decimator scores -28.7 under this same metric";
}

// A KERNEL THAT IS WRONG BUT STILL NORMALISED passes every other test here.
// Three of five kernel mutations survived the whole file - shifting the tap
// offset by one, sliding the window two samples off the sinc, and dropping the
// cutoff from 0.45 to 0.25 - because every other assertion is on AMPLITUDE, and
// the constructor divides each phase by its own sum, so a normalisation check
// is tautological.
//
// Two properties catch them, and neither is an amplitude.

// TIMING. A transition asked for at position p must arrive at p. An offset
// error moves every edge by a whole sample and changes no amplitude anywhere,
// so nothing else in this file would see it.
//
// MEASURED, on the half-height crossing of the reconstructed step: exact at
// phase 0.0 and 0.5, and +-0.045 samples at 0.25 and 0.75. That residual is the
// crossing measure itself - the step is not a straight line between samples -
// and it is symmetric, so there is no systematic skew. A one-sample offset
// would be 20x outside this bound.
GTEST_TEST(testAudio, a_transition_arrives_where_it_was_asked_for)
{
    for (const double fraction : {0.0, 0.25, 0.5, 0.75}) {
        BlipSynth synth{1024};
        const double requested = 64.0 + fraction;
        synth.add_delta(requested, 1.0f);

        std::vector<float> step(512);
        const size_t got = synth.read_settled(step.data(), step.size());
        ASSERT_GT(got, 256u);

        double crossing = -1.0;
        for (size_t i = 1; i < got; ++i) {
            if (step[i - 1] < 0.5f && step[i] >= 0.5f) {
                crossing = static_cast<double>(i - 1) + (0.5 - step[i - 1]) / (step[i] - step[i - 1]);
                break;
            }
        }
        ASSERT_GT(crossing, 0.0) << "the step never reached half height";
        EXPECT_NEAR(requested, crossing, 0.1) << "phase " << fraction << " arrived at " << crossing;
    }
}

// THE FREQUENCY RESPONSE, which is what a wrong cutoff or a misaligned window
// changes while leaving DC gain and step height untouched.
//
// Measured on the kernel's own impulse - the reconstructed step, differenced,
// which undoes the integrator and leaves the windowed sinc. Normalised
// frequency, so 0.5 is Nyquist:
//
//     0.05  0.996      0.30  0.858      0.45  0.349
//     0.20  0.936      0.40  0.728      0.49  0.045
//
// The 0.40 and 0.49 bounds are set to DISCRIMINATE KERNEL WIDTH, which is the
// property the alias test above cannot see - its metric reads 32 taps and 16
// taps 0.4 dB apart where an independent instrument measures 8.7. Halving
// half_width widens the transition band: 0.40 falls to 0.607 and 0.49 rises to
// 0.067, both outside these bounds. That is the whole reason this test exists
// alongside the alias one.
GTEST_TEST(testAudio, the_kernel_passes_the_audible_band_and_rolls_off_before_nyquist)
{
    BlipSynth synth{1024};
    synth.add_delta(64.0, 1.0f);

    std::vector<float> step(512);
    const size_t got = synth.read_settled(step.data(), step.size());
    ASSERT_GT(got, 256u);

    std::vector<double> impulse(got);
    for (size_t i = 1; i < got; ++i) {
        impulse[i] = step[i] - step[i - 1];
    }

    const auto magnitude = [&](const double normalised) {
        double re = 0.0;
        double im = 0.0;
        for (size_t i = 0; i < got; ++i) {
            const double t = -2.0 * 3.14159265358979 * normalised * static_cast<double>(i);
            re += impulse[i] * std::cos(t);
            im += impulse[i] * std::sin(t);
        }
        return std::sqrt(re * re + im * im);
    };

    const double dc = magnitude(0.0);
    ASSERT_NEAR(1.0, dc, 1e-3) << "the kernel must pass DC at unity - this is the step height";

    EXPECT_GT(magnitude(0.05) / dc, 0.99) << "flat well inside the band";
    EXPECT_GT(magnitude(0.20) / dc, 0.90) << "still near-flat at 8.8kHz";
    EXPECT_GT(magnitude(0.40) / dc, 0.68) << "the passband must reach 0.40; a narrower kernel droops here";
    EXPECT_LT(magnitude(0.49) / dc, 0.055) << "and be nearly gone at Nyquist; this is what rejects aliases";

    // Nothing is asserted above 0.5. For a real signal the transform mirrors
    // about Nyquist, so magnitude(0.7) is identically magnitude(0.3) - an
    // earlier version asserted on it and was measuring its own reflection.
}

// THE STARTUP FIX HAD NO TEST, and reverting it was green across all 23 cases.
//
// AudioSampler begins at position = BlipSynth::width because add_delta drops any
// transition that would index before its buffer - which, from position 0, is
// every transition in the first 285 CPU cycles, while push() advances its level
// regardless. The step is then lost from the integrator permanently.
//
// Measured: a single step at CPU cycle 10 gives peak 0.9643 with the fix and
// EXACTLY 0.0 without, from the constructor and after clear() alike. That is
// about as clean a discriminator as a test gets, and it was missing.
GTEST_TEST(testAudio, a_transition_in_the_first_cycles_after_startup_is_not_lost)
{
    AudioSampler sampler;
    for (int i = 0; i < 500000; ++i) {
        sampler.push(i < 10 ? 0.0f : 1.0f);
    }

    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 4000u);
    sampler.read(out.data(), out.size());

    float peak = 0.0f;
    for (size_t i = 0; i < 4000; ++i) {
        peak = std::max(peak, std::fabs(out[i]));
    }
    EXPECT_GT(peak, 0.9f) << "a step at cycle 10 must survive; starting at position 0 loses it entirely";
}

// And the same after clear(), which restores the same invariant.
GTEST_TEST(testAudio, a_transition_just_after_clear_is_not_lost)
{
    AudioSampler sampler;
    for (int i = 0; i < 200000; ++i) {
        sampler.push(0.5f);
    }
    std::vector<float> discard(sampler.available());
    sampler.read(discard.data(), discard.size());
    sampler.clear();

    for (int i = 0; i < 500000; ++i) {
        sampler.push(i < 10 ? 0.0f : 1.0f);
    }
    std::vector<float> out(sampler.available());
    ASSERT_GT(out.size(), 4000u);
    sampler.read(out.data(), out.size());

    float peak = 0.0f;
    for (size_t i = 0; i < 4000; ++i) {
        peak = std::max(peak, std::fabs(out[i]));
    }
    EXPECT_GT(peak, 0.9f) << "clear() must restore the startup invariant, not reset position to 0";
}

// --- the ring buffer --------------------------------------------------------
//
// Tested DIRECTLY rather than through AudioSampler::push, which filters. An
// earlier version pushed a ramp and asserted it came back monotonic, at a rate
// where the 90 Hz high-pass reduces it to values around 1e-9 in descending
// order. That test was not wrong about the ring; it could not see the ring at
// all. SampleRing exists as a separate type because of it.

GTEST_TEST(testAudio, samples_come_back_in_the_order_they_went_in)
{
    SampleRing ring{16};
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(ring.write(static_cast<float>(i)));
    }
    ASSERT_EQ(10u, ring.available());

    std::vector<float> out(10);
    ASSERT_EQ(10u, ring.read(out.data(), 10));
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(static_cast<float>(i + 1), out[static_cast<size_t>(i)]) << "at index " << i;
    }
    EXPECT_EQ(0u, ring.available());
}

// The wraparound, driven far past the capacity so the indices lap many times.
// A wrap that dropped, duplicated or reordered breaks the sequence.
GTEST_TEST(testAudio, the_ring_wraps_without_losing_or_reordering)
{
    SampleRing ring{1024};
    float expected = 0.0f;
    float next = 0.0f;

    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 137; ++i) {
            ASSERT_TRUE(ring.write(next)) << "draining every round should never fill it";
            next += 1.0f;
        }
        std::vector<float> out(ring.available());
        ring.read(out.data(), out.size());
        for (const float value : out) {
            ASSERT_EQ(expected, value) << "the stream broke";
            expected += 1.0f;
        }
    }
    EXPECT_EQ(27400.0f, expected) << "every sample written must have come back exactly once";
}

// OVERFLOW DROPS THE NEWEST AND SAYS SO. Overwriting the oldest would reorder
// the stream, which sounds like an emulation fault rather than a dropout.
GTEST_TEST(testAudio, a_full_ring_refuses_the_write_rather_than_overwriting)
{
    SampleRing ring{1024};
    for (size_t i = 0; i < ring.capacity(); ++i) {
        ASSERT_TRUE(ring.write(static_cast<float>(i)));
    }
    EXPECT_FALSE(ring.write(-1.0f)) << "a full ring must refuse";

    // The oldest sample is still the oldest - nothing was overwritten.
    float first = -99.0f;
    ring.read(&first, 1);
    EXPECT_EQ(0.0f, first);
}

GTEST_TEST(testAudio, a_short_read_leaves_the_tail_it_could_not_fill)
{
    SampleRing ring{1024};
    for (int i = 0; i < 5; ++i) {
        ring.write(1.0f);
    }

    std::vector<float> out(20, -1.0f);
    EXPECT_EQ(5u, ring.read(out.data(), 20)) << "read must report what it actually wrote";
    EXPECT_EQ(-1.0f, out[5]) << "and must not touch what it did not write";
}

// --- the sampler's accounting -----------------------------------------------

GTEST_TEST(testAudio, overflow_is_counted_rather_than_absorbed)
{
    // Equal rates, so one output per input, and far more than the ring holds.
    AudioSampler sampler{1000.0f, 1000.0f};
    const size_t capacity = sampler.capacity();

    for (size_t i = 0; i < capacity + 4000; ++i) {
        sampler.push(static_cast<float>(i % 7));
    }
    EXPECT_EQ(capacity, sampler.available()) << "the ring must be full";
    EXPECT_GT(sampler.dropped(), 0u) << "every sample past the end must be counted, not silently lost";
}

GTEST_TEST(testAudio, a_short_read_is_counted)
{
    AudioSampler sampler{1000.0f, 1000.0f};

    std::vector<float> out(20, -1.0f);
    EXPECT_EQ(0u, sampler.read(out.data(), 20)) << "nothing has been pushed yet";
    EXPECT_EQ(1u, sampler.starved());
}

// The counters survive a clear, because they are the record of what a run lost.
GTEST_TEST(testAudio, clearing_the_buffer_keeps_the_loss_counters)
{
    AudioSampler sampler{1000.0f, 1000.0f};
    for (size_t i = 0; i < sampler.capacity() + 4000; ++i) {
        sampler.push(static_cast<float>(i % 7));
    }
    const uint64_t lost = sampler.dropped();
    ASSERT_GT(lost, 0u);

    sampler.clear();
    EXPECT_EQ(0u, sampler.available());
    EXPECT_EQ(lost, sampler.dropped()) << "a caller clearing between frames must not reset the evidence";
}

// --- the thing the ring exists for ------------------------------------------
//
// EVERY TEST ABOVE IS SINGLE-THREADED, and the first version of this ring
// passed all of them while being unsafe in three separate ways. It kept a
// `count` that both sides read-modify-wrote; measured at -O3 with no sanitizer,
// a producer and a consumer running concurrently made read() hand out 2.6x more
// samples than had ever been written - stale slots, re-read after the counter
// was corrupted upward. No out-of-bounds access, so ASan was silent, and this
// project runs no TSan.
//
// A test that exercises one thread cannot see any of that. This one runs both.
//
// It is a probabilistic test and says so: passing does not prove the ring is
// correct, it only fails when it is badly wrong. ThreadSanitizer is the tool
// that would prove it, and nothing here runs it - so the memory ordering in
// SampleRing rests on being the standard SPSC construction, and this checks
// that the construction was assembled correctly rather than that it is sound.
GTEST_TEST(testAudio, the_ring_conserves_every_sample_across_two_threads)
{
    SampleRing ring{1024};
    constexpr long long total = 2000000;

    // NO ASSERT_* BEFORE THE JOIN, and this is not style. gtest's ASSERT_
    // macros return from the enclosing function; doing that here left the
    // producer std::thread joinable, its destructor called std::terminate, and
    // a FAILING test became a process ABORT that took down the whole shard and
    // dumped core. Under the mutation harness - which exists to make this test
    // fail - that turned every ring mutant into a crash rather than a kill.
    //
    // So the consumer records the first mismatch, both threads are stopped
    // deliberately, and the assertions happen afterwards on plain data.
    std::atomic<bool> stop{false};
    std::atomic<long long> produced{0};

    std::thread producer([&] {
        long long next = 0;
        while (next < total && !stop.load(std::memory_order_relaxed)) {
            if (ring.write(static_cast<float>(next))) {
                ++next;
                produced.store(next, std::memory_order_relaxed);
            }
        }
    });

    long long expected = 0;
    long long consumed = 0;
    long long first_bad_at = -1;
    float first_bad_value = 0.0f;
    std::vector<float> buffer(256);

    // Bounded, so a ring that stops delivering fails rather than hanging. A
    // hung test is indistinguishable from a slow one and blocks the suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (consumed < total && std::chrono::steady_clock::now() < deadline) {
        const size_t got = ring.read(buffer.data(), buffer.size());
        for (size_t i = 0; i < got; ++i) {
            if (first_bad_at < 0 && buffer[i] != static_cast<float>(expected)) {
                first_bad_at = consumed;
                first_bad_value = buffer[i];
            }
            ++expected;
            ++consumed;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    // Drain, so a producer blocked on a full ring can see the stop flag.
    while (producer.joinable()) {
        ring.read(buffer.data(), buffer.size());
        producer.join();
    }

    EXPECT_EQ(-1, first_bad_at) << "the sequence broke at sample " << first_bad_at << ", which read " << first_bad_value
                                << " - a duplicate, a gap or a reorder";
    EXPECT_EQ(total, consumed) << "every sample written must be read exactly once; produced " << produced.load();
}

// --- the whole path, from a register write to a sample ----------------------
//
// Everything above tests a piece. This is the only test that goes from a CPU
// write to a playable sample, through the APU, the gates, the mixer, the
// synthesiser, the filters and the ring - which is the path the SDL layer
// depends on and the one nothing else covers end to end.
GTEST_TEST(testAudio, a_note_written_to_the_registers_reaches_the_sample_stream)
{
    Bus console;
    console.audio_enabled = true;

    // ~440 Hz on pulse 1: period = 1789773 / (16 * 440) - 1 = 253.
    console.apu.write(0x4015, 0x01);
    console.apu.write(0x4000, 0xBF);  // duty 2, constant volume 15, halt set
    console.apu.write(0x4001, 0x08);  // sweep off, negate so the target cannot overflow
    console.apu.write(0x4002, 253 & 0xFF);
    console.apu.write(0x4003, static_cast<uint8_t>((253 >> 8) | 0x08));

    std::vector<float> all;
    std::vector<float> chunk(4096);
    // Bus::clock() is a MASTER cycle, one CPU cycle in twelve - a loop labelled
    // in CPU cycles here would run a twelfth of its intended length, which has
    // already produced one confident wrong answer in this project.
    for (int i = 0; i < 12 * 400000; ++i) {
        console.clock();
        if (console.audio.available() >= chunk.size()) {
            const size_t got = console.audio.read(chunk.data(), chunk.size());
            all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
    }

    ASSERT_GT(all.size(), 8000u) << "the stream must be produced at roughly 44.1kHz";
    ASSERT_EQ(15, console.apu.pulse_level(APU::pulse1)) << "the channel must actually be sounding";

    size_t non_zero = 0;
    double energy = 0.0;
    for (const float v : all) {
        if (v != 0.0f) {
            ++non_zero;
        }
        energy += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(energy / static_cast<double>(all.size()));

    EXPECT_GT(non_zero, all.size() / 2) << "a sounding channel must not produce silence";
    EXPECT_GT(rms, 0.01) << "and must carry real energy, not just dither";
    EXPECT_LT(rms, 0.5) << "without clipping";
}

// The counterpart: with audio_enabled false - the default, which every other
// test in this repository runs under - nothing is collected at all.
GTEST_TEST(testAudio, the_sampler_is_off_unless_a_frontend_asks_for_it)
{
    Bus console;
    ASSERT_FALSE(console.audio_enabled) << "headless tests must not pay for a sample stream";

    console.apu.write(0x4015, 0x01);
    console.apu.write(0x4000, 0xBF);
    console.apu.write(0x4002, 0xFD);
    console.apu.write(0x4003, 0x08);
    for (int i = 0; i < 12 * 200000; ++i) {
        console.clock();
    }
    EXPECT_EQ(0u, console.audio.available());
}

}  // namespace audio
}  // namespace tests

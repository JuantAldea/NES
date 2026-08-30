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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "../include/audio.h"
#include "../include/bus.h"
#include "gtest/gtest.h"
#include "rom_fixture.h"

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
    // CONSTRUCTED ONCE. This used to build a BlipSynth inside the INNER loop -
    // 8224 constructions of the whole 257 x 32 table, each integrating every tap
    // by Simpson's rule over 64 sub-intervals. It cost 86 seconds, which was 96%
    // of the entire audio suite's runtime and multiplied the cost of every
    // mutation sweep over this file by about thirty.
    const BlipSynth synth{256};
    for (int phase = 0; phase <= BlipSynth::phases; ++phase) {
        double sum = 0.0;
        for (int tap = 0; tap < BlipSynth::width; ++tap) {
            sum += synth.kernel_tap(phase, tap);
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

    // Through Bus::write, not APU::write. The comment above claims this starts
    // at a CPU write and an earlier version bypassed the bus decode entirely,
    // which is the one part of "end to end" it was named for.
    console.write(0x4015, 0x01);
    console.write(0x4000, 0xBF);  // duty 2, constant volume 15, halt set
    console.write(0x4001, 0x08);  // sweep off, negate so the target cannot overflow
    console.write(0x4002, 253 & 0xFF);
    console.write(0x4003, static_cast<uint8_t>((253 >> 8) | 0x08));

    std::vector<float> all;
    std::vector<float> chunk(64);
    // Bus::clock() is a MASTER cycle, one CPU cycle in twelve - a loop labelled
    // in CPU cycles here would run a twelfth of its intended length, which has
    // already produced three confident wrong answers in this project.
    constexpr int cpu_cycles = 400000;
    for (int i = 0; i < 12 * cpu_cycles; ++i) {
        console.clock();
        // Drained in small blocks so the count is not quantised to a large
        // chunk size: an earlier version drained 4096 at a time, which made the
        // sample count a multiple of 4096 and blunted the rate assertion below.
        if (console.audio.available() >= chunk.size()) {
            const size_t got = console.audio.read(chunk.data(), chunk.size());
            all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
    }

    // THE RATE, BOUNDED ON BOTH SIDES. An earlier version asserted only
    // "more than 8000 samples", which an 88.2 kHz sampler passes as happily as
    // a 37 kHz one. 400000 CPU cycles is 0.2235 s, so 9856 samples at 44.1 kHz;
    // the shortfall is the synthesiser's fixed settling window, not a rate
    // error, which is why the lower bound is a little slack and the upper one
    // is not.
    const double seconds = cpu_cycles / static_cast<double>(AudioSampler::ntsc_cpu_hz);
    const double rate = all.size() / seconds;
    EXPECT_GT(rate, 43600.0) << "produced " << all.size() << " samples in " << seconds << " s";
    EXPECT_LT(rate, 44200.0) << "produced " << all.size() << " samples in " << seconds << " s";

    // The channel is sounding for a reason that does not depend on which duty
    // step the loop happened to stop on. pulse_level() is 0 on every low half
    // of the waveform, so asserting it equals 15 here is a coin toss dressed as
    // a precondition.
    ASSERT_NE(0, console.apu.length_counter(APU::pulse1)) << "the length counter must be loaded";
    ASSERT_FALSE(console.apu.sweep_is_muting(APU::pulse1)) << "and the sweep must not be muting it";

    size_t non_zero = 0;
    double energy = 0.0;
    for (const float v : all) {
        if (v != 0.0f) {
            ++non_zero;
        }
        energy += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(energy / static_cast<double>(all.size()));

    EXPECT_GT(non_zero, all.size() * 9 / 10) << "a sounding channel must not produce silence";

    // RMS PINNED TIGHTLY, because loose bounds here proved nothing. Measured
    // 0.0551 for the documented chain. With the three hardware filters deleted
    // it is 0.1049; with the synthesiser replaced by zero-order-hold decimation
    // it is 0.3285. A range of 0.01 to 0.5 - which is what this used to assert -
    // accepts all three, so the test could not tell the chain from no chain at
    // all, in the one place standing behind a resampler with no oracle.
    // THIS TEST DOES NOT CONSTRAIN THE CORNERS, and a comment in audio.h used to
    // say it did. Measured: moving the low-pass 1%, 14000 to 14140, moves this
    // rms from 0.055186 to 0.055197 - 0.02%, against a tolerance of 0.012 that
    // is 1100x larger. It pins the chain's LEVEL, which is what it says, and the
    // corners are pinned by the_sampler_builds_its_chain_at_the_documented_corners.
    EXPECT_NEAR(0.0551, rms, 0.012) << "the whole chain's output level; filters removed reads 0.105, ZOH 0.329";
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

// SUPER MARIO BROS. IS SILENT ON ITS TITLE SCREEN AND IN ITS DEMO, BY DESIGN.
//
// This looked like an audio bug for some hours and is not one. SMB's SoundEngine
// at $F2D0 is, verbatim from the published disassembly:
//
//     SoundEngine:  lda OperMode            ; $0770 - title screen mode?
//                   bne SndOn
//                   sta SND_MASTERCTRL_REG  ; if so, disable sound and leave
//                   rts
//
// https://github.com/nwoeanhinnogaehr/smb-assembler/blob/master/smbdis.asm
//
// The attract demo runs from INSIDE the title screen - RunDemo at $82C0 calls
// the game engine and then writes 0 back to OperMode - so OperMode stays 0 for
// the whole demo and the engine's entire body is "$4015 = $00; rts". A second
// guard says the same thing independently: GetAreaMusic opens with "lda
// OperMode / beq ExitGetM", so the area music is never even queued.
//
// Pinned because the evidence for it is a ROM disassembly rather than anything
// in this repository, and without a test the next person to notice the silence
// will re-derive it from scratch.
GTEST_TEST(commercialRom, smb_is_silent_until_it_leaves_the_title_screen)
{
    SKIP_IF_ROM_ABSENT(std::string(NES_TEST_FILES_DIR) + "/local/smb.nes",
                       "supply a dump of a cartridge you own - see tests/test_files/local/README.md");

    Bus console;
    ASSERT_TRUE(console.load_cartridge(std::string(NES_TEST_FILES_DIR) + "/local/smb.nes"));
    console.cpu.power_on();
    console.audio_enabled = true;

    // Well past the point the demo has started - Mario is walking by frame 1000.
    const uint64_t start = console.ppu.frame;
    while (console.ppu.frame - start < 1200) {
        console.clock();
    }

    EXPECT_EQ(0x00, console.read(0x0770)) << "OperMode: the demo runs in title-screen mode";
    EXPECT_EQ(0, console.apu.length_counter(APU::pulse1)) << "so the sound engine returns before loading anything";

    std::vector<float> out(console.audio.available());
    ASSERT_GT(out.size(), 1000u);
    console.audio.read(out.data(), out.size());
    for (const float v : out) {
        ASSERT_EQ(0.0f, v) << "and the stream is silent - the game asked for silence";
    }
}

// --- the only hardware oracle the audio has ---------------------------------
//
// Everything else here is measured against cited documentation or against the
// implementation's own model. This is measured against a NINTENDO
// ENTERTAINMENT SYSTEM: volume_tests ships nes-001.ogg, recorded from an NTSC
// U/C console with a PowerPak, and the twelve numbers below are that recording,
// reduced by the metric its own README prescribes.
//
//   "you can't just measure the maximum voltage; you have to measure the
//    difference between the high and low values"        - DC filters differ
//   "you have to compare relative volumes, not absolute volumes"  - headroom
//
// So: peak-to-peak per tone, normalised to tone 3. See
// tests/test_files/fetch_volume_tests.sh for how these were derived and for the
// three other emulators measured identically.
//
// THE TOLERANCE IS CALIBRATED, not guessed. Measured the same way against the
// same recording: fceux 2.49 dB worst-case and 1.05 rms, this emulator 2.99 and
// 1.66, Nestopia 3.51 and 1.66, Nintendulator 4.01 and 1.88. Three mature
// emulators span 1.05-1.88 dB rms, so the metric itself cannot resolve better
// than a couple of dB and a bound tighter than that would be measuring noise.
// 4.5 dB per tone and 2.5 dB rms passes all four and still catches a channel
// balance that is grossly wrong - swapping two tnd divisors moves a tone by
// 3.5 dB on top of whatever error is already there.
//
// A first reading of this emulator's triangle at -3.0 dB looked like a mixer
// bug. Nintendulator's is -4.0. Measuring the other emulators before drawing
// the conclusion is the only reason that was not chased.
//
// WHAT THIS CANNOT CATCH, so nobody reads a pass as more than it is. Swapping
// the triangle and noise divisors fails here, as does mis-scaling the DMC or
// the pulse group. Setting all three tnd divisors EQUAL does not: it moves the
// noise by 3.45 dB from a starting error of -1.6, which lands inside the bound
// and happens to point toward hardware rather than away. A tolerance tight
// enough to catch it would be tighter than the spread between fceux and
// Nintendulator, i.e. it would be measuring this metric's own noise.
//
// testAPUMixer covers that case, and covers every constant to its last digit.
// The two are complementary on purpose: those tests pin the formula, this one
// pins the result against a console.
constexpr float kHardwareLevels[12] = {
    0.9801f, 0.9962f, 1.0000f, 0.9922f,  // pulse 1 at 1/8, 1/4, 1/2, 3/4 duty
    1.6767f, 1.6996f, 1.7134f, 1.6906f,  // pulses 1+2 at the same four duties
    1.4594f,                             // triangle
    0.8199f, 0.8640f,                    // noise, long then short LFSR
    1.0534f,                             // DMC at amplitude 30
};

// The 1st and 99th percentile, so a single sample cannot set the level. The
// README asks for "the difference between the high and low values"; on a
// waveform with any overshoot the extremes are not that difference.
float peak_to_peak(std::vector<float> segment)
{
    std::sort(segment.begin(), segment.end());
    const size_t lo = segment.size() / 100;
    const size_t hi = segment.size() - 1 - lo;
    return segment[hi] - segment[lo];
}

// Splits the render into the twelve tones by short-time energy, and returns
// each one's peak-to-peak. Windows are 10 ms; a segment shorter than 200 ms is
// a transition rather than a tone.
std::vector<float> tone_levels(const std::vector<float>& samples)
{
    constexpr size_t window = 441;
    std::vector<float> envelope;
    for (size_t i = 0; i + window <= samples.size(); i += window) {
        double sum = 0.0;
        for (size_t j = 0; j < window; ++j) {
            sum += static_cast<double>(samples[i + j]) * samples[i + j];
        }
        envelope.push_back(static_cast<float>(std::sqrt(sum / window)));
    }
    if (envelope.empty()) {
        return {};
    }

    const float threshold = *std::max_element(envelope.begin(), envelope.end()) * 0.15f;
    std::vector<float> levels;
    size_t start = 0;
    bool inside = false;
    for (size_t i = 0; i < envelope.size() && levels.size() < 12; ++i) {
        const bool loud = envelope[i] > threshold;
        if (loud && !inside) {
            start = i;
            inside = true;
        } else if (!loud && inside) {
            inside = false;
            if (i - start < 20) {
                continue;
            }
            // Trimmed either side, so the attack and release are not measured.
            const size_t from = (start + 2) * window;
            const size_t to = (i - 2) * window;
            if (to > from && to - from >= 4410) {
                levels.push_back(peak_to_peak(std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(from),
                                                                 samples.begin() + static_cast<std::ptrdiff_t>(to))));
            }
        }
    }
    return levels;
}

GTEST_TEST(volumeTests, channel_balance_matches_a_real_nes)
{
    const std::string path = std::string(NES_TEST_FILES_DIR) + "/volume_tests/volumes.nes";
    REQUIRE_ROM(path, "run tests/test_files/fetch_volume_tests.sh");

    Bus console;
    ASSERT_TRUE(console.load_cartridge(path));
    console.cpu.power_on();
    console.audio_enabled = true;

    // NOTHING PLAYS UNTIL A IS PRESSED. The README says so, and a first render
    // of this ROM was seventeen seconds of silence before that was read
    // properly.
    constexpr long long master_hz = 21477272;
    const long long press_at = master_hz;
    const long long release_at = master_hz * 6 / 5;
    const long long total = master_hz * 17;

    std::vector<float> all;
    std::vector<float> chunk(4096);
    for (long long i = 0; i < total; ++i) {
        if (i == press_at) {
            console.controllers.press(0, Controllers::A);
        }
        if (i == release_at) {
            console.controllers.release(0, Controllers::A);
        }
        console.clock();
        if (console.audio.available() >= chunk.size()) {
            const size_t got = console.audio.read(chunk.data(), chunk.size());
            all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
        }
    }

    const std::vector<float> levels = tone_levels(all);
    ASSERT_EQ(12u, levels.size()) << "expected twelve tones, segmented " << levels.size()
                                  << ". The ROM plays pulse 1 at four duties, pulses 1+2 at four duties,\n"
                                     "  triangle, noise long, noise short and the DMC - if fewer were found the\n"
                                     "  emulator went silent partway, which is a different fault from a wrong level.";
    ASSERT_GT(levels[2], 0.0f) << "tone 3 is the normalisation reference and cannot be silent";

    static const char* const kNames[12] = {
        "pulse1 1/8",   "pulse1 1/4",   "pulse1 1/2", "pulse1 3/4", "pulse1+2 1/8", "pulse1+2 1/4",
        "pulse1+2 1/2", "pulse1+2 3/4", "triangle",   "noise long", "noise short",  "dmc 30",
    };

    double sum_squared = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double relative = levels[static_cast<size_t>(i)] / levels[2];
        const double error_db = 20.0 * std::log10(relative / kHardwareLevels[i]);
        sum_squared += error_db * error_db;

        EXPECT_LT(std::fabs(error_db), 4.5) << kNames[i] << " is " << error_db << " dB from a real NES (measured "
                                            << relative << ", hardware " << kHardwareLevels[i]
                                            << ").\n  Mature emulators land within 4.0 dB on this metric; see"
                                               " fetch_volume_tests.sh.";
    }

    const double rms_db = std::sqrt(sum_squared / 12.0);
    EXPECT_LT(rms_db, 2.5) << "overall channel balance is " << rms_db
                           << " dB rms from hardware. fceux measures 1.05, this emulator 1.66,"
                              " Nestopia 1.66, Nintendulator 1.88.";
}

// THE KERNEL, REBUILT FROM ITS DEFINITION AND COMPARED TAP BY TAP.
//
// blip.h invites exactly this: kernel_tap exists "for tests", because the table
// "is a pure function of the constants above, so a test can rebuild it
// independently rather than comparing the table against itself".
//
// It was needed. Mechanical mutation of blip.cpp's kernel construction found the
// window coefficients, the sinc bandwidth and the Simpson weights unconstrained -
// a 20-mutant sample killed 8. The two tests that look like they would cover this
// do not: the response test asserts only ONE-SIDED bounds, so anything that lifts
// the passband passes it, and the alias test has about 4 dB of margin while a
// coefficient nudge moves it under 2 dB.
//
// WHAT THIS PROVES AND WHAT IT DOES NOT. It proves the table matches the formula
// the file documents. It cannot prove the formula is the right one - a test that
// reimplements a specification never can. That belongs to the test below, which
// measures what the resulting filter does.
GTEST_TEST(testAudio, the_kernel_matches_its_definition_rebuilt_independently)
{
    constexpr double pi = 3.14159265358979323846;

    const auto sinc = [&](const double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x); };
    const auto blackman = [&](const double n, const double n_max) {
        const double t = n / n_max;
        return 0.42 - 0.5 * std::cos(2.0 * pi * t) + 0.08 * std::cos(4.0 * pi * t);
    };
    const auto impulse = [&](const double t) {
        return 2.0 * BlipSynth::cutoff * sinc(2.0 * BlipSynth::cutoff * t) *
               blackman(t + BlipSynth::half_width, BlipSynth::width);
    };
    // Simpson over 64 sub-intervals of [x-1, x]: the band-limited STEP increment.
    const auto step_increment = [&](const double x) {
        constexpr int steps = 64;
        const double h = 1.0 / steps;
        double total = impulse(x - 1.0) + impulse(x);
        for (int i = 1; i < steps; ++i) {
            total += (i % 2 == 1 ? 4.0 : 2.0) * impulse(x - 1.0 + h * i);
        }
        return total * h / 3.0;
    };

    const BlipSynth synth{1024};

    double worst = 0.0;
    int worst_phase = -1;
    int worst_tap = -1;
    for (int phase = 0; phase <= BlipSynth::phases; ++phase) {
        const double offset = static_cast<double>(phase) / BlipSynth::phases;

        std::vector<double> raw(static_cast<size_t>(BlipSynth::width));
        double sum = 0.0;
        for (int tap = 0; tap < BlipSynth::width; ++tap) {
            raw[static_cast<size_t>(tap)] =
                step_increment(static_cast<double>(tap - BlipSynth::half_width) + 1.0 - offset);
            sum += raw[static_cast<size_t>(tap)];
        }

        for (int tap = 0; tap < BlipSynth::width; ++tap) {
            // Normalised per phase in the same order and precision the
            // implementation uses - float value over float sum - so this compares
            // the formula and not the rounding.
            const float expected = static_cast<float>(raw[static_cast<size_t>(tap)]) / static_cast<float>(sum);
            const double deviation = std::fabs(static_cast<double>(expected - synth.kernel_tap(phase, tap)));
            if (deviation > worst) {
                worst = deviation;
                worst_phase = phase;
                worst_tap = tap;
            }
        }
    }

    // Taps reach about 0.2, so 1e-6 is a few parts per million: far below any
    // change to a coefficient and far above float rounding.
    EXPECT_LT(worst, 1e-6) << "kernel departs from its definition by " << worst << " at phase " << worst_phase
                           << ", tap " << worst_tap;
}

// THE WINDOW IS BLACKMAN-CLASS AND NOT HAMMING-CLASS, measured rather than
// assumed, because blip.cpp chooses one over the other on exactly this ground.
//
// VALIDATED AGAINST A KNOWN-BAD, which is the only reason the number below can be
// trusted: substituting Hamming (0.54 - 0.46cos) by hand and rebuilding moves the
// stopband peak from -80.9 dB to -60.7 dB. Both existing kernel tests - the
// response test and the alias test - PASS with that Hamming window installed. The
// window could be swapped wholesale and nothing in the suite noticed.
//
// AND THE FIGURES IN BLIP.CPP'S COMMENT ARE NOT THIS FILTER'S. It cites "about
// -74 dB against -41 dB". Those are the two windows' own asymptotic sidelobe
// levels, not the stopband of the windowed sinc built from them, which measures
// -80.9 and -60.7 here. The comparison it draws is sound; the numbers are a
// textbook's, and are labelled as such at their source now.
//
// The threshold is deliberately loose. It separates the two window FAMILIES with
// about 9 dB either side and is not sensitive enough to catch one coefficient
// moving - 0.42 to 0.43 costs 1.8 dB. That is the rebuild test's job, above.
GTEST_TEST(testAudio, the_kernel_reaches_a_blackman_stopband_a_hamming_window_would_not)
{
    const BlipSynth synth{1024};

    // The polyphase table reassembled into the oversampled impulse response it
    // decomposes. x = (tap - half_width) + 1 - phase/phases, so x rises with tap
    // ASCENDING and phase DESCENDING - getting that order wrong scrambles the
    // response into noise rather than failing visibly, which is what the DC
    // assertion below is for.
    const int n = BlipSynth::width * BlipSynth::phases;
    std::vector<double> h(static_cast<size_t>(n));
    for (int tap = 0; tap < BlipSynth::width; ++tap) {
        for (int p = 0; p < BlipSynth::phases; ++p) {
            h[static_cast<size_t>(tap * BlipSynth::phases + p)] = synth.kernel_tap(BlipSynth::phases - 1 - p, tap);
        }
    }

    // Frequency in cycles per OUTPUT sample, so 0.5 is the output Nyquist and
    // everything above it is an image this kernel exists to suppress.
    const auto magnitude = [&](const double f) {
        double re = 0.0;
        double im = 0.0;
        for (int m = 0; m < n; ++m) {
            const double t = -2.0 * 3.14159265358979323846 * f * static_cast<double>(m) / BlipSynth::phases;
            re += h[static_cast<size_t>(m)] * std::cos(t);
            im += h[static_cast<size_t>(m)] * std::sin(t);
        }
        return std::sqrt(re * re + im * im);
    };

    const double dc = magnitude(0.0);
    ASSERT_NEAR(BlipSynth::phases, dc, 1e-3) << "every phase is normalised to unit sum, so all " << BlipSynth::phases
                                             << " must sum to that at DC - if this fails the reassembly order is "
                                                "wrong and every number below is meaningless";

    // The transition band is about 5.5/width wide around the 0.45 cutoff, so the
    // stopband proper begins near 0.55. Scanned rather than spot-probed: the peak
    // sidelobe sits at 0.575 and a single probe elsewhere reads the skirt.
    double peak = 0.0;
    double peak_at = 0.0;
    for (double f = 0.55; f <= 8.0; f += 0.005) {
        const double m = magnitude(f) / dc;
        if (m > peak) {
            peak = m;
            peak_at = f;
        }
    }

    const double peak_db = 20.0 * std::log10(peak);
    EXPECT_LT(peak_db, -72.0) << "worst stopband sidelobe is " << peak_db << " dB at f=" << peak_at
                              << "; Blackman measures -80.9 here and Hamming -60.7";
}

// THE CHAIN'S THREE CORNERS ARE THE HARDWARE'S, and until this nothing checked
// which ones AudioSampler passed to its filters.
//
// The gap was structural rather than an oversight. each_filter_is_minus_three_db
// _at_its_corner tests FirstOrderFilter thoroughly - but it builds its own
// filters, at its own corners, so it says nothing about the sampler's. And the
// end-to-end test that looks like it would cover it,
// a_note_written_to_the_registers_reaches_the_sample_stream, measures the chain's
// rms level: moving the low-pass 1% changes that by 0.02%, inside a tolerance
// 1100x larger. Mechanical mutation found all three corners free to move.
//
// -3 dB AT THE CORNER IS WHAT "CORNER" MEANS, so this asserts the definition
// rather than the coefficients, and holds for any discretisation. A section
// built at 90.9 Hz reads 0.7036 at 90 Hz against 0.70711 - three thousand times
// the tolerance below, which is float noise.
//
// The frequencies are nesdev's, for the NES's own analogue output stage: two
// high-passes at 90 Hz and 440 Hz and a low-pass at 14 kHz.
GTEST_TEST(testAudio, the_sampler_builds_its_chain_at_the_documented_corners)
{
    const float rate = 44100.0f;
    const AudioSampler sampler{rate};

    EXPECT_NEAR(0.70710678, magnitude_at(sampler.first_high_pass(), 90.0, rate), 1e-6)
        << "the first high-pass is not cornered at 90 Hz";
    EXPECT_NEAR(0.70710678, magnitude_at(sampler.second_high_pass(), 440.0, rate), 1e-6)
        << "the second high-pass is not cornered at 440 Hz";
    EXPECT_NEAR(0.70710678, magnitude_at(sampler.low_pass(), 14000.0, rate), 1e-6)
        << "the low-pass is not cornered at 14 kHz";

    // The corners are built FROM the output rate, so they must follow it rather
    // than being right only at 44.1 kHz. This is also what distinguishes passing
    // the rate to the filter from hard-coding a coefficient.
    const float other = 96000.0f;
    const AudioSampler faster{other};
    EXPECT_NEAR(0.70710678, magnitude_at(faster.low_pass(), 14000.0, other), 1e-6)
        << "the 14 kHz corner did not follow the output rate";
}

// Half a second of audio, which is the sizing rule the constructor documents.
GTEST_TEST(testAudio, the_ring_is_sized_to_half_a_second_of_output)
{
    const AudioSampler sampler{44100.0f};
    EXPECT_EQ(static_cast<size_t>(44100.0f / 2.0f), sampler.capacity()) << "half a second at 44.1 kHz";
}

// --- what the mutation sweep of audio.cpp found nothing watching -------------
//
// 36 of 69 killed on the first pass. The five tests below close the survivors
// that are real; the rest are equivalent and recorded next to the code.

// reset() clears the MEMORY and not the coefficients, and nothing checked the
// first half: setting previous_input and previous_output to 1.0f instead of
// 0.0f survived the whole suite. A filter that kept its history across a reset
// would carry the last run's tail into the next one - audibly, since clear() is
// what runs between pause and resume.
GTEST_TEST(testAudio, resetting_a_filter_discards_the_samples_it_remembers)
{
    FirstOrderFilter filter{FirstOrderFilter::Kind::high_pass, 440.0f, 44100.0f};

    filter.process(1.0f);
    filter.process(1.0f);
    filter.reset();

    // Silence in, silence out - only true if BOTH remembered samples are zero.
    // previous_input surviving would leave b1, previous_output would leave -a1,
    // and neither is small for this corner.
    EXPECT_EQ(0.0f, filter.process(0.0f)) << "a reset filter fed silence must produce silence";
}

// THE SPARE SLOT AND THE FLOOR, the two things SampleRing's constructor does.
//
// `std::max<size_t>(1024, capacity) + 1` had four surviving mutants. The `+ 1`
// becoming `- 1` costs the spare slot the ring needs to tell full from empty;
// the 1024 becoming 1025, or the 1 becoming 2, moves the floor. All of them were
// invisible, including to the test that already asks for capacity() - because it
// asked at a size where the floor does not apply.
GTEST_TEST(testAudio, the_ring_holds_exactly_what_it_promises_at_and_below_the_floor)
{
    SampleRing large{2048};
    EXPECT_EQ(2048u, large.capacity()) << "above the floor, capacity is what was asked for";

    size_t written = 0;
    while (large.write(1.0f)) {
        ++written;
    }
    EXPECT_EQ(2048u, written) << "and it must actually accept that many - the spare slot is the "
                                 "difference between a full ring and an empty one";

    // Below the floor is where the 1024 itself is observable. A ring sized from
    // a low output rate would otherwise have capacity zero and drop everything.
    SampleRing small{10};
    EXPECT_EQ(1024u, small.capacity()) << "below the floor, the floor applies";
}

// A READ MUST NOT WRITE PAST THE COUNT IT WAS GIVEN, and this is the survivor
// that was a live buffer overrun rather than a tidiness question.
//
// `while (taken < requested && r != w)` survived both `<` becoming `<=` and `&&`
// becoming `||`. Either one writes one element beyond the caller's buffer
// whenever the ring holds more than was asked for - which is the normal case for
// an audio callback draining a full ring. Nothing in the suite noticed, because
// every existing read either drained the ring exactly or asked for more than it
// held, and neither reaches the boundary.
GTEST_TEST(testAudio, a_read_writes_no_further_than_the_count_it_was_given)
{
    SampleRing ring{64};
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(ring.write(static_cast<float>(i)));
    }

    // Ask for FEWER than are available, so the loop's count bound is what has to
    // stop it rather than the ring running dry.
    float out[8];
    std::fill(std::begin(out), std::end(out), -999.0f);
    const size_t got = ring.read(out, 5);

    EXPECT_EQ(5u, got);
    EXPECT_EQ(-999.0f, out[5]) << "read wrote past the 5 samples it was asked for";
    EXPECT_EQ(4.0f, out[4]) << "and the last sample it was asked for is the right one";
}

// A read that was fully satisfied is not a starve. `take < requested` becoming
// `take <= requested` counts one on EVERY successful read, which would turn the
// counter from evidence about the plumbing into a count of callbacks - the exact
// failure its own comment says it exists to avoid.
GTEST_TEST(testAudio, a_fully_satisfied_read_is_not_counted_as_a_starve)
{
    AudioSampler sampler;
    for (int i = 0; i < 200000; ++i) {
        sampler.push((i / 400) % 2 == 0 ? 0.25f : -0.25f);
    }

    const size_t available = sampler.available();
    ASSERT_GT(available, 16u) << "nothing to read, so the assertion below would be vacuous";

    std::vector<float> out(available);
    EXPECT_EQ(available, sampler.read(out.data(), available));
    EXPECT_EQ(0u, sampler.starved()) << "a read given exactly what the ring held was not short";

    // The other edge, so this cannot pass by never counting at all.
    std::vector<float> more(available);
    sampler.read(more.data(), available);
    EXPECT_EQ(1u, sampler.starved()) << "a read of an empty ring IS short and must be counted";
}

}  // namespace audio
}  // namespace tests

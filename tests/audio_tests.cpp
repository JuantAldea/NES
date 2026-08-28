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
#include <cmath>
#include <cstdint>
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

// Runs a sine of `hz` through one filter at `rate` and returns its steady-state
// amplitude, having discarded the transient.
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
// AT THE RATES THIS ACTUALLY RUNS AT, which matters more than it looks.
// The RC difference equation is an approximation whose corner drifts as
// fc/rate grows, and MEASURED here:
//
//   fc/rate 0.02268 (1kHz at 44.1kHz)      gain 0.6835   3.3% low
//   fc/rate 0.00782 (14kHz at 1.79MHz)     gain 0.6986   1.2% low
//   fc/rate 0.00025 (440Hz at 1.79MHz)     gain 0.7068   0.04% low
//   fc/rate 0.00005 (90Hz at 1.79MHz)      gain 0.7071   exact
//
// An earlier version tested at 44.1kHz and failed on that 3.3%, which is the
// discretisation and not a defect. The filters run at the INPUT rate, so the
// first row is a configuration this project never uses; testing there was
// asserting an error into existence. The 0.015 tolerance covers the worst of
// the three real corners with a little room.
GTEST_TEST(testAudio, each_filter_is_minus_three_db_at_its_corner)
{
    const float rate = AudioSampler::ntsc_cpu_hz;
    for (const float corner : {90.0f, 440.0f, 14000.0f}) {
        FirstOrderFilter low{FirstOrderFilter::Kind::low_pass, corner, rate};
        EXPECT_NEAR(0.7071f, steady_state_gain(low, corner, rate), 0.015f) << "low-pass at " << corner;

        FirstOrderFilter high{FirstOrderFilter::Kind::high_pass, corner, rate};
        EXPECT_NEAR(0.7071f, steady_state_gain(high, corner, rate), 0.015f) << "high-pass at " << corner;
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
GTEST_TEST(testAudio, the_two_filter_kinds_do_not_share_a_coefficient)
{
    const float rate = 1789773.0f;
    FirstOrderFilter high{FirstOrderFilter::Kind::high_pass, 90.0f, rate};
    FirstOrderFilter low{FirstOrderFilter::Kind::low_pass, 90.0f, rate};

    EXPECT_GT(high.coefficient(), 0.999f) << "a 90Hz high-pass at 1.79MHz is very nearly all-pass per sample";
    EXPECT_LT(low.coefficient(), 0.001f);
    EXPECT_NEAR(1.0f, high.coefficient() + low.coefficient(), 1e-5f) << "the two alphas are complements";
}

// DC IS REMOVED, which is the whole reason the hardware has these filters and
// the direct consequence of the APU's idle triangle offset.
GTEST_TEST(testAudio, a_constant_input_decays_to_nothing)
{
    AudioSampler sampler;
    for (int i = 0; i < 400000; ++i) {
        sampler.push(0.25f);
    }

    std::vector<float> out(sampler.available());
    ASSERT_FALSE(out.empty());
    sampler.read(out.data(), out.size());

    EXPECT_GT(std::fabs(out.front()), 0.01f) << "the step at the start must be audible; that is what a transient is";
    EXPECT_LT(std::fabs(out.back()), 1e-3f) << "and steady DC must be gone by the end";
}

// The same thing through the real APU, which is where the offset comes from.
GTEST_TEST(testAudio, an_idle_console_settles_to_silence)
{
    Bus console;
    AudioSampler sampler;

    ASSERT_GT(console.apu.mixer_output(), 0.2f) << "an idle APU emits DC - the triangle holds a step, it does not mute";

    for (int i = 0; i < 400000; ++i) {
        console.apu.clock();
        sampler.push(console.apu.mixer_output());
    }

    std::vector<float> out(sampler.available());
    ASSERT_FALSE(out.empty());
    sampler.read(out.data(), out.size());
    EXPECT_LT(std::fabs(out.back()), 1e-3f) << "the 90Hz high-pass is what makes an idle console silent";
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
    const uint64_t produced = sampler.available() + sampler.dropped();
    EXPECT_NEAR(44100.0, static_cast<double>(produced), 2.0) << "one second of input must be one second of output";
}

GTEST_TEST(testAudio, a_different_output_rate_changes_the_count_proportionally)
{
    AudioSampler sampler{48000.0f};
    for (int i = 0; i < 1789773; ++i) {
        sampler.push(0.0f);
    }
    const uint64_t produced = sampler.available() + sampler.dropped();
    EXPECT_NEAR(48000.0, static_cast<double>(produced), 2.0);
}

// EVERY input sample contributes, rather than one being taken and the rest
// discarded. A point-sampling implementation would report the last value in the
// window; averaging reports the mean, and the two differ on any input that is
// not constant.
GTEST_TEST(testAudio, an_output_sample_averages_its_whole_input_window)
{
    // Filters bypassed by feeding a rate high enough that a 41-sample window is
    // far below every corner - the point here is the decimator, not the chain.
    AudioSampler sampler{1.0f, 41.0f};

    // A ramp: the mean over the window is nowhere near its last value.
    for (int i = 0; i < 41; ++i) {
        sampler.push(static_cast<float>(i));
    }
    ASSERT_GE(sampler.available(), 1u);

    float out = 0.0f;
    sampler.read(&out, 1);
    // The filters attenuate the absolute value heavily, so the assertion is on
    // the RATIO: point-sampling would report the window's last value, which is
    // twice its mean for a ramp.
    EXPECT_LT(out, 30.0f) << "a point sampler would report something near the window's final value, 40";
}

// THE CHAIN IS BUILT WITH THE DOCUMENTED CORNERS. Every filter test above
// constructs a FirstOrderFilter directly, so none of them says anything about
// which frequencies AudioSampler wired up - changing its 90 to 90.9, its 440 to
// 444.4 or its 14000 to 14140 survived all of them.
//
// Asserted on the coefficients rather than the response, because a 1% corner
// shift moves the gain at that corner by about 0.3%, which is under the
// discretisation error the response tests already have to tolerate.
GTEST_TEST(testAudio, the_chain_uses_the_documented_corner_frequencies)
{
    AudioSampler sampler;
    const float rate = AudioSampler::ntsc_cpu_hz;

    const FirstOrderFilter expected_90{FirstOrderFilter::Kind::high_pass, 90.0f, rate};
    const FirstOrderFilter expected_440{FirstOrderFilter::Kind::high_pass, 440.0f, rate};
    const FirstOrderFilter expected_14k{FirstOrderFilter::Kind::low_pass, 14000.0f, rate};

    EXPECT_FLOAT_EQ(expected_90.coefficient(), sampler.high_pass_90_coefficient());
    EXPECT_FLOAT_EQ(expected_440.coefficient(), sampler.high_pass_440_coefficient());
    EXPECT_FLOAT_EQ(expected_14k.coefficient(), sampler.low_pass_14k_coefficient());
}

// THE FILTERS ARE BUILT FOR THE INPUT RATE, not the output rate. Getting that
// wrong puts every corner 40x off and is not visible in any gain measurement
// taken at the same wrong rate.
GTEST_TEST(testAudio, the_chain_is_built_for_the_input_rate)
{
    AudioSampler sampler{44100.0f, AudioSampler::ntsc_cpu_hz};
    const FirstOrderFilter at_input{FirstOrderFilter::Kind::low_pass, 14000.0f, AudioSampler::ntsc_cpu_hz};
    const FirstOrderFilter at_output{FirstOrderFilter::Kind::low_pass, 14000.0f, 44100.0f};

    ASSERT_NE(at_input.coefficient(), at_output.coefficient());
    EXPECT_FLOAT_EQ(at_input.coefficient(), sampler.low_pass_14k_coefficient());
}

// EVERYTHING RESETS. A filter or accumulator carrying state into a fresh run -
// or across a clear() - is a click at the start of playback, and nothing above
// looks at initial state at all: mutating any of the zero initialisers to 1
// survived the whole suite.
GTEST_TEST(testAudio, a_cleared_sampler_behaves_exactly_like_a_fresh_one)
{
    const auto run = [](AudioSampler& sampler) {
        std::vector<float> out;
        for (int i = 0; i < 200000; ++i) {
            sampler.push(i % 97 == 0 ? 0.5f : 0.1f);
        }
        out.resize(sampler.available());
        sampler.read(out.data(), out.size());
        return out;
    };

    AudioSampler fresh;
    const std::vector<float> first = run(fresh);

    AudioSampler reused;
    run(reused);
    reused.clear();
    const std::vector<float> second = run(reused);

    ASSERT_FALSE(first.empty());
    ASSERT_EQ(first.size(), second.size()) << "a cleared sampler must resume the same phase";
    for (size_t i = 0; i < first.size(); ++i) {
        ASSERT_FLOAT_EQ(first[i], second[i]) << "cleared and fresh diverge at sample " << i;
    }
}

// A read that is exactly satisfied is NOT starvation. `take < requested` and
// `take <= requested` differ only here, and nothing distinguished them.
GTEST_TEST(testAudio, an_exactly_satisfied_read_is_not_counted_as_starved)
{
    AudioSampler sampler{1.0f, 1.0f};
    for (int i = 0; i < 8; ++i) {
        sampler.push(1.0f);
    }

    std::vector<float> out(8);
    EXPECT_EQ(8u, sampler.read(out.data(), 8));
    EXPECT_EQ(0u, sampler.starved()) << "asking for exactly what is there is not a short read";
}

// PUSH IS THE FILTER CHAIN, IN ORDER, DIVIDED BY THE WINDOW. With the input and
// output rates equal the window is exactly one sample, so every output must
// equal the three filters applied directly - which pins the chain's order, the
// fact that all three are applied, and the accumulator's arithmetic in one
// comparison.
//
// The accumulator is why this exists. Leaving accumulated_count at 1 instead of
// 0 after emitting makes every window divide by one more than it summed - a
// constant ~2.4% gain error at the real rates, which no test comparing outputs
// to EACH OTHER can see, because it scales them all equally. Against a
// reference it is a factor of two here and obvious.
GTEST_TEST(testAudio, push_applies_the_three_filters_in_order_and_divides_by_the_window)
{
    const float rate = AudioSampler::ntsc_cpu_hz;
    AudioSampler sampler{rate, rate};  // one output per input: window of 1

    FirstOrderFilter high90{FirstOrderFilter::Kind::high_pass, 90.0f, rate};
    FirstOrderFilter high440{FirstOrderFilter::Kind::high_pass, 440.0f, rate};
    FirstOrderFilter low14k{FirstOrderFilter::Kind::low_pass, 14000.0f, rate};

    std::vector<float> expected;
    for (int i = 0; i < 500; ++i) {
        const float input = std::sin(static_cast<float>(i) * 0.01f) * 0.4f + 0.3f;
        sampler.push(input);
        expected.push_back(low14k.process(high440.process(high90.process(input))));
    }

    std::vector<float> actual(sampler.available());
    ASSERT_EQ(expected.size(), actual.size()) << "one output per input at equal rates";
    sampler.read(actual.data(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_FLOAT_EQ(expected[i], actual[i]) << "diverged at sample " << i;
    }
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
    AudioSampler sampler{1.0f, 1.0f};
    const size_t capacity = sampler.capacity();

    for (size_t i = 0; i < capacity + 500; ++i) {
        sampler.push(1.0f);
    }
    EXPECT_EQ(capacity, sampler.available());
    EXPECT_EQ(500u, sampler.dropped()) << "every sample past the end must be counted, not silently lost";
}

GTEST_TEST(testAudio, a_short_read_is_counted)
{
    AudioSampler sampler{1.0f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        sampler.push(1.0f);
    }

    std::vector<float> out(20, -1.0f);
    EXPECT_EQ(5u, sampler.read(out.data(), 20));
    EXPECT_EQ(1u, sampler.starved());
}

// The counters survive a clear, because they are the record of what a run lost.
GTEST_TEST(testAudio, clearing_the_buffer_keeps_the_loss_counters)
{
    AudioSampler sampler{1.0f, 1.0f};
    for (size_t i = 0; i < sampler.capacity() + 3; ++i) {
        sampler.push(1.0f);
    }
    ASSERT_EQ(3u, sampler.dropped());

    sampler.clear();
    EXPECT_EQ(0u, sampler.available());
    EXPECT_EQ(3u, sampler.dropped()) << "a caller clearing between frames must not reset the evidence";
}

}  // namespace audio
}  // namespace tests

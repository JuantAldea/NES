#include "../include/audio.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;
}  // namespace

FirstOrderFilter::FirstOrderFilter(const Kind filter_kind, const float corner_hz, const float rate_hz)
    : kind{filter_kind}
{
    // Prewarped, so the bilinear transform's frequency compression does not
    // move the corner: K = tan(pi * fc / fs) is where the analogue corner has
    // to sit for the digital one to land on fc.
    const float k = std::tan(static_cast<float>(kPi) * corner_hz / rate_hz);
    a1 = (k - 1.0f) / (k + 1.0f);

    if (kind == Kind::high_pass) {
        b0 = 1.0f / (1.0f + k);
        b1 = -b0;
    } else {
        b0 = k / (1.0f + k);
        b1 = b0;
    }
}

float FirstOrderFilter::process(const float sample)
{
    const float out = b0 * sample + b1 * previous_input - a1 * previous_output;
    previous_input = sample;
    previous_output = out;
    return out;
}

void FirstOrderFilter::reset()
{
    previous_input = 0.0f;
    previous_output = 0.0f;
}

// Half a second, but never fewer than 1024 samples, in capacity + 1 slots.
//
// The spare slot is what distinguishes a full ring from an empty one without a
// shared counter. The 1024 floor is not padding: sized purely from the output
// rate, a low rate gives a capacity of ZERO, the full-test is true immediately,
// and every sample is dropped - a sampler that silently produces nothing. Found
// by a test using a 1 Hz rate to make the decimator's arithmetic legible.
//
// The 0.0f fill is the one mutant here that survives, and it is UNREACHABLE
// rather than untested: read() stops at write_index, so no slot can be read
// before something has written it, and what it was initialised to cannot be
// observed. Swapping std::max's arguments is likewise equivalent - max is
// commutative - and so is the std::min in push().
SampleRing::SampleRing(const size_t capacity) : storage(std::max<size_t>(1024, capacity) + 1, 0.0f) {}

// PRODUCER SIDE. The release on write_index is what publishes the slot: it
// guarantees the consumer's acquire load cannot observe the new index without
// also observing the sample written before it.
bool SampleRing::write(const float sample)
{
    const size_t w = write_index.load(std::memory_order_relaxed);
    const size_t next = (w + 1) % storage.size();

    // Full when advancing would collide with the reader. Acquire, because a
    // stale read_index here can only make this look MORE full than it is -
    // which drops a sample rather than corrupting one.
    if (next == read_index.load(std::memory_order_acquire)) {
        return false;
    }

    storage[w] = sample;
    write_index.store(next, std::memory_order_release);
    return true;
}

// CONSUMER SIDE. Symmetric: acquire the producer's index, then release our own
// so the slots just consumed become available for overwriting.
size_t SampleRing::read(float* const out, const size_t requested)
{
    size_t r = read_index.load(std::memory_order_relaxed);
    const size_t w = write_index.load(std::memory_order_acquire);

    size_t taken = 0;
    while (taken < requested && r != w) {
        out[taken++] = storage[r];
        r = (r + 1) % storage.size();
    }

    read_index.store(r, std::memory_order_release);
    return taken;
}

size_t SampleRing::available() const
{
    const size_t w = write_index.load(std::memory_order_acquire);
    const size_t r = read_index.load(std::memory_order_acquire);
    return (w + storage.size() - r) % storage.size();
}

void SampleRing::clear()
{
    write_index.store(0, std::memory_order_relaxed);
    read_index.store(0, std::memory_order_relaxed);
}

AudioSampler::AudioSampler(const float output_rate_hz, const float input_rate_hz)
    : input_hz{input_rate_hz},
      output_hz{output_rate_hz},
      cycles_per_sample{static_cast<double>(input_rate_hz) / static_cast<double>(output_rate_hz)},
      // Room for a comfortable flush plus the kernel's reach.
      synth{4096},
      // AT THE OUTPUT RATE. The synthesiser has already band-limited the signal
      // by the time these run, so their job is tone shaping and nothing else.
      high_pass_90{FirstOrderFilter::Kind::high_pass, 90.0f, output_rate_hz},
      high_pass_440{FirstOrderFilter::Kind::high_pass, 440.0f, output_rate_hz},
      low_pass_14k{FirstOrderFilter::Kind::low_pass, 14000.0f, output_rate_hz},
      ring{static_cast<size_t>(output_rate_hz / 2.0f)}
{
    // NOT ZERO. A delta at position p writes into floor(p)-7 upwards, so a
    // transition below position 7 would index before the buffer and be dropped
    // - which is every transition in the first 285 CPU cycles after
    // construction or clear(). The comment on that clamp used to say it "should
    // never fire"; it fired on every startup, and because push() advances
    // last_level regardless, the step was lost from the integrator for good.
    //
    // Measured before the fix: a step at cycle 10 produced output that was
    // identically zero forever. In a running console the loss was the net level
    // change over those cycles, bled off by the 90 Hz high-pass over ~1.8 ms -
    // a click on every clear(), which is precisely the artefact this file calls
    // the most audible one there is.
    position = BlipSynth::width;
}

void AudioSampler::push(const float sample)
{
    // ONLY TRANSITIONS MATTER. A piecewise-constant signal spends most of its
    // time unchanged, so this is a comparison and a branch for the great
    // majority of the 1.79 million calls a second - which is why band-limited
    // synthesis is cheaper here than the box average it replaced, not dearer.
    if (!have_level) {
        last_level = sample;
        have_level = true;
    } else if (sample != last_level) {
        synth.add_delta(position, sample - last_level);
        last_level = sample;
    }

    position += 1.0 / cycles_per_sample;

    // Flush whatever is now beyond the kernel's reach. A delta at position p
    // writes into p-7 through p+8, so samples within half_width of the newest
    // transition are still incomplete and must not be emitted.
    const double settled = position - BlipSynth::width;
    if (settled < 64.0) {
        return;
    }

    const size_t count = static_cast<size_t>(settled);
    float scratch[4096];
    const size_t got = synth.read_settled(scratch, std::min(count, sizeof(scratch) / sizeof(scratch[0])));

    for (size_t i = 0; i < got; ++i) {
        float filtered = high_pass_90.process(scratch[i]);
        filtered = high_pass_440.process(filtered);
        filtered = low_pass_14k.process(filtered);

        // Counted rather than silently absorbed: a run that sounds wrong needs
        // to distinguish "the emulator produced the wrong samples" from "the
        // samples were fine and the plumbing lost them".
        if (!ring.write(filtered)) {
            dropped_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }

    synth.advance(got);
    position -= static_cast<double>(got);
}

size_t AudioSampler::read(float* const out, const size_t requested)
{
    const size_t take = ring.read(out, requested);
    if (take < requested) {
        starved_reads.fetch_add(1, std::memory_order_relaxed);
    }
    return take;
}

size_t AudioSampler::available() const { return ring.available(); }

// Exposed so tests do not restate the sizing rule. One that hard-coded 22050
// went on asserting a capacity the constructor had stopped producing.
size_t AudioSampler::capacity() const { return ring.capacity(); }

void AudioSampler::clear()
{
    ring.clear();
    synth.clear();
    position = BlipSynth::width;  // see the constructor
    have_level = false;
    high_pass_90.reset();
    high_pass_440.reset();
    low_pass_14k.reset();

    // The drop and starve counters deliberately SURVIVE a clear. They are a
    // record of what the run lost, and a caller clearing the buffer between
    // frames would otherwise reset the evidence every time.
}

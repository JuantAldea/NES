#include "../include/audio.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kTwoPi = 6.28318530717958647692f;
}  // namespace

// The standard RC discretisations, with alpha derived from the corner frequency
// and the sample rate rather than hard-coded - the same filter is used at three
// different corners and would otherwise be three near-copies.
//
//   high-pass:  y[n] = a * (y[n-1] + x[n] - x[n-1]),  a = RC / (RC + dt)
//   low-pass:   y[n] = y[n-1] + a * (x[n] - y[n-1]),  a = dt / (RC + dt)
//
// with RC = 1 / (2*pi*fc). Note the two alphas are NOT the same quantity: the
// high-pass one tends to 1 as the corner drops and the low-pass one tends to 0.
// Sharing a name for them is a real trap, so they are computed in one place.
FirstOrderFilter::FirstOrderFilter(const Kind filter_kind, const float corner_hz, const float rate_hz)
    : kind{filter_kind}
{
    const float rc = 1.0f / (kTwoPi * corner_hz);
    const float dt = 1.0f / rate_hz;
    alpha = kind == Kind::high_pass ? rc / (rc + dt) : dt / (rc + dt);
}

float FirstOrderFilter::process(const float sample)
{
    if (kind == Kind::high_pass) {
        const float out = alpha * (previous_output + sample - previous_input);
        previous_input = sample;
        previous_output = out;
        return out;
    }

    previous_output += alpha * (sample - previous_output);
    return previous_output;
}

void FirstOrderFilter::reset()
{
    previous_input = 0.0f;
    previous_output = 0.0f;
}

// Half a second, but never fewer than 1024 samples. The floor is not padding:
// sized purely from the rate, a low output rate gives a ring of ZERO, the
// full-test is true immediately, and every sample is dropped - a sampler that
// silently produces nothing. Found by a test using a 1 Hz rate to make the
// decimator's arithmetic legible.
// capacity + 1 slots, because the spare is what tells a full ring from an empty
// one without a shared counter. The 1024 floor is not padding: sized purely
// from the output rate, a low rate gives zero capacity, the full-test is true
// immediately, and every sample is dropped - a sampler that silently produces
// nothing.
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
      cycles_per_sample{input_rate_hz / output_rate_hz},
      // The filters run at the INPUT rate, so that is the rate they are built
      // for. Building them at the output rate would put every corner frequency
      // 40x off.
      high_pass_90{FirstOrderFilter::Kind::high_pass, 90.0f, input_rate_hz},
      high_pass_440{FirstOrderFilter::Kind::high_pass, 440.0f, input_rate_hz},
      low_pass_14k{FirstOrderFilter::Kind::low_pass, 14000.0f, input_rate_hz},
      ring{static_cast<size_t>(output_rate_hz / 2.0f)}
{
}

void AudioSampler::push(const float sample)
{
    float filtered = high_pass_90.process(sample);
    filtered = high_pass_440.process(filtered);
    filtered = low_pass_14k.process(filtered);

    // Every input sample in the output period contributes, rather than one
    // being taken and the other forty discarded. See the note on band-limited
    // synthesis in the header for what this is and is not.
    accumulated += filtered;
    ++accumulated_count;

    phase += 1.0f;
    if (phase < cycles_per_sample) {
        return;
    }
    phase -= cycles_per_sample;

    const float out = accumulated / static_cast<float>(accumulated_count);
    accumulated = 0.0f;
    accumulated_count = 0;

    // Counted rather than silently absorbed: a run that sounds wrong needs to
    // distinguish "the emulator produced the wrong samples" from "the samples
    // were fine and the plumbing lost them".
    if (!ring.write(out)) {
        dropped_samples.fetch_add(1, std::memory_order_relaxed);
    }
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
    phase = 0.0f;
    accumulated = 0.0f;
    accumulated_count = 0;
    high_pass_90.reset();
    high_pass_440.reset();
    low_pass_14k.reset();

    // The drop and starve counters deliberately SURVIVE a clear. They are a
    // record of what the run lost, and a caller clearing the buffer between
    // frames would otherwise reset the evidence every time.
}

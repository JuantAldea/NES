#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Turning the APU's per-cycle level into samples an audio device can play.
//
// Three separate jobs, kept separate because only the first two are the
// hardware's and the third is entirely ours:
//
//   the analogue filter chain the NES puts after its DAC
//   decimating ~1.79 MHz down to a sound card's rate
//   buffering, so an emulation thread and an audio callback can meet
//
// NOTHING HERE HAS AN ORACLE EITHER, and less than the APU does: the filter
// corner frequencies are measured values from real hardware, but the resampler
// is an engineering choice this project is making, not a chip behaviour to
// reproduce. Where that is true it is said so rather than dressed up.

// One first-order filter, the only kind the NES's output stage contains.
//
// nesdev, on what follows the DACs: "A first-order high-pass filter at 90 Hz,
// Another first-order high-pass filter at 440 Hz, A first-order low-pass filter
// at 14 kHz." The Famicom differs - one high-pass at 37 Hz, then "the unknown
// (and varying) properties of the RF modulator and demodulator" - and is not
// modelled, because "unknown and varying" is not something to approximate.
//
// https://www.nesdev.org/wiki/APU_Mixer
class FirstOrderFilter
{
public:
    enum class Kind { high_pass, low_pass };

    // `corner_hz` is the -3dB point; `rate_hz` the rate process() is called at.
    FirstOrderFilter(Kind kind, float corner_hz, float rate_hz);

    float process(float sample);

    // Discards the filter's memory without changing its coefficients.
    void reset();

    float coefficient() const { return alpha; }

private:
    Kind kind;
    float alpha;
    float previous_input = 0.0f;
    float previous_output = 0.0f;
};

// The whole path from APU levels to playable samples.
//
// THE FILTERS RUN AT THE INPUT RATE, before decimation, which is the one
// ordering decision here that is not arbitrary: the 14 kHz low-pass is the only
// thing attenuating content above the output Nyquist, so running it after
// decimation would filter aliases that had already folded down and be far too
// late.
//
// AND THIS IS NOT BAND-LIMITED SYNTHESIS. A first-order low-pass rolls off at
// 6 dB/octave, so content an octave above 14 kHz is only halved, and some of it
// still aliases. The accepted answer is a band-limited step synthesiser
// (blip_buf and its relatives), which reconstructs the waveform from its
// transitions rather than point-sampling it. Averaging every input sample in
// the output period, as below, is a box filter - strictly better than taking
// one sample and throwing the rest away, and strictly worse than doing it
// properly. Written down because the difference is audible on high pulse notes
// and someone will eventually wonder whether it was considered.
// The buffer between the emulation and the audio callback, on its own.
//
// SEPARATE FROM AudioSampler BECAUSE THE HEADER CLAIMED IT ALREADY WAS. This
// file opened by saying the three jobs are "kept separate", and they were not -
// the ring was a handful of members inside the sampler, reachable only through
// push(), which filters. Testing whether a wraparound reorders therefore meant
// pushing a ramp through a 90 Hz high-pass and asserting it came out monotonic,
// which it does not and should not. The claim was true of the intent and false
// of the code; this makes it true of the code.
class SampleRing
{
public:
    explicit SampleRing(size_t capacity);

    // Appends one sample. Returns false if the buffer was full, in which case
    // the sample is DROPPED rather than overwriting the oldest: overwriting
    // would reorder the stream and produce a click that sounds like an
    // emulation fault, where a gap sounds like what it is.
    bool write(float sample);

    // Copies up to `count` out, returning how many were written. A short read
    // leaves the untouched tail alone rather than zeroing it - the caller knows
    // what its device wants there, and this does not.
    size_t read(float* out, size_t count);

    size_t available() const { return count; }
    size_t capacity() const { return storage.size(); }
    void clear();

private:
    std::vector<float> storage;
    size_t write_index = 0;
    size_t read_index = 0;
    size_t count = 0;
};

class AudioSampler
{
public:
    // NTSC. The APU is clocked at the CPU rate, and this is sampled from
    // Bus::clock, so the input rate is the CPU's.
    static constexpr float ntsc_cpu_hz = 1789773.0f;

    explicit AudioSampler(float output_rate_hz = 44100.0f, float input_rate_hz = ntsc_cpu_hz);

    // One input sample, at the input rate. Filters it, and emits an output
    // sample whenever enough input has accumulated.
    void push(float sample);

    // Copies up to `count` samples out, returning how many were written. Short
    // reads are normal and are the caller's problem to pad - see the comment on
    // starved() for why this does not pad them itself.
    size_t read(float* out, size_t count);

    size_t available() const;
    size_t capacity() const;
    void clear();

    // How many output samples have been dropped because the buffer was full,
    // and how many reads have come up short. Both are counted rather than
    // silently absorbed: a run that sounds wrong needs to distinguish "the
    // emulator produced the wrong samples" from "the samples were fine and the
    // plumbing lost them", and without these two numbers those are
    // indistinguishable after the fact.
    uint64_t dropped() const { return dropped_samples; }
    uint64_t starved() const { return starved_reads; }

    float input_rate() const { return input_hz; }
    float output_rate() const { return output_hz; }

    // The chain's three coefficients, for inspection.
    //
    // Exposed because the RESPONSE cannot pin them tightly enough. A 1% shift
    // in a corner frequency moves the gain at that corner by about 0.3%, which
    // is smaller than the RC discretisation error already present - so a test
    // measuring gain has to carry a tolerance wide enough to let a wrong corner
    // through. The coefficient is an exact function of the corner and the rate,
    // so asserting it pins which frequencies this chain was actually built
    // with. Mutating 90 to 90.9 survived every response-based test.
    float high_pass_90_coefficient() const { return high_pass_90.coefficient(); }
    float high_pass_440_coefficient() const { return high_pass_440.coefficient(); }
    float low_pass_14k_coefficient() const { return low_pass_14k.coefficient(); }

private:
    float input_hz;
    float output_hz;

    // Input samples per output sample - 40.58 at NTSC/44.1kHz, and the
    // fractional part is why this is a phase accumulator rather than a counter.
    // Rounding it to 40 would run the audio 1.4% fast, which is about a quarter
    // of a semitone.
    float cycles_per_sample;
    float phase = 0.0f;

    float accumulated = 0.0f;
    int accumulated_count = 0;

    FirstOrderFilter high_pass_90;
    FirstOrderFilter high_pass_440;
    FirstOrderFilter low_pass_14k;

    // Roughly half a second, which is far more than any sane callback interval
    // and cheap enough not to tune.
    SampleRing ring;

    uint64_t dropped_samples = 0;
    uint64_t starved_reads = 0;
};

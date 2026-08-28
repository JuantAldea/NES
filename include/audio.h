#pragma once
#include <atomic>
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

// The buffer between the emulation and the audio callback, on its own.
//
// SINGLE PRODUCER, SINGLE CONSUMER, and lock-free by construction: the
// emulation thread writes, the audio callback reads, neither blocks.
//
// THE FIRST VERSION WAS NOT THREAD-SAFE AT ALL, and the failure was not subtle.
// It kept a `count` member both sides read-modify-wrote - a lost update, a torn
// read and a payload race in three lines. Measured at -O3 with no sanitizer,
// which is the configuration this project ships, one producer and one consumer:
//
//   written=19887570  read=52690101      2.6x more samples read than existed
//   written=18904291  read=31234723
//
// The excess are stale slots handed to the audio device after the count was
// corrupted upward: a loud repeating buzz that sounds like an emulation fault.
// No out-of-bounds access, because the indices are always taken modulo - so
// ASan could never see it, and nothing here runs TSan. It survived a green
// suite. The fix is the standard SPSC ring: no shared counter at all.
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

    // Safe from either side, and approximate from both: the other thread may
    // have moved its index by the time the answer is used. That is inherent -
    // a consumer can only be told a lower bound and a producer an upper one,
    // and each is what that caller needs.
    size_t available() const;

    size_t capacity() const { return storage.size() - 1; }

    // NOT THREAD-SAFE, and cannot be made so: it moves both indices. Callable
    // only while the audio device is stopped.
    void clear();

private:
    // capacity + 1 slots. The spare is what distinguishes a full ring from an
    // empty one once the shared counter is gone.
    std::vector<float> storage;
    std::atomic<size_t> write_index{0};
    std::atomic<size_t> read_index{0};
};

// The whole path from APU levels to playable samples.
//
// THE FILTERS RUN AT THE INPUT RATE, before decimation, because anything
// attenuating content above the output Nyquist has to act before that content
// folds down.
//
// THE BOX AVERAGE IS THE STRONGER OF THE TWO ANTI-ALIAS FILTERS over most of
// the range, which is the opposite of what the arrangement suggests. Measured
// attenuation before folding:
//
//     f_in      low-pass 14k   box(40.58)     folds to
//     22050        -5.4 dB       -3.9 dB       22050 Hz
//     30000        -7.5 dB       -8.1 dB       14100 Hz
//     39100        -9.4 dB      -18.0 dB        5000 Hz
//    100000       -17.2 dB      -19.6 dB       11800 Hz
//
// Above about 30 kHz the decimator does most of the work and its nulls sit on
// multiples of 44.1 kHz. An earlier version called the low-pass "the only thing
// attenuating content above the output Nyquist", which is false and hid the
// fact that the decimator is the anti-alias filter here.
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
    uint64_t dropped() const { return dropped_samples.load(std::memory_order_relaxed); }
    uint64_t starved() const { return starved_reads.load(std::memory_order_relaxed); }

    float input_rate() const { return input_hz; }
    float output_rate() const { return output_hz; }

    // THREE COEFFICIENT ACCESSORS WERE HERE AND ARE GONE. They were added
    // because a corner-frequency mutation survived, and justified by a comment
    // claiming it "survived every response-based test" - true of an earlier
    // state of the branch and false by the time it was committed. A reference
    // test written in the SAME commit already killed all three corner
    // mutations, and kills a 0.11% shift where these were sold as necessary for
    // 1%. Internals exposed to raise a mutation score, on a justification never
    // re-measured.

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

    // Atomic because they are written from OPPOSITE THREADS - dropped by the
    // producer in push(), starved by the consumer in read() - and read by
    // either. Relaxed is enough: they order nothing, they only count.
    std::atomic<uint64_t> dropped_samples{0};
    std::atomic<uint64_t> starved_reads{0};
};

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "blip.h"

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
    //
    // BILINEAR, WITH PREWARPING, which is exact at the corner by construction.
    //
    // Two earlier forms were not. The naive RC coefficient, dt/(RC+dt), drifts
    // as fc/fs grows: measured 0.6835 against 0.7071 at fc/fs = 0.0227. Then
    // exp(-2*pi*fc/fs) was tried on the grounds of being "the exact pole" - it
    // is exact for the POLE and still not for the -3dB point, because impulse
    // invariance matches the impulse response and aliases the frequency
    // response. It measured 0.6899 at fc/fs = 0.0078, worse than the form it
    // replaced.
    //
    // None of that mattered while everything ran at 1.79 MHz. Moving the chain
    // to the output rate puts the 14 kHz low-pass at fc/fs = 0.317, where the
    // difference between these forms is large, and made the question real.
    //
    // The bilinear transform has a zero as well as a pole, so this is a
    // two-coefficient filter rather than a one-alpha one.
    FirstOrderFilter(Kind kind, float corner_hz, float rate_hz);

    float process(float sample);

    // Discards the filter's memory without changing its coefficients.
    void reset();

    // The input coefficient. Low-pass and high-pass at the same corner sum to 1
    // algebraically - their numerators add to the shared denominator - but NOT
    // exactly in float: measured at 44.1 kHz the residual is -6.8e-08 at 90 Hz
    // and +7.1e-08 at 440 Hz, and only 14 kHz lands on exactly 1. "Exactly" was
    // the wrong word.
    float coefficient() const { return b0; }

    // The other two, so a test can evaluate the exact magnitude response rather
    // than measuring the peak of a sampled sine - which is unusable within a
    // few octaves of Nyquist, where there are too few samples per period for
    // any of them to land near the true peak.
    float feedforward_1() const { return b1; }
    float feedback_1() const { return a1; }

private:
    Kind kind;
    float b0;
    float b1;
    float a1;
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
// SEPARATE FROM AudioSampler SO IT CAN BE TESTED DIRECTLY. As members inside the
// sampler it was reachable only through push(), which filters - so asking
// whether a wraparound reorders meant pushing a ramp through a 90 Hz high-pass
// and asserting the output was monotonic, which it is not and should not be.
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
// IT IS BAND-LIMITED SYNTHESIS, and the order below follows from that.
//
// The APU's output is piecewise constant, so BlipSynth reconstructs it from its
// TRANSITIONS rather than sampling it. That fixes the aliasing (see blip.h for
// the measured numbers) but it constrains where the analogue filters can go:
// a filtered signal is no longer piecewise constant, so there are no
// transitions left to find. The synthesiser must see the raw stepped signal.
//
// So the chain runs at the OUTPUT rate, after synthesis, which is the reverse
// of what the box-filter version did. That was correct then and is wrong now:
// the previous arrangement needed the 14 kHz low-pass before decimation because
// nothing else attenuated above Nyquist, and the synthesiser's own kernel now
// does that job far better. The hardware filters are left doing what they
// actually are - tone shaping - rather than doubling as an anti-alias filter
// they were never good at.
//
// One consequence is worth stating: at 44.1 kHz the 14 kHz low-pass has
// fc/rate = 0.317, where the naive RC coefficient is badly wrong. See
// FirstOrderFilter for what replaced it.
//
// AND THE BILINEAR TRANSFORM IS EXACT AT EXACTLY ONE FREQUENCY. Its zero at
// z = -1 forces the response to zero at Nyquist, so above the corner it departs
// from the analogue filter nesdev describes: measured, +0.6 dB at 10 kHz, 0 at
// 14 kHz by construction, -3.4 dB at 18 kHz and -8.3 dB at 20 kHz. That is a
// defensible trade - the corner is the documented number and 20 kHz is at the
// edge of hearing - but it is a trade, not a free win.
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

    // The three sections of the tone chain, const, so a test can check WHICH
    // corner each was built with. There is no other way to see it: a filter does
    // not store its corner, and the chain's audible output barely depends on it.
    //
    // THESE WERE HERE, WERE REMOVED, AND ARE BACK, so the round trip is worth
    // recording. The removal's stated reason was that "a reference test written
    // in the SAME commit already killed all three corner mutations, and kills a
    // 0.11% shift where these were sold as necessary for 1%" - and that reference
    // test is a_note_written_to_the_registers_reaches_the_sample_stream, which
    // asserts an rms of 0.0551 to within 0.012.
    //
    // MEASURED, by moving the low-pass 1% to 14140 Hz and rebuilding: the rms
    // goes from 0.055186 to 0.055197. That is 0.02%, against a tolerance 1100x
    // larger, and the test passes comfortably. Mechanical mutation agrees - all
    // three of 90, 440 and 14000 survive the whole testAudio suite, that test
    // included.
    //
    // So the objection was right in principle and false in fact: internals should
    // not be exposed to raise a mutation score, but nothing was covering these,
    // and the sentence claiming otherwise was itself never measured - which is
    // exactly what it accused the code it deleted of.
    const FirstOrderFilter& first_high_pass() const { return high_pass_90; }
    const FirstOrderFilter& second_high_pass() const { return high_pass_440; }
    const FirstOrderFilter& low_pass() const { return low_pass_14k; }

private:
    float input_hz;
    float output_hz;

    // Input samples per output sample - 40.58 at NTSC/44.1kHz. The fractional
    // part is the whole reason a transition's position is a double: rounding it
    // to 40 would run the audio 1.4% fast, about a quarter of a semitone, and
    // rounding each transition to a whole output sample is the +-0.5 sample
    // jitter the box version had and never documented.
    double cycles_per_sample;

    // Where we are, in output samples, since the last flush.
    //
    // BOTH INITIALISERS ARE DEAD, which is why mutating either survives the
    // suite. position is overwritten with BlipSynth::width by the constructor
    // and again by clear() - it must be, and the comment at both sites says why -
    // and last_level is not read until have_level is true, which only the first
    // push() makes it. They are unreachable rather than untested, and are kept
    // because a member left uninitialised is worse than one initialised twice.
    double position = 0.0;
    float last_level = 0.0f;
    bool have_level = false;

    BlipSynth synth;

    // AT THE OUTPUT RATE now, not the input rate - see the note above.
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

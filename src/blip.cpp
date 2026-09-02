#include "../include/blip.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;

double sinc(const double x)
{
    if (std::fabs(x) < 1e-12) {
        return 1.0;
    }
    return std::sin(kPi * x) / (kPi * x);
}

// Blackman, chosen over Hamming for the stopband, for a transition band roughly
// 1.5x wider. With the cutoff at 0.45 there is room for the wider transition.
//
// TWO SETS OF NUMBERS, easily confused. A window's own asymptotic sidelobe level
// - about -74 dB for Blackman, -41 dB for Hamming - is a textbook figure
// describing the WINDOW. What the windowed sinc built from it delivers is a
// different quantity, MEASURED over the reassembled polyphase kernel by
// testAudio.the_kernel_reaches_a_blackman_stopband_a_hamming_window_would_not:
//
//     Blackman   -80.9 dB      Hamming   -60.7 dB
//
// Hamming was installed by hand to get that second number, and both kernel tests
// that existed at the time passed with it in place - the window could be swapped
// wholesale and the suite did not notice. That is what the stopband test is for.
double blackman(const double n, const double n_max)
{
    const double t = n / n_max;
    return 0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
}

// The windowed sinc itself: the band-limited IMPULSE.
double impulse(const double t)
{
    return 2.0 * BlipSynth::cutoff * sinc(2.0 * BlipSynth::cutoff * t) *
           blackman(t + BlipSynth::half_width, BlipSynth::width);
}

// Its integral over one output sample, [x-1, x]. Simpson's rule over 64
// sub-intervals, which is far finer than the kernel's own curvature.
double step_increment(const double x)
{
    constexpr int steps = 64;
    const double h = 1.0 / steps;
    double total = impulse(x - 1.0) + impulse(x);
    for (int i = 1; i < steps; ++i) {
        total += (i % 2 == 1 ? 4.0 : 2.0) * impulse(x - 1.0 + h * i);
    }
    return total * h / 3.0;
}
}  // namespace

BlipSynth::BlipSynth(const size_t buffer_samples)
    : kernel(static_cast<size_t>(phases + 1) * width, 0.0f), deltas(buffer_samples + width, 0.0f)
{
    // EACH TAP IS S(x) - S(x-1), NOT h(x), and that difference is the whole
    // correctness of this class.
    //
    // read_settled reconstructs by ACCUMULATING taps. Accumulation is
    // 1/(1 - z^-1); the band-limited step it is supposed to produce is the
    // integral of the impulse. Storing h(x) and accumulating therefore applies
    // a rectangular integrator instead of a true one, which is not a subtle
    // difference: measured, it boosts the passband by 1/sinc(f/fs) - +1.43 dB
    // at 14 kHz - and advances everything by half a sample.
    //
    // The gain error is the part that matters. With that boost feeding the
    // 14 kHz low-pass, the chain's -3dB point sat at 15730 Hz against the
    // 14000 Hz nesdev value the filter is built from - a hardware-measured
    // corner 12% out.
    //
    // An earlier version of this said 16287 Hz and called it MEASURED. It was
    // not: it was the analytic chain times the theoretical 1/sinc factor, which
    // ignores the old kernel's own rolloff above 16 kHz. 15730 is what the code
    // actually produced. Writing "measured" over a calculation is the exact
    // failure the commit containing this comment opened by confessing to,
    // repeated about its own headline number.
    //
    // blip_buf does not do this. Its table construction integrates the impulse
    // and takes a first difference across one output sample - "integrate, first
    // difference, rescale, convert to int" - so its stored tap is the band-
    // limited STEP, differenced. An earlier version of this file claimed to be
    // "the construction blargg's blip_buf uses" while omitting exactly that
    // step. Storing the integral directly, as here, is the same thing arrived
    // at analytically rather than by running sums.
    for (int phase = 0; phase <= phases; ++phase) {
        const double offset = static_cast<double>(phase) / phases;
        double sum = 0.0;

        for (int tap = 0; tap < width; ++tap) {
            const double x = static_cast<double>(tap - half_width) + 1.0 - offset;
            const double value = step_increment(x);
            kernel[static_cast<size_t>(phase) * width + static_cast<size_t>(tap)] = static_cast<float>(value);
            sum += value;
        }

        // NORMALISED PER PHASE. The taps telescope to S(last) - S(first-1), so
        // their sum IS the height of the step produced for a unit transition.
        // Unnormalised, a waveform's amplitude would depend on where between
        // two output samples each edge fell - an amplitude modulation at the
        // beat between the waveform and the sample rate.
        //
        // THIS IS ALSO WHY FIVE MUTANTS ABOVE ARE EQUIVALENT, recorded here so
        // nobody re-derives it. Anything that scales step_increment UNIFORMLY
        // divides straight back out - (k*v)/(k*s) = v/s - which covers the
        // impulse's leading `2.0 * cutoff` amplitude factor and Simpson's
        // `/ 3.0`, but NOT the `2.0` inside sinc's argument, which sets the
        // bandwidth and is killed. A sixth, `blackman(t + half_width)` becoming
        // `t - half_width`, is equivalent for a different reason: the window is
        // built from cosines, so blackman(-n) = blackman(n), and it is symmetric
        // about its midpoint, so blackman(32-n) = blackman(n) - together those
        // give blackman(t-16) = blackman(16+t) identically.
        //
        // Those are not assertions. The rebuild test compares this table against
        // an INDEPENDENTLY recomputed one, so a mutant that survives it has
        // produced a table matching to within 1e-6 - below the float precision
        // the table is stored at. For this kernel a survivor is an answer rather
        // than a question.
        for (int tap = 0; tap < width; ++tap) {
            kernel[static_cast<size_t>(phase) * width + static_cast<size_t>(tap)] /= static_cast<float>(sum);
        }
    }
}

float BlipSynth::kernel_tap(const int phase, const int tap) const
{
    return kernel[static_cast<size_t>(phase) * width + static_cast<size_t>(tap)];
}

void BlipSynth::add_delta(const double position, const float amplitude)
{
    const double base = std::floor(position);
    const int phase = static_cast<int>((position - base) * phases + 0.5);

    // The kernel reaches half_width-1 samples back, so a transition below
    // position 7 would write before the buffer starts.
    //
    // THIS USED TO FIRE ON EVERY STARTUP, silently. AudioSampler began at
    // position 0, so every transition in the first 285 CPU cycles was dropped
    // while push() advanced its level anyway - the step lost from the
    // integrator permanently. It now begins at width, which is why this is
    // again what the comment always claimed: a guard, not a live path.
    const long long start = static_cast<long long>(base) - half_width + 1;
    if (start < 0 || static_cast<size_t>(start) + width > deltas.size()) {
        return;
    }

    const float* const taps = &kernel[static_cast<size_t>(phase) * width];
    for (int tap = 0; tap < width; ++tap) {
        deltas[static_cast<size_t>(start) + static_cast<size_t>(tap)] += amplitude * taps[tap];
    }
}

size_t BlipSynth::read_settled(float* const out, const size_t count)
{
    const size_t take = std::min(count, deltas.size() > width ? deltas.size() - width : 0);
    for (size_t i = 0; i < take; ++i) {
        integrator += deltas[i];
        out[i] = integrator;
    }
    return take;
}

void BlipSynth::advance(const size_t count)
{
    if (count == 0) {
        return;
    }
    if (count >= deltas.size()) {
        std::fill(deltas.begin(), deltas.end(), 0.0f);
        return;
    }
    std::move(deltas.begin() + static_cast<std::ptrdiff_t>(count), deltas.end(), deltas.begin());
    std::fill(deltas.end() - static_cast<std::ptrdiff_t>(count), deltas.end(), 0.0f);
}

void BlipSynth::clear()
{
    std::fill(deltas.begin(), deltas.end(), 0.0f);
    integrator = 0.0f;
}

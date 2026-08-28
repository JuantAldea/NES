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

// Blackman, chosen over Hamming for the stopband: about -74 dB against -41 dB,
// for a transition band roughly 1.5x wider. With the cutoff at 0.45 there is
// room for the wider transition and no room for -41 dB of alias.
double blackman(const double n, const double n_max)
{
    const double t = n / n_max;
    return 0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
}
}  // namespace

BlipSynth::BlipSynth(const size_t buffer_samples)
    : kernel(static_cast<size_t>(phases + 1) * width, 0.0f), deltas(buffer_samples + width, 0.0f)
{
    // One windowed sinc per sub-sample phase.
    //
    // NORMALISED PER PHASE, and that is not cosmetic. The running sum of a
    // phase's taps is the step this synthesiser produces for a unit transition,
    // so if the taps do not sum to 1 the waveform's amplitude depends on WHERE
    // between two samples each edge happened to fall. On a square wave that is
    // an amplitude modulation at the beat frequency between the waveform and
    // the sample rate - which sounds like a wobble and looks like nothing in
    // the code.
    for (int phase = 0; phase <= phases; ++phase) {
        const double offset = static_cast<double>(phase) / phases;
        double sum = 0.0;

        for (int tap = 0; tap < width; ++tap) {
            // Position of this tap relative to the transition, in output
            // samples. The transition sits between taps half_width-1 and
            // half_width, displaced by the phase.
            const double x = static_cast<double>(tap - half_width) + 1.0 - offset;
            const double value = 2.0 * cutoff * sinc(2.0 * cutoff * x) * blackman(x + half_width, width);
            kernel[static_cast<size_t>(phase) * width + static_cast<size_t>(tap)] = static_cast<float>(value);
            sum += value;
        }

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

    // The kernel reaches half_width-1 samples back, so a transition in the
    // first few samples of the buffer would write before its start. Deltas are
    // held with that much headroom and the caller keeps the newest transition
    // clear of the read point, so this clamp should never fire - it is here
    // because silently corrupting the sample before the buffer would be
    // inaudible until it was not.
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

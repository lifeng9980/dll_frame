#include "IR_analysis.h"
#include "api_common.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace ir::api;

namespace
{

constexpr double kPi = std::numbers::pi;
using Complex = std::complex<double>;

bool isPowerOfTwo(size_t n) noexcept
{
    return n > 0 && (n & (n - 1)) == 0;
}

size_t nextPowerOfTwo(size_t n) noexcept
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

void radix2Fft(std::vector<Complex> &data, bool inverse)
{
    const size_t n = data.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(data[i], data[j]);
    }

    const double sign = inverse ? 1.0 : -1.0;
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = sign * (2.0 * kPi / static_cast<double>(len));
        const Complex wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len)
        {
            Complex w(1.0, 0.0);
            const size_t half = len >> 1;
            for (size_t j = 0; j < half; ++j)
            {
                const Complex u = data[i + j];
                const Complex v = data[i + j + half] * w;
                data[i + j] = u + v;
                data[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse)
    {
        const double invN = 1.0 / static_cast<double>(n);
        for (Complex &z : data)
            z *= invN;
    }
}

std::vector<Complex> computeDftComplex(const std::vector<double> &input)
{
    const size_t n = input.size();
    if (isPowerOfTwo(n))
    {
        std::vector<Complex> data(n);
        for (size_t i = 0; i < n; ++i)
            data[i] = Complex(input[i], 0.0);
        radix2Fft(data, false);
        return data;
    }

    // Bluestein chirp-z transform handles any arbitrary N >= 2 in O(N log N).
    const size_t m = nextPowerOfTwo(2 * n - 1);
    std::vector<Complex> a(m, Complex(0.0, 0.0));
    std::vector<Complex> b(m, Complex(0.0, 0.0));
    std::vector<Complex> chirp(n);

    const int64_t twoN = 2 * static_cast<int64_t>(n);
    for (size_t k = 0; k < n; ++k)
    {
        const int64_t kSqMod = (static_cast<int64_t>(k) * static_cast<int64_t>(k)) % twoN;
        const double angle = kPi * static_cast<double>(kSqMod) / static_cast<double>(n);
        chirp[k] = Complex(std::cos(angle), -std::sin(angle));
        a[k] = Complex(input[k], 0.0) * chirp[k];
    }

    b[0] = Complex(1.0, 0.0);
    for (size_t k = 1; k < n; ++k)
    {
        const Complex conjChirp = std::conj(chirp[k]);
        b[k] = conjChirp;
        b[m - k] = conjChirp;
    }

    radix2Fft(a, false);
    radix2Fft(b, false);
    for (size_t i = 0; i < m; ++i)
        a[i] *= b[i];
    radix2Fft(a, true);

    std::vector<Complex> out(n);
    for (size_t k = 0; k < n; ++k)
        out[k] = a[k] * chirp[k];
    return out;
}

void computeRfftCore(const double *signal,
                     int32_t length,
                     double sampleRateHz,
                     int32_t windowKind,
                     int32_t detrendMean,
                     double *outFreqsHz,
                     double *outAmplitude,
                     double *outPsd)
{
    double mean = 0.0;
    if (detrendMean != 0)
    {
        double sum = 0.0;
        for (int32_t i = 0; i < length; ++i)
            sum += signal[i];
        mean = sum / static_cast<double>(length);
    }

    std::vector<double> windowed(static_cast<size_t>(length), 0.0);
    double windowSum = 0.0;
    double windowSqSum = 0.0;
    for (int32_t i = 0; i < length; ++i)
    {
        double w = 1.0;
        if (windowKind == IR_WINDOW_HANN)
            w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / static_cast<double>(length - 1)));
        else if (windowKind == IR_WINDOW_HAMMING)
            w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / static_cast<double>(length - 1));
        windowed[static_cast<size_t>(i)] = (signal[i] - mean) * w;
        windowSum += w;
        windowSqSum += w * w;
    }
    require(windowSum > 0.0 && windowSqSum > 0.0, IR_BAD_ARGUMENT,
            "Selected window yields zero weight for this signal length");

    const std::vector<Complex> spectrum = computeDftComplex(windowed);
    const int32_t bins = length / 2 + 1;
    const double df = sampleRateHz / static_cast<double>(length);

    for (int32_t bin = 0; bin < bins; ++bin)
    {
        const Complex &z = spectrum[static_cast<size_t>(bin)];
        const double mag = std::hypot(z.real(), z.imag());
        const bool edgeBin = (bin == 0) || ((length % 2 == 0) && (bin == length / 2));
        const double ampScale = edgeBin ? 1.0 : 2.0;

        if (outFreqsHz)
            outFreqsHz[bin] = static_cast<double>(bin) * df;
        outAmplitude[bin] = mag * ampScale / windowSum;
        if (outPsd)
            outPsd[bin] = (mag * mag) * ampScale / (sampleRateHz * windowSqSum);
    }
}

void validateWindowKind(int32_t windowKind)
{
    require(windowKind == IR_WINDOW_RECTANGULAR ||
                windowKind == IR_WINDOW_HANN ||
                windowKind == IR_WINDOW_HAMMING,
            IR_BAD_ARGUMENT, "Unsupported FFT window_kind");
}

} // namespace

extern "C" IR_ANALYSIS_API int32_t IR_ComputeRfft1D(
    const double *signal,
    int32_t length,
    double sample_rate_hz,
    int32_t window_kind,
    int32_t detrend_mean,
    double *out_freqs_hz,
    double *out_amplitude,
    double *out_psd,
    int32_t out_bins_capacity,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        require(signal != nullptr && out_amplitude != nullptr, IR_NULL_POINTER,
                "signal and out_amplitude must not be null");
        require(length >= 2, IR_BAD_ARGUMENT, "FFT signal length must be >= 2");
        require(std::isfinite(sample_rate_hz) && sample_rate_hz > 0.0, IR_BAD_ARGUMENT,
                "sample_rate_hz must be finite and positive");
        validateWindowKind(window_kind);
        const int32_t bins = length / 2 + 1;
        needed(out_amplitude, out_bins_capacity, bins);
        if (out_freqs_hz)
            needed(out_freqs_hz, out_bins_capacity, bins);
        if (out_psd)
            needed(out_psd, out_bins_capacity, bins);

        for (int32_t i = 0; i < length; ++i)
            require(std::isfinite(signal[i]), IR_BAD_ARGUMENT, "FFT input signal must be finite");

        computeRfftCore(signal, length, sample_rate_hz, window_kind, detrend_mean,
                        out_freqs_hz, out_amplitude, out_psd);
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_ChannelSpectrumU16(
    const uint16_t *frames,
    int64_t frame_elements,
    int32_t frame_count,
    int32_t rows,
    int32_t cols,
    int32_t axis,
    int32_t channel_count,
    double sample_rate_hz,
    int32_t window_kind,
    double *out_freqs_hz,
    int32_t freq_capacity,
    double *out_channel_series,
    int64_t series_capacity,
    double *out_channel_amplitudes,
    int64_t amplitude_capacity,
    double *out_temporal_means,
    double *out_temporal_stds,
    int32_t channel_stats_capacity,
    char *err,
    int32_t err_bytes)
{
    return guarded(err, err_bytes, [&] {
        const int64_t n = pixels(rows, cols);
        require(frame_count >= 2, IR_BAD_ARGUMENT, "Channel spectrum requires at least 2 frames");
        needed(frames, frame_elements, stackSize(n, frame_count));
        require(axis == IR_SPECTRUM_AXIS_COLUMNS || axis == IR_SPECTRUM_AXIS_ROWS,
                IR_BAD_ARGUMENT, "axis must be IR_SPECTRUM_AXIS_COLUMNS or IR_SPECTRUM_AXIS_ROWS");
        require(channel_count >= 1, IR_BAD_ARGUMENT, "channel_count must be >= 1");
        const int32_t axisLength = (axis == IR_SPECTRUM_AXIS_COLUMNS) ? cols : rows;
        require(axisLength % channel_count == 0, IR_BAD_ARGUMENT,
                "Image axis length must be divisible by channel_count");
        require(std::isfinite(sample_rate_hz) && sample_rate_hz > 0.0, IR_BAD_ARGUMENT,
                "sample_rate_hz must be finite and positive");
        validateWindowKind(window_kind);

        const int32_t bins = frame_count / 2 + 1;
        const int64_t requiredSeries = int64_t(channel_count) * frame_count;
        const int64_t requiredAmps = int64_t(channel_count) * bins;

        needed(out_channel_amplitudes, amplitude_capacity, requiredAmps);
        if (out_freqs_hz)
            needed(out_freqs_hz, freq_capacity, bins);
        if (out_channel_series)
            needed(out_channel_series, series_capacity, requiredSeries);
        if (out_temporal_means)
            needed(out_temporal_means, channel_stats_capacity, channel_count);
        if (out_temporal_stds)
            needed(out_temporal_stds, channel_stats_capacity, channel_count);

        std::vector<std::vector<double>> channelSeries(
            static_cast<size_t>(channel_count), std::vector<double>(static_cast<size_t>(frame_count), 0.0));

        const int64_t samplesPerChannel = n / channel_count;
        std::vector<long double> sums(static_cast<size_t>(channel_count), 0.0L);

        for (int32_t f = 0; f < frame_count; ++f)
        {
            std::fill(sums.begin(), sums.end(), 0.0L);
            const uint16_t *framePtr = frames + int64_t(f) * n;
            for (int32_t r = 0; r < rows; ++r)
            {
                const uint16_t *rowPtr = framePtr + int64_t(r) * cols;
                if (axis == IR_SPECTRUM_AXIS_ROWS)
                {
                    const int32_t ch = r % channel_count;
                    long double rowSum = 0.0L;
                    for (int32_t c = 0; c < cols; ++c)
                        rowSum += rowPtr[c];
                    sums[static_cast<size_t>(ch)] += rowSum;
                }
                else
                {
                    for (int32_t c = 0; c < cols; ++c)
                        sums[static_cast<size_t>(c % channel_count)] += rowPtr[c];
                }
            }

            for (int32_t ch = 0; ch < channel_count; ++ch)
            {
                const double val =
                    static_cast<double>(sums[static_cast<size_t>(ch)] / static_cast<long double>(samplesPerChannel));
                channelSeries[static_cast<size_t>(ch)][static_cast<size_t>(f)] = val;
            }
        }

        for (int32_t ch = 0; ch < channel_count; ++ch)
        {
            const auto &series = channelSeries[static_cast<size_t>(ch)];
            if (out_channel_series)
            {
                std::copy(series.begin(), series.end(),
                          out_channel_series + int64_t(ch) * frame_count);
            }
            if (out_temporal_means || out_temporal_stds)
            {
                double sum = 0.0;
                for (double v : series)
                    sum += v;
                const double mean = sum / static_cast<double>(frame_count);
                if (out_temporal_means)
                    out_temporal_means[ch] = mean;
                if (out_temporal_stds)
                {
                    double sq = 0.0;
                    for (double v : series)
                    {
                        const double d = v - mean;
                        sq += d * d;
                    }
                    out_temporal_stds[ch] = std::sqrt(sq / static_cast<double>(frame_count));
                }
            }

            computeRfftCore(series.data(), frame_count, sample_rate_hz, window_kind, 1,
                            (ch == 0) ? out_freqs_hz : nullptr,
                            out_channel_amplitudes + int64_t(ch) * bins,
                            nullptr);
        }
    });
}

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

struct ComplexFft1dPlan
{
    size_t n{};
    bool powerOfTwo{false};
    size_t bluesteinSize{0};
    std::vector<Complex> chirp;
    std::vector<Complex> kernelFft;

    explicit ComplexFft1dPlan(size_t length)
        : n(length), powerOfTwo(isPowerOfTwo(length))
    {
        if (n <= 1 || powerOfTwo)
            return;
        bluesteinSize = nextPowerOfTwo(2 * n - 1);
        chirp.resize(n);
        kernelFft.assign(bluesteinSize, Complex(0.0, 0.0));
        const int64_t twoN = 2 * static_cast<int64_t>(n);
        for (size_t k = 0; k < n; ++k)
        {
            const int64_t kSqMod = (static_cast<int64_t>(k) * static_cast<int64_t>(k)) % twoN;
            const double angle = kPi * static_cast<double>(kSqMod) / static_cast<double>(n);
            chirp[k] = Complex(std::cos(angle), -std::sin(angle));
        }
        kernelFft[0] = Complex(1.0, 0.0);
        for (size_t k = 1; k < n; ++k)
        {
            const Complex conjChirp = std::conj(chirp[k]);
            kernelFft[k] = conjChirp;
            kernelFft[bluesteinSize - k] = conjChirp;
        }
        radix2Fft(kernelFft, false);
    }

    void transform(const Complex *src, Complex *dst) const
    {
        if (n == 0)
            return;
        if (n == 1)
        {
            dst[0] = src[0];
            return;
        }
        if (powerOfTwo)
        {
            std::vector<Complex> buf(src, src + n);
            radix2Fft(buf, false);
            std::copy(buf.begin(), buf.end(), dst);
            return;
        }
        std::vector<Complex> a(bluesteinSize, Complex(0.0, 0.0));
        for (size_t k = 0; k < n; ++k)
            a[k] = src[k] * chirp[k];
        radix2Fft(a, false);
        for (size_t i = 0; i < bluesteinSize; ++i)
            a[i] *= kernelFft[i];
        radix2Fft(a, true);
        for (size_t k = 0; k < n; ++k)
            dst[k] = a[k] * chirp[k];
    }
};

std::vector<double> buildWindow1d(int32_t length, int32_t windowKind, bool fallbackShortToUnit = false)
{
    std::vector<double> w(static_cast<size_t>(length), 1.0);
    // 2D FFT 短轴（length <= 2）保留等权采样；1D RFFT 对对称 Hann(length==2) 保持零权重校验。
    if (windowKind == IR_WINDOW_RECTANGULAR || length <= 1 || (fallbackShortToUnit && length <= 2))
        return w;
    for (int32_t i = 0; i < length; ++i)
    {
        if (windowKind == IR_WINDOW_HANN)
            w[static_cast<size_t>(i)] = 0.5 * (1.0 - std::cos(2.0 * kPi * i / static_cast<double>(length - 1)));
        else if (windowKind == IR_WINDOW_HAMMING)
            w[static_cast<size_t>(i)] = 0.54 - 0.46 * std::cos(2.0 * kPi * i / static_cast<double>(length - 1));
    }
    return w;
}

struct PreparedRfft1d {
    std::vector<double> win;
    double windowSum{0.0};
    double windowSqSum{0.0};
    ComplexFft1dPlan plan;

    PreparedRfft1d(int32_t length, int32_t windowKind)
        : win(buildWindow1d(length, windowKind)),
          plan(static_cast<size_t>(length))
    {
        for (double w : win) {
            windowSum += w;
            windowSqSum += w * w;
        }
        require(windowSum > 0.0 && windowSqSum > 0.0, IR_BAD_ARGUMENT,
                "Selected window yields zero weight for this signal length");
    }
};

void computeRfftWithPlan(const double *signal,
                         int32_t length,
                         double sampleRateHz,
                         int32_t detrendMean,
                         const PreparedRfft1d &prep,
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

    std::vector<Complex> windowed(static_cast<size_t>(length));
    for (int32_t i = 0; i < length; ++i)
        windowed[static_cast<size_t>(i)] = Complex((signal[i] - mean) * prep.win[static_cast<size_t>(i)], 0.0);

    std::vector<Complex> spectrum(static_cast<size_t>(length));
    prep.plan.transform(windowed.data(), spectrum.data());
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
        outAmplitude[bin] = mag * ampScale / prep.windowSum;
        if (outPsd)
            outPsd[bin] = (mag * mag) * ampScale / (sampleRateHz * prep.windowSqSum);
    }
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
    const PreparedRfft1d prep(length, windowKind);
    computeRfftWithPlan(signal, length, sampleRateHz, detrendMean, prep,
                        outFreqsHz, outAmplitude, outPsd);
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

        const PreparedRfft1d prep(frame_count, window_kind);
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

            computeRfftWithPlan(series.data(), frame_count, sample_rate_hz, 1, prep,
                                (ch == 0) ? out_freqs_hz : nullptr,
                                out_channel_amplitudes + int64_t(ch) * bins,
                                nullptr);
        }
    });
}

extern "C" IR_ANALYSIS_API int32_t IR_Fft2dMagnitudeF64(
    const double *in_map,
    const uint8_t *valid_mask,
    int32_t rows,
    int32_t cols,
    int64_t pixel_capacity,
    int32_t window_kind,
    int32_t detrend_mean,
    int32_t shift_dc_center,
    IRFft2dSummary *summary,
    double *out_magnitude_map,
    double *out_power_map,
    char *err,
    int32_t err_bytes)
{
    static_assert(sizeof(IRFft2dSummary) == 64, "Unexpected IRFft2dSummary size");
    if (summary)
        *summary = IRFft2dSummary{};
    return guarded(err, err_bytes, [&] {
        const int64_t total = pixels(rows, cols);
        needed(in_map, pixel_capacity, total);
        if (valid_mask)
            needed(valid_mask, pixel_capacity, total);
        require(summary != nullptr, IR_NULL_POINTER, "summary must not be null");
        require(window_kind == IR_WINDOW_RECTANGULAR || window_kind == IR_WINDOW_HANN,
                IR_BAD_ARGUMENT, "2D FFT window_kind supports IR_WINDOW_RECTANGULAR or IR_WINDOW_HANN");
        if (out_magnitude_map)
            needed(out_magnitude_map, pixel_capacity, total);
        if (out_power_map)
            needed(out_power_map, pixel_capacity, total);

        double sumValid = 0.0;
        int64_t validCount = 0;
        for (int64_t i = 0; i < total; ++i)
        {
            if (valid_mask && valid_mask[i] == 0)
                continue;
            const double v = in_map[i];
            if (!std::isfinite(v))
                continue;
            sumValid += v;
            ++validCount;
        }
        require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for 2D FFT");
        require(validCount <= INT32_MAX, IR_COMPUTATION_FAILED, "valid_pixel_count exceeds INT32_MAX");
        const double validMean = sumValid / static_cast<double>(validCount);

        const auto winRow = buildWindow1d(rows, window_kind, true);
        const auto winCol = buildWindow1d(cols, window_kind, true);
        double rowWinSum = 0.0;
        double colWinSum = 0.0;
        for (double w : winRow)
            rowWinSum += w;
        for (double w : winCol)
            colWinSum += w;
        const double windowNorm = rowWinSum * colWinSum;
        require(windowNorm > 0.0, IR_BAD_ARGUMENT,
                "Selected 2D FFT window yields zero weight for these image dimensions");

        const ComplexFft1dPlan rowPlan(static_cast<size_t>(cols));
        const ComplexFft1dPlan colPlan(static_cast<size_t>(rows));

        std::vector<Complex> work(static_cast<size_t>(total));
        std::vector<Complex> rowIn(static_cast<size_t>(cols));
        std::vector<Complex> rowOut(static_cast<size_t>(cols));
        for (int32_t r = 0; r < rows; ++r)
        {
            const int64_t rowBase = int64_t(r) * cols;
            const double wr = winRow[static_cast<size_t>(r)];
            for (int32_t c = 0; c < cols; ++c)
            {
                const int64_t idx = rowBase + c;
                const bool ok = (!valid_mask || valid_mask[idx] != 0) && std::isfinite(in_map[idx]);
                const double baseVal = ok ? in_map[idx] : validMean;
                const double centered = (detrend_mean != 0) ? (baseVal - validMean) : baseVal;
                rowIn[static_cast<size_t>(c)] = Complex(centered * wr * winCol[static_cast<size_t>(c)], 0.0);
            }
            rowPlan.transform(rowIn.data(), rowOut.data());
            std::copy(rowOut.begin(), rowOut.end(), work.begin() + rowBase);
        }

        std::vector<Complex> colIn(static_cast<size_t>(rows));
        std::vector<Complex> colOut(static_cast<size_t>(rows));
        for (int32_t c = 0; c < cols; ++c)
        {
            for (int32_t r = 0; r < rows; ++r)
                colIn[static_cast<size_t>(r)] = work[static_cast<size_t>(int64_t(r) * cols + c)];
            colPlan.transform(colIn.data(), colOut.data());
            for (int32_t r = 0; r < rows; ++r)
                work[static_cast<size_t>(int64_t(r) * cols + c)] = colOut[static_cast<size_t>(r)];
        }

        const double invNorm = 1.0 / windowNorm;
        const int32_t shiftR = (shift_dc_center != 0) ? (rows / 2) : 0;
        const int32_t shiftC = (shift_dc_center != 0) ? (cols / 2) : 0;

        double dcMag = 0.0;
        double peakAcMag = 0.0;
        int32_t peakAcRow = 0;
        int32_t peakAcCol = 0;
        double totalAcPower = 0.0;
        double lowFreqPower = 0.0;
        double highFreqPower = 0.0;

        for (int32_t u = 0; u < rows; ++u)
        {
            const int32_t outR = (u + shiftR) % rows;
            const double ru = static_cast<double>(std::min(u, rows - u)) / static_cast<double>(rows);
            for (int32_t v = 0; v < cols; ++v)
            {
                const int32_t outC = (v + shiftC) % cols;
                const Complex &z = work[static_cast<size_t>(int64_t(u) * cols + v)];
                const double mag = std::hypot(z.real(), z.imag()) * invNorm;
                const double pwr = mag * mag;

                if (out_magnitude_map)
                    out_magnitude_map[static_cast<size_t>(int64_t(outR) * cols + outC)] = mag;
                if (out_power_map)
                    out_power_map[static_cast<size_t>(int64_t(outR) * cols + outC)] = pwr;

                if (u == 0 && v == 0)
                {
                    dcMag = mag;
                }
                else
                {
                    if (mag > peakAcMag)
                    {
                        peakAcMag = mag;
                        peakAcRow = outR;
                        peakAcCol = outC;
                    }
                    totalAcPower += pwr;
                    const double rv = static_cast<double>(std::min(v, cols - v)) / static_cast<double>(cols);
                    const double rad = std::hypot(ru, rv);
                    if (rad <= 0.25)
                        lowFreqPower += pwr;
                    else
                        highFreqPower += pwr;
                }
            }
        }

        IRFft2dSummary out{};
        out.dc_magnitude = dcMag;
        out.peak_ac_magnitude = peakAcMag;
        out.total_ac_power = totalAcPower;
        if (totalAcPower > 0.0)
        {
            out.low_freq_power_ratio = lowFreqPower / totalAcPower;
            out.high_freq_power_ratio = highFreqPower / totalAcPower;
        }
        out.peak_ac_row = peakAcRow;
        out.peak_ac_col = peakAcCol;
        out.rows = rows;
        out.cols = cols;
        out.valid_pixel_count = static_cast<int32_t>(validCount);
        out.reserved = 0;
        *summary = out;
    });
}

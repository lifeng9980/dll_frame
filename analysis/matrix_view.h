#pragma once
#include "api_common.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ir::analysis
{

struct Rect2D
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] bool contains(int32_t px, int32_t py) const noexcept
    {
        return px >= x && px < (x + width) && py >= y && py < (y + height);
    }
};

template <typename T>
struct ConstMatrixView
{
    const T *ptr = nullptr;
    int32_t rows = 0;
    int32_t cols = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return ptr == nullptr || rows <= 0 || cols <= 0;
    }

    [[nodiscard]] int64_t size() const noexcept
    {
        return int64_t(rows) * int64_t(cols);
    }

    [[nodiscard]] const T *rowPtr(int32_t row) const noexcept
    {
        return ptr + int64_t(row) * cols;
    }

    [[nodiscard]] T at(int32_t row, int32_t col) const noexcept
    {
        return ptr[int64_t(row) * cols + col];
    }
};

template <typename T>
struct Matrix2D
{
    int32_t rows = 0;
    int32_t cols = 0;
    std::vector<T> data;

    Matrix2D() = default;

    Matrix2D(int32_t r, int32_t c, T init = T{})
        : rows(r), cols(c), data(size_t(int64_t(r) * int64_t(c)), init)
    {
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return rows <= 0 || cols <= 0 || data.empty();
    }

    [[nodiscard]] int64_t size() const noexcept
    {
        return int64_t(rows) * int64_t(cols);
    }

    [[nodiscard]] T *rowPtr(int32_t row) noexcept
    {
        return data.data() + int64_t(row) * cols;
    }

    [[nodiscard]] const T *rowPtr(int32_t row) const noexcept
    {
        return data.data() + int64_t(row) * cols;
    }

    [[nodiscard]] T &at(int32_t row, int32_t col) noexcept
    {
        return data[size_t(int64_t(row) * cols + col)];
    }

    [[nodiscard]] const T &at(int32_t row, int32_t col) const noexcept
    {
        return data[size_t(int64_t(row) * cols + col)];
    }

    [[nodiscard]] ConstMatrixView<T> view() const noexcept
    {
        return ConstMatrixView<T>{data.data(), rows, cols};
    }

    void fill(T value)
    {
        std::fill(data.begin(), data.end(), value);
    }
};

// Keep OpenCV cv::divide semantics: division by exact 0.0 yields 0.0 instead of Inf/NaN.
[[nodiscard]] inline double safeDivide(double num, double den) noexcept
{
    return (den == 0.0) ? 0.0 : (num / den);
}

[[nodiscard]] inline double arithmeticMean(ConstMatrixView<double> matrix)
{
    ir::api::require(!matrix.empty(), IR_BAD_ARGUMENT, "Input matrix must not be empty");
    double sum = 0.0;
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        sum += matrix.ptr[i];
    }
    return sum / static_cast<double>(total);
}

[[nodiscard]] inline double finiteMean(ConstMatrixView<double> matrix)
{
    ir::api::require(!matrix.empty(), IR_BAD_ARGUMENT, "Input matrix must not be empty");
    double sum = 0.0;
    int64_t validCount = 0;
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        const double value = matrix.ptr[i];
        if (!std::isfinite(value))
            continue;
        sum += value;
        ++validCount;
    }
    ir::api::require(validCount > 0, IR_COMPUTATION_FAILED, "Matrix contains no finite values");
    return sum / static_cast<double>(validCount);
}

[[nodiscard]] inline double maskedFiniteMean(ConstMatrixView<double> matrix,
                                             ConstMatrixView<uint8_t> validMask)
{
    ir::api::require(!matrix.empty() && matrix.rows == validMask.rows && matrix.cols == validMask.cols,
                     IR_BAD_ARGUMENT, "Matrix and mask dimensions must match");
    double sum = 0.0;
    int64_t validCount = 0;
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask.ptr[i] == 0)
            continue;
        const double value = matrix.ptr[i];
        if (!std::isfinite(value))
            continue;
        sum += value;
        ++validCount;
    }
    ir::api::require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for mean calculation");
    return sum / static_cast<double>(validCount);
}

[[nodiscard]] inline double maskedFiniteStdDev(ConstMatrixView<double> matrix,
                                               ConstMatrixView<uint8_t> validMask,
                                               double mean)
{
    ir::api::require(!matrix.empty() && matrix.rows == validMask.rows && matrix.cols == validMask.cols,
                     IR_BAD_ARGUMENT, "Matrix and mask dimensions must match");
    double squaredDiffSum = 0.0;
    int64_t validCount = 0;
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        if (validMask.ptr[i] == 0)
            continue;
        const double value = matrix.ptr[i];
        if (!std::isfinite(value))
            continue;
        const double diff = value - mean;
        squaredDiffSum += diff * diff;
        ++validCount;
    }
    ir::api::require(validCount > 0, IR_COMPUTATION_FAILED, "No valid finite pixels for std calculation");
    return std::sqrt(squaredDiffSum / static_cast<double>(validCount));
}

[[nodiscard]] inline Matrix2D<uint8_t> createRangePassMask(ConstMatrixView<double> matrix,
                                                           double lowerLimit,
                                                           double upperLimit)
{
    Matrix2D<uint8_t> passMask(matrix.rows, matrix.cols, 0);
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        const double value = matrix.ptr[i];
        if (!std::isfinite(value))
            continue;
        if (value >= lowerLimit && value <= upperLimit)
            passMask.data[size_t(i)] = 1;
    }
    return passMask;
}

[[nodiscard]] inline Matrix2D<uint8_t> createUpperPassMask(ConstMatrixView<double> matrix,
                                                           double upperLimit)
{
    Matrix2D<uint8_t> passMask(matrix.rows, matrix.cols, 0);
    const int64_t total = matrix.size();
    for (int64_t i = 0; i < total; ++i)
    {
        const double value = matrix.ptr[i];
        if (!std::isfinite(value))
            continue;
        if (value <= upperLimit)
            passMask.data[size_t(i)] = 1;
    }
    return passMask;
}

[[nodiscard]] inline int32_t countNonZero(ConstMatrixView<uint8_t> mask)
{
    int32_t count = 0;
    const int64_t total = mask.size();
    for (int64_t i = 0; i < total; ++i)
    {
        if (mask.ptr[i] != 0)
            ++count;
    }
    return count;
}

} // namespace ir::analysis

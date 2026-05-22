/*
 *  Copyright (C) 2010-2021 Hendrik Leppkes (original LAV Filters implementation)
 *  Copyright (C) 2025 Team Kodi (Kodi adaptation)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  This is a Kodi adaptation of the FloatingAverage class from LAV Filters,
 *  used for audio jitter tracking and A/V sync correction in passthrough mode.
 *  Original source: LAVFilters/common/DSUtilLite/FloatingAverage.h
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace AudioSync
{

template <typename T, size_t N>
class CFloatingAverage
{
public:
  CFloatingAverage() { m_samples.fill(T{0}); }

  void Sample(T sample)
  {
    m_samples[m_currentSample] = sample;
    if (++m_currentSample >= N)
      m_currentSample = 0;
    if (m_sampleCount < N)
      m_sampleCount++;
  }

  T Average() const
  {
    if (m_sampleCount == 0)
      return T{0};
    T sum{0};
    for (size_t i = 0; i < m_sampleCount; ++i)
      sum += m_samples[i];
    return sum / static_cast<T>(m_sampleCount);
  }

  T Minimum() const
  {
    if (m_sampleCount == 0)
      return T{0};
    T minVal = m_samples[0];
    for (size_t i = 1; i < m_sampleCount; ++i)
    {
      if (m_samples[i] < minVal)
        minVal = m_samples[i];
    }
    return minVal;
  }

  T AbsMinimum() const
  {
    if (m_sampleCount == 0)
      return T{0};
    T minVal = m_samples[0];
    for (size_t i = 1; i < m_sampleCount; ++i)
    {
      if (std::abs(m_samples[i]) < std::abs(minVal))
        minVal = m_samples[i];
    }
    return minVal;
  }

  T Maximum() const
  {
    if (m_sampleCount == 0)
      return T{0};
    T maxVal = m_samples[0];
    for (size_t i = 1; i < m_sampleCount; ++i)
    {
      if (m_samples[i] > maxVal)
        maxVal = m_samples[i];
    }
    return maxVal;
  }

  T AbsMaximum() const
  {
    if (m_sampleCount == 0)
      return T{0};
    T maxVal = m_samples[0];
    for (size_t i = 1; i < m_sampleCount; ++i)
    {
      if (std::abs(m_samples[i]) > std::abs(maxVal))
        maxVal = m_samples[i];
    }
    return maxVal;
  }

  void OffsetValues(T value)
  {
    for (size_t i = 0; i < N; ++i)
      m_samples[i] += value;
  }

  void Reset()
  {
    m_samples.fill(T{0});
    m_currentSample = 0;
    m_sampleCount = 0;
  }

  size_t CurrentSample() const { return m_currentSample; }
  size_t SampleCount() const { return m_sampleCount; }
  bool IsFull() const { return m_sampleCount >= N; }

private:
  std::array<T, N> m_samples;
  size_t m_currentSample{0};
  size_t m_sampleCount{0};
};

} // namespace AudioSync
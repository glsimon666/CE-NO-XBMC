/*
 *  Copyright (C) 2025 Team Kodi
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  AdaptiveJitterFilter.h - Second-order EWMA jitter tracker with drift
 *
 *  Replaces CFloatingAverage<T,N> with a statistically superior model:
 *
 *    Problem with AbsMinimum() in a sliding window:
 *      - Needs full 256-sample window to converge (hot restart is slow)
 *      - A single outlier can become the window's minimum, spoiling the baseline
 *      - Cannot distinguish a constant offset from a growing drift
 *      - After a discontinuity, old stale samples pollute the baseline
 *
 *    AdaptiveJitterFilter advantages:
 *      - EWMA + drift rate (first derivative) — tracks clock skew, not just jitter
 *      - Adaptive alpha — fast tracking when noisy, smooth when stable
 *      - Predict(N) — forecasts future jitter, enabling pre-emptive correction
 *      - Confidence() — tells caller whether to trust the current estimate
 *      - O(1) per sample, fixed tiny state (no window allocation)
 *
 *  Usage:
 *    AdaptiveJitterFilter filter;
 *    filter.Sample(jitter_seconds, dt_seconds);
 *    double predicted = filter.Predict(horizon_seconds);
 *    if (filter.Confidence() > 0.6) { act on prediction; }
 */

#pragma once

#include <algorithm>
#include <cmath>

namespace AudioSync
{

class CAdaptiveJitterFilter
{
public:
  CAdaptiveJitterFilter() { Reset(); }

  /*
   * Feed a new jitter observation.
   *   jitter: measured jitter value (in any unit — seconds, 90k ticks, ...)
   *   dt:     time elapsed since the previous sample (same time unit as alpha expects)
   */
  void Sample(double jitter, double dt)
  {
    const double delta = jitter - m_mean;
    const double alpha = CalcAlpha();

    m_mean += alpha * delta;
    m_variance += alpha * (delta * delta - m_variance);

    if (dt > 1e-9)
    {
      const double instantDrift = delta / dt;
      m_drift += alpha * (instantDrift - m_drift);
    }

    m_converged = m_sampleCount++ > CONVERGENCE_WINDOW;
  }

  /*
   * Predict the jitter value `horizon` time units into the future.
   * Uses: prediction = mean + drift * horizon
   */
  double Predict(double horizon) const
  {
    if (!m_converged)
      return 0.0;
    return m_mean + m_drift * horizon;
  }

  /*
   * Returns 0.0 - 1.0 indicating how much to trust the current estimate.
   * High variance or pre-convergence state → low confidence.
   */
  double Confidence() const
  {
    if (!m_converged)
      return 0.0;

    const double noiseRatio = std::max(0.0, m_variance) / NOISE_SCALE;
    return 1.0 / (1.0 + noiseRatio);
  }

  /*
   * Mean jitter estimate (central tendency of the distribution).
   */
  double Mean() const { return m_mean; }

  /*
   * Drift rate: how fast the jitter is growing or shrinking per time unit.
   * Positive = jitter is increasing over time (clock drift).
   */
  double Drift() const { return m_drift; }

  /*
   * Reset all state. Equivalent to constructor.
   */
  void Reset()
  {
    m_mean = 0.0;
    m_variance = 0.0;
    m_drift = 0.0;
    m_sampleCount = 0;
    m_converged = false;
  }

  /*
   * Whether enough samples have been collected for reliable estimates.
   */
  bool IsConverged() const { return m_converged; }

  unsigned int SampleCount() const { return m_sampleCount; }

  /*
   * Explicitly mark as converged. Use this to skip the warm-up period
   * when there is a known-good initial estimate.
   */
  void ForceConverged()
  {
    m_converged = true;
    m_sampleCount = CONVERGENCE_WINDOW;
  }

private:
  static constexpr double BASE_ALPHA    = 0.04;   /* slow, smooth tracking  */
  static constexpr double ADAPT_ALPHA   = 0.35;   /* fast, agile tracking   */
  static constexpr double NOISE_SCALE   = 1.0e-4; /* normalised variance    */
  static constexpr unsigned int CONVERGENCE_WINDOW = 8;

  double CalcAlpha() const
  {
    /*
     * Adaptive weight:
     *   low noise  → low alpha  (smooth, confident)
     *   high noise → high alpha (agile, quick to track changes)
     */
    const double safeVariance = std::max(0.0, m_variance);
    const double noiseRatio = std::min(safeVariance / NOISE_SCALE, 1.0);
    return BASE_ALPHA + (ADAPT_ALPHA - BASE_ALPHA) * noiseRatio;
  }

  double m_mean{0.0};
  double m_variance{0.0};
  double m_drift{0.0};

  unsigned int m_sampleCount{0};
  bool m_converged{false};
};

} // namespace AudioSync

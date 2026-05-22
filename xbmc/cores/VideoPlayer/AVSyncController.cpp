/*
 *  Copyright (C) 2025 Team Kodi
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  AVSyncController.cpp - Implementation of the unified sync controller.
 *
 *  Talks to /dev/avsync_v2 kernel driver for single-authority A/V sync.
 */

#include "AVSyncController.h"

#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include "uapi/amlogic/avsync_v2.h"
}

using namespace std::chrono;

namespace
{

constexpr const char* DEV_PATH = "/dev/avsync_v2";

uint64_t MonotonicNs()
{
  auto now = steady_clock::now().time_since_epoch();
  return duration_cast<nanoseconds>(now).count();
}

} // anonymous namespace

CAVSyncController::CAVSyncController() = default;

CAVSyncController::~CAVSyncController()
{
  Close();
}

bool CAVSyncController::Open(AVSyncPlaybackMode mode,
                              double videoFps,
                              uint32_t audioSampleRate,
                              uint32_t audioFrameDuration90k)
{
  if (m_fd >= 0)
    Close();

  m_fd = open(DEV_PATH, O_RDWR);
  if (m_fd < 0)
  {
    CLog::Log(LOGWARNING,
              "CAVSyncController::Open - cannot open {}, errno={}",
              DEV_PATH, errno);
    return false;
  }

  m_mode = mode;

  struct avs_session_cfg cfg {};
  if (mode == AVS_MODE_PASSTHROUGH)
    cfg.flags |= AVS_CFG_PASSTHROUGH;
  if (mode == AVS_MODE_LIVE)
    cfg.flags |= AVS_CFG_LIVE;

  if (videoFps > 0.0)
  {
    double denom = 1001.0;
    double numer = videoFps * denom;
    cfg.video_frame_rate_num = static_cast<uint32_t>(numer);
    cfg.video_frame_rate_den = static_cast<uint32_t>(denom);
  }
  else
  {
    cfg.video_frame_rate_num = 24000;
    cfg.video_frame_rate_den = 1001;
  }

  cfg.audio_sample_rate = audioSampleRate;
  cfg.audio_frame_duration_90k = audioFrameDuration90k;
  cfg.start_buf_thres_90k = -1;

  if (ioctl(m_fd, AVSYNC_V2_IOC_CREATE, &cfg) < 0)
  {
    CLog::Log(LOGERROR,
              "CAVSyncController::Open - AVSYNC_V2_IOC_CREATE failed, errno={}",
              errno);
    close(m_fd);
    m_fd = -1;
    return false;
  }

  m_sessionId = cfg.reserved[0];

  m_videoJitter.Reset();
  m_audioJitter.Reset();
  m_recentVptsDeltas.clear();
  m_contentType = AVS_CT_UNKNOWN;
  m_lastVideoUpdateNs = 0;
  m_lastAudioUpdateNs = 0;

  CLog::Log(LOGINFO,
            "CAVSyncController::Open - session {} created, mode={}",
            m_sessionId, static_cast<int>(mode));
  return true;
}

void CAVSyncController::Close()
{
  if (m_fd < 0)
    return;

  if (m_sessionId > 0)
  {
    ioctl(m_fd, AVSYNC_V2_IOC_DESTROY);
    m_sessionId = 0;
  }

  close(m_fd);
  m_fd = -1;

  CLog::Log(LOGINFO, "CAVSyncController::Close");
}

void CAVSyncController::PushVideoPts(double pts90k, double delay90k,
                                      bool discontinuity, bool isIdr,
                                      bool isBFrame)
{
  if (m_fd < 0)
    return;

  struct avs_pts_sample s {};
  s.pts_90k = static_cast<uint64_t>(pts90k);
  s.delay_90k = static_cast<uint64_t>(delay90k);
  s.mono_ns = MonotonicNs();
  if (discontinuity) s.flags |= AVS_FLAG_DISCONTINUITY;
  if (isIdr)         s.flags |= AVS_FLAG_IDR;
  if (isBFrame)      s.flags |= AVS_FLAG_BFRAME;

  ioctl(m_fd, AVSYNC_V2_IOC_PUSH_VPTS, &s);

  /* VFR detection: track inter-frame PTS deltas */
  if (!discontinuity)
  {
    m_recentVptsDeltas.push_back(pts90k);
    while (m_recentVptsDeltas.size() > CADENCE_WINDOW)
      m_recentVptsDeltas.erase(m_recentVptsDeltas.begin());
  }

  const uint64_t nowNs = MonotonicNs();
  if (m_lastVideoUpdateNs > 0)
  {
    const double dtSec =
        static_cast<double>(nowNs - m_lastVideoUpdateNs) * 1e-9;
    m_videoJitter.Sample(pts90k, dtSec);
  }
  m_lastVideoUpdateNs = nowNs;
}

void CAVSyncController::PushAudioPts(double pts90k, double delay90k,
                                      bool discontinuity)
{
  if (m_fd < 0)
    return;

  struct avs_pts_sample s {};
  s.pts_90k = static_cast<uint64_t>(pts90k);
  s.delay_90k = static_cast<uint64_t>(delay90k);
  s.mono_ns = MonotonicNs();
  if (discontinuity) s.flags |= AVS_FLAG_DISCONTINUITY;

  ioctl(m_fd, AVSYNC_V2_IOC_PUSH_APTS, &s);

  const uint64_t nowNs = MonotonicNs();
  if (m_lastAudioUpdateNs > 0)
  {
    const double dtSec =
        static_cast<double>(nowNs - m_lastAudioUpdateNs) * 1e-9;
    m_audioJitter.Sample(pts90k, dtSec);
  }
  m_lastAudioUpdateNs = nowNs;
}

int CAVSyncController::SendIoctlSyncGet(struct avs_sync *info)
{
  return ioctl(m_fd, AVSYNC_V2_IOC_GET_SYNC, info);
}

AVSyncDecision CAVSyncController::QuerySync()
{
  AVSyncDecision decision;

  if (m_fd < 0)
    return decision;

  struct avs_sync info {};
  if (SendIoctlSyncGet(&info) < 0)
    return decision;

  /* Convert kernel enum to our action */
  switch (info.action)
  {
  case AVS_ACT_DROP_VIDEO:
    decision.action = AVSyncDecision::DROP_VIDEO;
    break;
  case AVS_ACT_REPEAT_VIDEO:
    decision.action = AVSyncDecision::REPEAT_VIDEO;
    break;
  case AVS_ACT_ADJUST_TIMING:
    decision.action = AVSyncDecision::TIMING_ADJUST;
    break;
  case AVS_ACT_INSERT_REPEAT:
    decision.action = AVSyncDecision::FRACTIONAL_REPEAT;
    break;
  case AVS_ACT_HOLD_FRAME:
    decision.action = AVSyncDecision::HOLD_FRAME;
    break;
  case AVS_ACT_NONE:
  default:
    decision.action = AVSyncDecision::NONE;
    break;
  }

  decision.frameCount     = static_cast<int>(info.count);
  decision.timingAdjustNs = info.timing_adjust_ns;
  decision.confidence     = info.confidence / 1000.0;

  /*
   * Userspace jitter filter cross-validation:
   * if the kernel's confidence is high but our own EWMA says variance
   * is still settling, override to NONE.
   */
  if (decision.action != AVSyncDecision::NONE &&
      m_audioJitter.Confidence() < m_confidenceThreshold)
  {
    decision.action     = AVSyncDecision::NONE;
    decision.frameCount = 0;
    decision.timingAdjustNs = 0;
    decision.confidence = m_audioJitter.Confidence();
  }

  /*
   * Passthrough guard: if we are in passthrough mode and the kernel
   * suggests dropping multiple frames, limit consecutive drops.
   */
  if (m_mode == AVS_MODE_PASSTHROUGH &&
      decision.action == AVSyncDecision::DROP_VIDEO &&
      decision.frameCount > 1)
  {
    CLog::Log(LOGDEBUG,
              "CAVSyncController::QuerySync - clamping passthrough drop "
              "from {} to 1 frame", decision.frameCount);
    decision.frameCount = 1;
  }

  return decision;
}

void CAVSyncController::SetAudioLatencyNs(uint64_t latencyNs)
{
  if (m_fd < 0)
    return;

  if (ioctl(m_fd, AVSYNC_V2_IOC_SET_AUDIO_LATENCY, &latencyNs) < 0)
  {
    CLog::Log(LOGWARNING,
              "CAVSyncController::SetAudioLatencyNs - ioctl failed");
  }
}

void CAVSyncController::SendIoctlEvent(uint32_t type, uint32_t value)
{
  if (m_fd < 0)
    return;

  struct avs_event ev {};
  ev.type = type;
  ev.value = value;
  ev.mono_ns = MonotonicNs();
  ioctl(m_fd, AVSYNC_V2_IOC_SEND_EVENT, &ev);
}

void CAVSyncController::NotifyPause()
{
  SendIoctlEvent(AVS_EVENT_VIDEO_PAUSE);
  SendIoctlEvent(AVS_EVENT_AUDIO_PAUSE);
}

void CAVSyncController::NotifyResume()
{
  SendIoctlEvent(AVS_EVENT_VIDEO_RESUME);
  SendIoctlEvent(AVS_EVENT_AUDIO_RESUME);
}

void CAVSyncController::NotifyFlush()
{
  SendIoctlEvent(AVS_EVENT_FLUSH);
  m_videoJitter.Reset();
  m_audioJitter.Reset();
  m_recentVptsDeltas.clear();
  m_contentType = AVS_CT_UNKNOWN;
}

void CAVSyncController::NotifyDiscontinuity()
{
  SendIoctlEvent(AVS_EVENT_DISCONTINUITY);
}

void CAVSyncController::NotifyAudioFormatChange()
{
  SendIoctlEvent(AVS_EVENT_AUDIO_FORMAT_CHANGE);

  /*
   * Audio format change resets the jitter baseline because the
   * ALSA ring buffer and HDMI TX may have a different pipeline
   * delay for the new format (e.g. switching from 48kHz PCM to
   * 192kHz TrueHD MAT).
   */
  m_audioJitter.Reset();
}

void CAVSyncController::DetectContentType()
{
  if (m_recentVptsDeltas.size() < 3)
  {
    m_contentType = AVS_CT_UNKNOWN;
    return;
  }

  double meanDelta = 0.0;
  double varDelta = 0.0;
  const size_t n = m_recentVptsDeltas.size();

  for (size_t i = 1; i < n; ++i)
  {
    const double d = m_recentVptsDeltas[i] - m_recentVptsDeltas[i - 1];
    meanDelta += d;
    varDelta += d * d;
  }
  meanDelta /= (n - 1);
  varDelta = varDelta / (n - 1) - meanDelta * meanDelta;

  /*
   * Coefficient of variation: σ / μ
   * Film:    very low CV (< 0.001) because all frame intervals are
   *          strictly equal (23.976 or 24 fps).
   * Video:   moderate CV (< 0.05) because of occasional repeats
   *          in 59.94 content.
   * Mixed:   high CV because of VFR / mixed cadence.
   */
  const double cv = std::sqrt(std::max(0.0, varDelta)) /
                    (std::abs(meanDelta) + 1e-9);

  if (cv < 0.001)
    m_contentType = AVS_CT_FILM;
  else if (cv < 0.05)
    m_contentType = AVS_CT_VIDEO;
  else
    m_contentType = AVS_CT_MIXED;
}

CAVSyncController::Diag CAVSyncController::GetDiagnostics()
{
  Diag diag {};

  if (m_fd < 0)
    return diag;

  struct avs_diag kdiag {};
  if (ioctl(m_fd, AVSYNC_V2_IOC_GET_DIAG, &kdiag) < 0)
    return diag;

  diag.totalVideoFrames       = kdiag.total_video_frames;
  diag.totalAudioFrames       = kdiag.total_audio_frames;
  diag.totalDrops             = kdiag.total_drops;
  diag.totalRepeats           = kdiag.total_repeats;
  diag.totalTimingAdjusts     = kdiag.total_timing_adjusts;
  diag.totalDiscontinuities   = kdiag.total_discontinuities;
  diag.cumulativeAVError90k  = kdiag.cumulative_av_error_90k;
  diag.sessionUptimeNs        = kdiag.session_uptime_ns;

  return diag;
}

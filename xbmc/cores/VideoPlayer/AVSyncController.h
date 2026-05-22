/*
 *  Copyright (C) 2025 Team Kodi
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  AVSyncController.h - Single sync authority for A/V playback
 *
 *  Communicates with aml_avsync_v2.ko kernel driver to unify all
 *  audio/video sync decisions through one coherent controller.
 *  Replaces the fragmented sync state machines that previously
 *  lived in VideoPlayer.cpp, AudioSinkAE.cpp and the kernel.
 */

#pragma once

#include "cores/VideoPlayer/DVDCodecs/Audio/AdaptiveJitterFilter.h"
#include "cores/VideoPlayer/DVDClock.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"

#include <memory>
#include <string>
#include <vector>

struct avs_pts_sample;
struct avs_sync;
struct avs_session_cfg;
struct avs_event;
struct avs_diag;

enum AVSyncContentType
{
  AVS_CT_FILM,      /* strict cadence (23.976/24) fs */
  AVS_CT_VIDEO,     /* normal (50/59.94) fs           */
  AVS_CT_MIXED,     /* VFR / mixed cadence            */
  AVS_CT_UNKNOWN,
};

struct AVSyncDecision
{
  enum Action
  {
    NONE,
    DROP_VIDEO,
    REPEAT_VIDEO,
    TIMING_ADJUST,
    FRACTIONAL_REPEAT,
    HOLD_FRAME,
  };

  Action action{NONE};
  int frameCount{0};
  int64_t timingAdjustNs{0};
  double confidence{0.0};
};

enum AVSyncPlaybackMode
{
  AVS_MODE_PASSTHROUGH,   /* compressed audio: can only adjust video  */
  AVS_MODE_PCM,           /* decoded audio: can resample               */
  AVS_MODE_LIVE,          /* live IPTV: asymmetric sync strategy       */
};

class CAVSyncController
{
public:
  CAVSyncController();
  ~CAVSyncController();

  /* ---- lifecycle ---- */
  bool  Open(AVSyncPlaybackMode mode,
             double videoFps,
             uint32_t audioSampleRate,
             uint32_t audioFrameDuration90k);
  void  Close();
  bool  IsOpen() const { return m_fd >= 0; }

  /* ---- PTS injection (called from decoder thread) ---- */
  void  PushVideoPts(double pts90k, double delay90k,
                     bool discontinuity, bool isIdr, bool isBFrame);
  void  PushAudioPts(double pts90k, double delay90k,
                     bool discontinuity);

  /* ---- sync query (called every vsync from render thread) ---- */
  AVSyncDecision QuerySync();

  /* ---- audio latency management ---- */
  void  SetAudioLatencyNs(uint64_t latencyNs);

  /* ---- event notification ---- */
  void  NotifyPause();
  void  NotifyResume();
  void  NotifyFlush();
  void  NotifyDiscontinuity();
  void  NotifyAudioFormatChange();

  /* ---- content-aware ---- */
  void  DetectContentType();
  AVSyncContentType GetContentType() const { return m_contentType; }

  /* ---- diagnostics ---- */
  struct Diag
  {
    uint64_t totalVideoFrames;
    uint64_t totalAudioFrames;
    uint64_t totalDrops;
    uint64_t totalRepeats;
    uint64_t totalTimingAdjusts;
    uint64_t totalDiscontinuities;
    int64_t  cumulativeAVError90k;
    uint64_t sessionUptimeNs;
  };
  Diag  GetDiagnostics();

  /* ---- tuning ---- */
  void  SetConfidenceThreshold(double t) { m_confidenceThreshold = t; }
  double ConfidenceThreshold() const { return m_confidenceThreshold; }

private:
  int  m_fd{-1};

  AVSyncPlaybackMode   m_mode{AVS_MODE_PASSTHROUGH};
  AVSyncContentType    m_contentType{AVS_CT_UNKNOWN};
  double               m_confidenceThreshold{0.6};
  uint32_t             m_sessionId{0};

  AudioSync::CAdaptiveJitterFilter m_videoJitter;
  AudioSync::CAdaptiveJitterFilter m_audioJitter;

  uint64_t m_lastVideoUpdateNs{0};
  uint64_t m_lastAudioUpdateNs{0};

  /* VFR detection helpers */
  std::vector<double> m_recentVptsDeltas;
  static constexpr size_t CADENCE_WINDOW = 30;

  /* kernel call helpers */
  int  SendIoctlSyncGet(struct avs_sync *info);
  void SendIoctlEvent(uint32_t type, uint32_t value = 0);
};

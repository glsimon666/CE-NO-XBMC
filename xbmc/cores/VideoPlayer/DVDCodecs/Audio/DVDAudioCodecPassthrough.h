/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  LAV A/V sync improvements based on LAV Filters by Hendrik Leppkes (Nevcairiel)
 *  https://github.com/Nevcairiel/LAVFilters
 *  (enabled via m_lavStyleSyncEnabled flag)
 *
 *  v2: Replaced CFloatingAverage<T,N> with CAdaptiveJitterFilter (EWMA+drift)
 *      Better convergence, drift compensation, and confidence gating.
 */

#pragma once

#include "DVDAudioCodec.h"
#include "AdaptiveJitterFilter.h"
#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Utils/AEBitstreamPacker.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"

#include <atomic>
#include <list>
#include <memory>
#include <vector>

class CProcessInfo;
class CPackerMAT;

class CDVDAudioCodecPassthrough : public CDVDAudioCodec
{
public:
  CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType);
  ~CDVDAudioCodecPassthrough() override;

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  void Dispose() override;
  bool AddData(const DemuxPacket &packet) override;
  void GetData(DVDAudioFrame &frame) override;
  void Reset() override;
  AEAudioFormat GetFormat() override { return m_format; }
  bool NeedPassthrough() override { return true; }
  std::string GetName() override { return m_codecName; }
  int GetBufferSize() override;

  void SetLavStyleSyncEnabled(bool enabled);
  void SetLavSeamlessBranchEnabled(bool enabled);
  bool IsLavStyleSyncEnabled() const { return m_lavStyleSyncEnabled; }
  bool IsLavSeamlessBranchEnabled() const { return m_lavSeamlessBranchEnabled; }

  void ResetLavSyncState();
  void SyncToResyncPts(double pts);

private:
  int GetData(uint8_t** dst);
  unsigned int PackTrueHD();
  CAEStreamParser m_parser;
  uint8_t* m_buffer = nullptr;
  unsigned int m_bufferSize = 0;
  unsigned int m_dataSize = 0;
  AEAudioFormat m_format;
  uint8_t *m_backlogBuffer = nullptr;
  unsigned int m_backlogBufferSize = 0;
  unsigned int m_backlogSize = 0;
  double m_currentPts = DVD_NOPTS_VALUE;
  double m_nextPts = DVD_NOPTS_VALUE;
  std::string m_codecName;

  std::unique_ptr<CPackerMAT> m_packerMAT;
  std::vector<uint8_t> m_trueHDBuffer;
  unsigned int m_trueHDoffset = 0;
  unsigned int m_trueHDframes = 0;
  bool m_deviceIsRAW{false};

  bool m_lavStyleSyncEnabled{false};
  bool m_lavSeamlessBranchEnabled{false};

  static constexpr double LOCAL_NOPTS = -1.0;
  static constexpr double MAX_REASONABLE_PTS = 86400000000.0;

  double m_lastOutputPts{LOCAL_NOPTS};
  double m_truehd_ptsCache{LOCAL_NOPTS};
  bool m_truehd_ptsCacheValid{false};

  AudioSync::CAdaptiveJitterFilter m_jitterTracker;

  static constexpr double JITTER_THRESHOLD_TRUEHD_DTS = 100000.0;
  static constexpr double JITTER_THRESHOLD_DEFAULT = 10000.0;
  double m_jitterThreshold{JITTER_THRESHOLD_DEFAULT};

  static constexpr double CONFIDENCE_THRESHOLD = 0.55;

  double m_internalClock{LOCAL_NOPTS};
  bool m_needsResync{true};
};
/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDAudioCodecPassthrough.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDStreamInfo.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
constexpr unsigned int TRUEHD_BUF_SIZE = 61440;

constexpr double LOCAL_NOPTS = -1.0;
constexpr double MAX_REASONABLE_PTS = 86400000000.0;

inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}
}

CDVDAudioCodecPassthrough::CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType) :
  CDVDAudioCodec(processInfo)
{
  m_format.m_streamInfo.m_type = streamType;
  m_deviceIsRAW = processInfo.WantsRawPassthrough();

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  Dispose();
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  m_parser.SetCoreOnly(false);
  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_codecName = "pt-ac3";
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_codecName = "pt-eac3";
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      m_codecName = "pt-dtshd_ma";
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd_hra";
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      if (m_lavStyleSyncEnabled)
        m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - passthrough output device is {}",
                __func__, m_deviceIsRAW ? "RAW" : "IEC");
      break;

    default:
      return false;
  }

  if (m_lavStyleSyncEnabled)
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - LAV Full sync ENABLED, jitter threshold {:.0f}ms for {}",
              __func__, m_jitterThreshold / 1000.0, m_codecName);
  }
  else if (m_lavSeamlessBranchEnabled)
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - LAV Seamless Branch ENABLED for {}",
              __func__, m_codecName);
  }

  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;

  if (m_lavStyleSyncEnabled)
  {
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_lastOutputPts = LOCAL_NOPTS;
    m_jitterTracker.Reset();
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
  return true;
}

void CDVDAudioCodecPassthrough::Dispose()
{
  if (m_buffer)
  {
    delete[] m_buffer;
    m_buffer = nullptr;
  }

  free(m_backlogBuffer);
  m_backlogBuffer = nullptr;
  m_backlogBufferSize = 0;

  m_bufferSize = 0;
}

bool CDVDAudioCodecPassthrough::AddData(const DemuxPacket &packet)
{
  if (m_backlogSize)
  {
    m_dataSize = m_bufferSize;
    unsigned int consumed = m_parser.AddData(m_backlogBuffer, m_backlogSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    if (consumed != m_backlogSize)
    {
      memmove(m_backlogBuffer, m_backlogBuffer+consumed, m_backlogSize-consumed);
    }
    m_backlogSize -= consumed;
  }

  auto pData(const_cast<uint8_t*>(packet.pData));
  int iSize(packet.iSize);

  if (m_lavStyleSyncEnabled)
  {
    double incomingPts = packet.pts;
    bool ptsIsValid = IsValidPts(incomingPts);

    if (pData)
    {
      if (!IsValidPts(m_currentPts))
        m_currentPts = LOCAL_NOPTS;
      if (!IsValidPts(m_nextPts))
        m_nextPts = LOCAL_NOPTS;

      if (m_currentPts == LOCAL_NOPTS)
      {
        if (m_nextPts != LOCAL_NOPTS)
        {
          m_currentPts = m_nextPts;
          m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
        }
        else if (ptsIsValid)
        {
          m_currentPts = incomingPts;
        }
      }
      else if (ptsIsValid)
      {
        m_nextPts = incomingPts;
      }
    }
  }
  else
  {
    if (pData)
    {
      if (m_currentPts == DVD_NOPTS_VALUE)
      {
        if (m_nextPts != DVD_NOPTS_VALUE)
        {
          m_currentPts = m_nextPts;
          m_nextPts = packet.pts;
        }
        else if (packet.pts != DVD_NOPTS_VALUE)
        {
          m_currentPts = packet.pts;
        }
      }
      else
      {
        m_nextPts = packet.pts;
      }
    }
  }

  if (pData && !m_backlogSize)
  {
    if (iSize <= 0)
      return true;

    m_dataSize = m_bufferSize;
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);

    if (used != iSize)
    {
      const unsigned int remaining = static_cast<unsigned int>(iSize - used);
      if (m_backlogBufferSize < remaining)
      {
        m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, remaining);
        m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
      }
      m_backlogSize = remaining;
      memcpy(m_backlogBuffer, pData + used, m_backlogSize);
    }
  }
  else if (pData)
  {
    const unsigned int newSize = m_backlogSize + static_cast<unsigned int>(iSize);
    if (m_backlogBufferSize < newSize)
    {
      m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, newSize);
      m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
    }
    memcpy(m_backlogBuffer + m_backlogSize, pData, iSize);
    m_backlogSize += static_cast<unsigned int>(iSize);
  }

  if (!m_dataSize)
    return true;

  m_format.m_dataFormat = AE_FMT_RAW;
  m_format.m_streamInfo = m_parser.GetStreamInfo();
  m_format.m_sampleRate = m_parser.GetSampleRate();
  m_format.m_frameSize = 1;
  CAEChannelInfo layout;
  for (unsigned int i = 0; i < m_parser.GetChannels(); i++)
  {
    layout += AE_CH_RAW;
  }
  m_format.m_channelLayout = layout;

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (m_deviceIsRAW)
    {
      m_dataSize = PackTrueHD();
    }
    else
    {
      if (m_lavStyleSyncEnabled)
      {
        if (!m_truehd_ptsCacheValid && IsValidPts(m_currentPts))
        {
          m_truehd_ptsCache = m_currentPts;
          m_truehd_ptsCacheValid = true;
        }
      }

      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;

        if (m_lavStyleSyncEnabled)
        {
          (void)m_packerMAT->HadDiscontinuity();

          if (m_truehd_ptsCacheValid)
          {
            m_currentPts = m_truehd_ptsCache;
            m_truehd_ptsCacheValid = false;
            m_truehd_ptsCache = LOCAL_NOPTS;
          }
        }
      }
      else
      {
        m_dataSize = 0;
      }
    }
  }

  return true;
}

unsigned int CDVDAudioCodecPassthrough::PackTrueHD()
{
  unsigned int dataSize{0};

  if (m_trueHDoffset == 0)
    m_trueHDframes = 0;

  memcpy(m_trueHDBuffer.data() + m_trueHDoffset, m_buffer, m_dataSize);

  m_trueHDoffset += m_dataSize;
  m_trueHDframes++;

  if (m_trueHDframes == 24)
  {
    dataSize = m_trueHDoffset;
    m_trueHDoffset = 0;
    m_trueHDframes = 0;
    return dataSize;
  }

  return 0;
}

void CDVDAudioCodecPassthrough::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = GetData(frame.data);
  frame.framesOut = 0;
  frame.hasDiscontinuity = false;
  frame.discontinuityCorrection = 0.0;

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = 8;
  frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());

  if (m_lavStyleSyncEnabled)
  {
    const CAEStreamInfo::DataType streamType = m_format.m_streamInfo.m_type;
    const bool isTrueHD = (streamType == CAEStreamInfo::STREAM_TYPE_TRUEHD);

    double samplesOffsetTime = 0.0;
    if (isTrueHD && m_packerMAT)
    {
      int samplesOffset = m_packerMAT->GetSamplesOffset();
      if (samplesOffset != 0)
      {
        samplesOffsetTime = static_cast<double>(samplesOffset) / m_format.m_sampleRate * DVD_TIME_BASE;
      }
    }

    const double demuxerPts = m_currentPts;
    const bool haveDemuxerPts = IsValidPts(demuxerPts);

    if (m_needsResync && haveDemuxerPts)
    {
      m_internalClock = demuxerPts;
      m_needsResync = false;
      m_jitterTracker.Reset();

      CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough: Internal clock synced to demuxer PTS {:.3f}s",
                demuxerPts / DVD_TIME_BASE);
    }

    if (IsValidPts(m_internalClock) && haveDemuxerPts)
    {
      double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
      m_jitterTracker.Sample(jitter);

      double absMinJitter = m_jitterTracker.AbsMinimum();

      if (std::abs(absMinJitter) > m_jitterThreshold)
      {
        m_internalClock -= absMinJitter;
        m_jitterTracker.OffsetValues(-absMinJitter);

        frame.hasDiscontinuity = true;
        frame.discontinuityCorrection = absMinJitter;

        CLog::Log(LOGDEBUG,
                  "CDVDAudioCodecPassthrough: Jitter correction {:.2f}ms (threshold {:.0f}ms)",
                  absMinJitter / 1000.0,
                  m_jitterThreshold / 1000.0);
      }
    }

    if (IsValidPts(m_internalClock))
    {
      frame.pts = m_internalClock;
      m_internalClock += frame.duration;
      m_lastOutputPts = frame.pts;
      m_dataCacheCore.SetAudioPts(frame.pts);
    }
    else if (haveDemuxerPts)
    {
      frame.pts = demuxerPts;
      m_internalClock = demuxerPts + frame.duration;
      m_lastOutputPts = frame.pts;
      m_dataCacheCore.SetAudioPts(frame.pts);
    }
    else
    {
      frame.pts = DVD_NOPTS_VALUE;
    }

    m_currentPts = LOCAL_NOPTS;
  }
  else
  {
    frame.pts = m_currentPts;

    if (m_currentPts != DVD_NOPTS_VALUE)
      m_dataCacheCore.SetAudioPts(m_currentPts);

    m_currentPts = DVD_NOPTS_VALUE;
  }
}

int CDVDAudioCodecPassthrough::GetData(uint8_t** dst)
{
  if (!m_dataSize)
    AddData(DemuxPacket());

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    *dst = m_trueHDBuffer.data();
  else
    *dst = m_buffer;

  int bytes = m_dataSize;
  m_dataSize = 0;
  return bytes;
}

void CDVDAudioCodecPassthrough::Reset()
{
  m_trueHDoffset = 0;
  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;

  if (m_lavStyleSyncEnabled)
  {
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_lastOutputPts = LOCAL_NOPTS;

    m_truehd_ptsCache = LOCAL_NOPTS;
    m_truehd_ptsCacheValid = false;

    m_internalClock = LOCAL_NOPTS;
    m_needsResync = true;
    m_jitterTracker.Reset();

    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::Reset - Internal clock reset, will resync");

    if (m_packerMAT)
      m_packerMAT->Reset();

    m_parser.Reset();
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
    m_parser.Reset();
  }
}

void CDVDAudioCodecPassthrough::SetLavStyleSyncEnabled(bool enabled)
{
  m_lavStyleSyncEnabled = enabled;

  if (enabled)
    m_lavSeamlessBranchEnabled = true;

  if (m_packerMAT)
    m_packerMAT->SetLavStyleEnabled(m_lavStyleSyncEnabled || m_lavSeamlessBranchEnabled);
}

void CDVDAudioCodecPassthrough::SetLavSeamlessBranchEnabled(bool enabled)
{
  m_lavSeamlessBranchEnabled = enabled;

  if (m_packerMAT)
    m_packerMAT->SetLavStyleEnabled(m_lavStyleSyncEnabled || m_lavSeamlessBranchEnabled);
}

void CDVDAudioCodecPassthrough::ResetLavSyncState()
{
  if (!m_lavStyleSyncEnabled)
    return;

  m_lastOutputPts = LOCAL_NOPTS;
  m_truehd_ptsCache = LOCAL_NOPTS;
  m_truehd_ptsCacheValid = false;
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();

  CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::ResetLavSyncState - Internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  if (!m_lavStyleSyncEnabled)
    return;

  if (pts != DVD_NOPTS_VALUE && pts >= 0.0 && pts <= MAX_REASONABLE_PTS)
  {
    m_internalClock = pts;
    m_needsResync = false;
    m_jitterTracker.Reset();

    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Internal clock set to RESYNC pts {:.3f}s",
              pts / DVD_TIME_BASE);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Invalid pts, ignoring");
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}
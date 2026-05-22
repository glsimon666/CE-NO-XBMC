/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PackerMAT.h"

#include "utils/log.h"

#include <array>
#include <assert.h>
#include <utility>

extern "C"
{
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>
}

namespace
{
constexpr uint32_t FORMAT_MAJOR_SYNC = 0xf8726fba;

constexpr auto BURST_HEADER_SIZE = 8;
constexpr auto MAT_BUFFER_SIZE = 61440;
constexpr auto MAT_BUFFER_LIMIT = MAT_BUFFER_SIZE - 24;
constexpr auto MAT_POS_MIDDLE = 30708 + BURST_HEADER_SIZE;

constexpr std::array<uint8_t, 20> mat_start_code = {0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01,
                                                    0x01, 0x80, 0x00, 0x56, 0xA5, 0x3B, 0xF4,
                                                    0x81, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 12> mat_middle_code = {0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA,
                                                     0x82, 0x83, 0x49, 0x80, 0x77, 0xE0};

constexpr std::array<uint8_t, 24> mat_end_code = {0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
} // namespace

CPackerMAT::CPackerMAT()
{
  m_buffer.reserve(MAT_BUFFER_SIZE);
}

void CPackerMAT::Reset()
{
  m_state = {};
  m_buffer.clear();
  m_bufferCount = 0;
  m_outputQueue.clear();
  m_offsetQueue.clear();
  m_discontinuityQueue.clear();
  m_lastOutputSamplesOffset = 0;
  m_lastOutputHadDiscontinuity = false;
  m_pendingDiscontinuity = false;
}

bool CPackerMAT::PackTrueHD(const uint8_t* data, int size)
{
  TrueHDMajorSyncInfo info;
  bool isMajorSync = (AV_RB32(data + 4) == FORMAT_MAJOR_SYNC);

  if (isMajorSync)
  {
    info = ParseTrueHDMajorSyncHeaders(data, size);

    if (!info.valid)
      return false;

    m_state.ratebits = info.ratebits;
  }
  else if (m_state.prevFrametimeValid == false)
  {
    m_state.numberOfSamplesOffset = 0;
    return false;
  }

  const uint16_t frameTime = AV_RB16(data + 2);
  uint32_t spaceSize = 0;
  const uint16_t frameSamples = 40 << (m_state.ratebits & 7);
  m_state.outputTiming += frameSamples;

  if (info.outputTimingPresent)
  {
    if (m_lavStyleEnabled && m_state.outputTimingValid && (info.outputTiming != m_state.outputTiming))
    {
      CLog::Log(LOGDEBUG, "CPackerMAT::PackTrueHD: detected stream discontinuity "
                "(seamless branch), expected outputTiming={}, actual={}",
                m_state.outputTiming, info.outputTiming);

      m_state.prevFrametimeValid = false;
      spaceSize = 40 * (64 >> (m_state.ratebits & 7));

      uint32_t prevOutput = static_cast<uint16_t>(info.outputTiming - frameSamples);
      if (prevOutput < frameTime)
        prevOutput += UINT16_MAX;

      int32_t currentFrameOutputOffset = static_cast<int32_t>(prevOutput - frameTime);

      if (m_state.nOutputTimeOffset >= currentFrameOutputOffset)
        m_state.padding += (m_state.nOutputTimeOffset - currentFrameOutputOffset) * static_cast<int32_t>(64 >> (m_state.ratebits & 7));

      CLog::Log(LOGDEBUG, "CPackerMAT::PackTrueHD: carrying forward {} padding (offset {} - {})",
                m_state.padding, m_state.nOutputTimeOffset, currentFrameOutputOffset);

      m_pendingDiscontinuity = true;
    }
    m_state.outputTiming = info.outputTiming;
    m_state.outputTimingValid = true;
  }

  if (m_state.prevFrametimeValid)
    spaceSize = uint16_t(frameTime - m_state.prevFrametime) * (64 >> (m_state.ratebits & 7));

  assert(!m_state.prevFrametimeValid || spaceSize >= m_state.prevMatFramesize);

  if (spaceSize < m_state.prevMatFramesize)
    spaceSize = FFALIGN(m_state.prevMatFramesize, (64 >> (m_state.ratebits & 7)));

  m_state.padding += static_cast<int32_t>(spaceSize - m_state.prevMatFramesize);

  if (m_lavStyleEnabled)
  {
    if (m_state.padding < 0)
      m_state.padding = 0;
  }
  else
  {
    if (m_state.padding > MAT_BUFFER_SIZE * 5)
    {
      CLog::Log(LOGINFO, "CPackerMAT::PackTrueHD: seek detected, re-initializing MAT packer state");
      m_state = {};
      m_state.init = true;
      m_buffer.clear();
      m_bufferCount = 0;
      return false;
    }
  }

  if (m_lavStyleEnabled && m_state.outputTimingValid)
  {
    uint32_t prevOutput = static_cast<uint16_t>(m_state.outputTiming - frameSamples);
    if (prevOutput < frameTime)
      prevOutput += UINT16_MAX;

    m_state.nOutputTimeOffset = static_cast<int32_t>(prevOutput - frameTime);
  }

  m_state.prevFrametime = frameTime;
  m_state.prevFrametimeValid = true;

  if (GetCount() == 0)
  {
    WriteHeader();

    if (m_state.init == false)
    {
      m_state.init = true;
      m_state.matFramesize = 0;
    }
  }

  while (m_state.padding > 0)
  {
    WritePadding();

    assert(m_state.padding == 0 || GetCount() == MAT_BUFFER_SIZE);

    if (GetCount() == MAT_BUFFER_SIZE)
    {
      FlushPacket();

      WriteHeader();
    }
  }

  m_state.samples += frameSamples;

  int remaining = FillDataBuffer(data, size, Type::DATA);

  if (remaining || GetCount() == MAT_BUFFER_SIZE)
  {
    FlushPacket();

    if (remaining)
    {
      WriteHeader();

      remaining = FillDataBuffer(data + (size - remaining), remaining, Type::DATA);

      assert(remaining == 0);
    }
  }

  m_state.prevMatFramesize = m_state.matFramesize;
  m_state.matFramesize = 0;

  return !m_outputQueue.empty();
}

std::vector<uint8_t> CPackerMAT::GetOutputFrame()
{
  if (m_outputQueue.empty())
    return {};

  std::vector<uint8_t> buffer = std::move(m_outputQueue.front());
  m_outputQueue.pop_front();

  if (m_lavStyleEnabled)
  {
    if (!m_offsetQueue.empty())
    {
      m_lastOutputSamplesOffset = m_offsetQueue.front();
      m_offsetQueue.pop_front();
    }
    else
    {
      m_lastOutputSamplesOffset = 0;
    }

    if (!m_discontinuityQueue.empty())
    {
      m_lastOutputHadDiscontinuity = m_discontinuityQueue.front();
      m_discontinuityQueue.pop_front();
    }
    else
    {
      m_lastOutputHadDiscontinuity = false;
    }
  }
  else
  {
    m_lastOutputSamplesOffset = 0;
    m_lastOutputHadDiscontinuity = false;
    m_offsetQueue.clear();
    m_discontinuityQueue.clear();
  }

  return buffer;
}

void CPackerMAT::WriteHeader()
{
  m_buffer.resize(MAT_BUFFER_SIZE);

  const size_t size = BURST_HEADER_SIZE + mat_start_code.size();

  memcpy(m_buffer.data() + BURST_HEADER_SIZE, mat_start_code.data(), mat_start_code.size());
  m_bufferCount = static_cast<uint32_t>(size);

  m_state.matFramesize += static_cast<uint32_t>(size);

  if (m_state.padding > 0)
  {
    if (m_state.padding > static_cast<int32_t>(size))
    {
      m_state.padding -= static_cast<int32_t>(size);
      m_state.matFramesize = 0;
    }
    else
    {
      m_state.matFramesize = static_cast<uint32_t>(static_cast<int32_t>(size) - m_state.padding);
      m_state.padding = 0;
    }
  }
}

void CPackerMAT::WritePadding()
{
  if (m_state.padding <= 0)
    return;

  const int remaining = FillDataBuffer(nullptr, static_cast<int>(m_state.padding), Type::PADDING);

  if (remaining >= 0)
  {
    m_state.padding = remaining;
    m_state.matFramesize = 0;
  }
  else
  {
    m_state.padding = 0;
    m_state.matFramesize = static_cast<uint32_t>(-remaining);
  }
}

void CPackerMAT::AppendData(const uint8_t* data, int size, Type type)
{
  if (type == Type::DATA)
    memcpy(m_buffer.data() + m_bufferCount, data, size);

  m_state.matFramesize += static_cast<uint32_t>(size);
  m_bufferCount += static_cast<uint32_t>(size);
}

int CPackerMAT::FillDataBuffer(const uint8_t* data, int size, Type type)
{
  if (GetCount() >= MAT_BUFFER_LIMIT)
    return size;

  int remaining = size;

  if (GetCount() <= MAT_POS_MIDDLE && GetCount() + static_cast<uint32_t>(size) > MAT_POS_MIDDLE)
  {
    int nBytesBefore = static_cast<int>(MAT_POS_MIDDLE - GetCount());
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    AppendData(mat_middle_code.data(), static_cast<int>(mat_middle_code.size()), Type::DATA);

    if (type == Type::PADDING)
      remaining -= static_cast<int>(mat_middle_code.size());

    if (remaining > 0)
      remaining = FillDataBuffer(data + nBytesBefore, remaining, type);

    return remaining;
  }

  if (GetCount() + static_cast<uint32_t>(size) >= MAT_BUFFER_LIMIT)
  {
    int nBytesBefore = static_cast<int>(MAT_BUFFER_LIMIT - GetCount());
    AppendData(data, nBytesBefore, type);
    remaining -= nBytesBefore;

    AppendData(mat_end_code.data(), static_cast<int>(mat_end_code.size()), Type::DATA);

    assert(GetCount() == MAT_BUFFER_SIZE);

    if (type == Type::PADDING)
      remaining -= static_cast<int>(mat_end_code.size());

    return remaining;
  }

  AppendData(data, size, type);

  return 0;
}

void CPackerMAT::FlushPacket()
{
  if (GetCount() == 0)
    return;

  assert(GetCount() == MAT_BUFFER_SIZE);

  const uint16_t frameSamples = 40 << (m_state.ratebits & 7);
  const uint32_t MATSamples = (frameSamples * 24);

  m_outputQueue.emplace_back(std::move(m_buffer));

  if (m_lavStyleEnabled)
  {
    m_offsetQueue.push_back(m_state.numberOfSamplesOffset);
    m_discontinuityQueue.push_back(m_pendingDiscontinuity);
    m_pendingDiscontinuity = false;

    if (MATSamples != m_state.samples)
      m_state.numberOfSamplesOffset += static_cast<int32_t>(m_state.samples) - static_cast<int32_t>(MATSamples);
  }

  m_state.samples = 0;

  m_buffer.clear();
  m_bufferCount = 0;
}

TrueHDMajorSyncInfo CPackerMAT::ParseTrueHDMajorSyncHeaders(const uint8_t* p, int buffsize) const
{
  TrueHDMajorSyncInfo info;

  if (buffsize < 32)
    return {};

  int majorSyncSize = 28;
  if (p[29] & 1)
  {
    int extensionSize = p[30] >> 4;
    majorSyncSize += 2 + extensionSize * 2;
  }

  CBitStream bs(p + 4, buffsize - 4);

  bs.SkipBits(32);

  info.ratebits = bs.ReadBits(4);
  info.valid = true;

  bs.SkipBits(1 + 1 + 2 + 2 + 2 + 5 + 2 + 13 + 16 + 16 + 16 + 1 + 15);

  const int numSubstreams = bs.ReadBits(4);

  bs.SkipBits(4 + (majorSyncSize - 17) * 8);

  for (int i = 0; i < numSubstreams; i++)
  {
    int extraSubstreamWord = bs.ReadBits(1);
    bs.SkipBits(15);
    if (extraSubstreamWord)
      bs.SkipBits(16);
  }

  for (int i = 0; i < numSubstreams; i++)
  {
    if (bs.ReadBits(1))
    {
      if (bs.ReadBits(1))
      {
        bs.SkipBits(14);
        info.outputTiming = bs.ReadBits(16);
        info.outputTimingPresent = true;
      }
    }
    break;
  }

  return info;
}
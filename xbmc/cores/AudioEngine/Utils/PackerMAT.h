/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <deque>
#include <stdint.h>
#include <vector>

struct TrueHDMajorSyncInfo
{
  int ratebits{0};
  uint16_t outputTiming{0};
  bool outputTimingPresent{false};
  bool valid{false};
};

enum class Type
{
  PADDING,
  DATA,
};

class CPackerMAT
{
public:
  CPackerMAT();
  ~CPackerMAT() = default;

  bool PackTrueHD(const uint8_t* data, int size);
  std::vector<uint8_t> GetOutputFrame();

  int GetSamplesOffset() const { return m_lavStyleEnabled ? m_lastOutputSamplesOffset : 0; }
  bool HadDiscontinuity() const { return m_lavStyleEnabled ? m_lastOutputHadDiscontinuity : false; }
  void Reset();

  void SetLavStyleEnabled(bool enabled) { m_lavStyleEnabled = enabled; }
  bool IsLavStyleEnabled() const { return m_lavStyleEnabled; }

private:
  struct MATState
  {
    bool init;

    int ratebits;

    uint16_t outputTiming;
    bool outputTimingValid;

    uint16_t prevFrametime;
    bool prevFrametimeValid;

    uint32_t matFramesize;
    uint32_t prevMatFramesize;

    int32_t padding;
    uint32_t samples;
    int32_t numberOfSamplesOffset;
    int32_t nOutputTimeOffset;

    /* Two-stage seamless branch confirmation */
    bool     branchCheckPending{false};
    uint32_t branchCheckDelta{0};
    bool     branchConfirmed{false};
  };

  void WriteHeader();
  void WritePadding();
  void AppendData(const uint8_t* data, int size, Type type);
  uint32_t GetCount() const { return m_bufferCount; }
  int FillDataBuffer(const uint8_t* data, int size, Type type);
  void FlushPacket();
  TrueHDMajorSyncInfo ParseTrueHDMajorSyncHeaders(const uint8_t* p, int buffsize) const;

  MATState m_state{};
  bool m_lavStyleEnabled{false};
  int m_lastOutputSamplesOffset{0};
  bool m_lastOutputHadDiscontinuity{false};

  uint32_t m_bufferCount{0};
  std::vector<uint8_t> m_buffer;
  std::deque<std::vector<uint8_t>> m_outputQueue;

  std::deque<int> m_offsetQueue;
  std::deque<bool> m_discontinuityQueue;
  bool m_pendingDiscontinuity{false};
};

class CBitStream
{
public:
  CBitStream(const uint8_t* bytes, int _size)
  {
    data = bytes;
    size = _size;
  }

  int ReadBits(int bits)
  {
    int dat = 0;
    for (int i = index; i < index + bits; i++)
    {
      dat = dat * 2 + getbit(data[i / 8], i % 8);
    }
    index += bits;
    return dat;
  }

  void SkipBits(int bits) { index += bits; }

private:
  uint8_t getbit(uint8_t x, int y) { return (x >> (7 - y)) & 1; }

  const uint8_t* data{nullptr};
  int size{0};
  int index{0};
};
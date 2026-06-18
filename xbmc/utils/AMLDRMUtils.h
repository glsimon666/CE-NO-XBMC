/*
 *  Copyright (C) 2024 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

class CAMLDRMUtils
{
public:
  CAMLDRMUtils();
  ~CAMLDRMUtils();

  bool IsAvailable() const { return m_fd >= 0; }

  // DRM device management
  bool OpenDevice();
  void CloseDevice();

  // DRM display control
  bool SetCrtcActive(bool active);
  bool SetMode(unsigned int width, unsigned int height);

  // DRM property access
  int GetProperty(const char* name, uint32_t obj_type);
  bool SetProperty(const char* name, uint32_t obj_type, uint32_t value);

private:
  int m_fd{-1};
  uint32_t m_crtc_id{0};
  uint32_t m_connector_id{0};
  uint32_t m_encoder_id{0};
  uint32_t m_plane_id{0};

  bool m_initialized{false};
};

// Global DRM utils instance
CAMLDRMUtils& AMLDRMUtils();

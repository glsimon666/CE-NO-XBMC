/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/EGLUtils.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext();
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(const RenderStereoMode mode) const override;
  void PresentRender(bool rendered, bool videoLayer) override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
  bool HasSubSurface() const override { return m_subEGLSurface != EGL_NO_SURFACE; }
  void BeginSubtitleRender() override;
  void EndSubtitleRender() override;
  void ClearSubFBId() override;
  EGLSurface GetSubEGLSurface() const { return m_subEGLSurface; }
  uint32_t GetSubFBId() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override {};

private:
  std::unique_ptr<CEGLContextUtils> m_pGLContext;
  EGLSurface m_subEGLSurface{EGL_NO_SURFACE};
  uint32_t m_subFBId{0};
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;
};

}
}
}

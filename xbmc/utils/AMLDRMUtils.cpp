/*
 *  Copyright (C) 2024 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLDRMUtils.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "utils/log.h"

CAMLDRMUtils& AMLDRMUtils()
{
  static CAMLDRMUtils instance;
  return instance;
}

CAMLDRMUtils::CAMLDRMUtils()
{
  OpenDevice();
}

CAMLDRMUtils::~CAMLDRMUtils()
{
  CloseDevice();
}

bool CAMLDRMUtils::OpenDevice()
{
  if (m_fd >= 0)
    return true;

  // Try to open DRM render node
  for (int i = 128; i < 140; i++)
  {
    std::string path = "/dev/dri/renderD" + std::to_string(i);
    m_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (m_fd >= 0)
    {
      CLog::Log(LOGINFO, "CAMLDRMUtils::{} - opened {}", __FUNCTION__, path);
      break;
    }
  }

  if (m_fd < 0)
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - no DRM device found", __FUNCTION__);
    return false;
  }

  // Get DRM resources
  drmModeResPtr resources = drmModeGetResources(m_fd);
  if (!resources)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get DRM resources", __FUNCTION__);
    close(m_fd);
    m_fd = -1;
    return false;
  }

  // Find connected HDMI connector
  for (int i = 0; i < resources->count_connectors; i++)
  {
    drmModeConnectorPtr connector = drmModeGetConnector(m_fd, resources->connectors[i]);
    if (!connector)
      continue;

    if (connector->connection == DRM_MODE_CONNECTED &&
        connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)
    {
      m_connector_id = connector->connector_id;
      m_encoder_id = connector->encoder_id;

      // Get encoder to find CRTC
      if (m_encoder_id)
      {
        drmModeEncoderPtr encoder = drmModeGetEncoder(m_fd, m_encoder_id);
        if (encoder)
        {
          m_crtc_id = encoder->crtc_id;
          drmModeFreeEncoder(encoder);
        }
      }
      drmModeFreeConnector(connector);
      break;
    }
    drmModeFreeConnector(connector);
  }

  // Find primary plane
  drmModePlaneResPtr planeResources = drmModeGetPlaneResources(m_fd);
  if (planeResources)
  {
    for (uint32_t i = 0; i < planeResources->count_planes; i++)
    {
      drmModePlanePtr plane = drmModeGetPlane(m_fd, planeResources->planes[i]);
      if (!plane)
        continue;

      // Get plane type property
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(m_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
      if (props)
      {
        for (uint32_t j = 0; j < props->count_props; j++)
        {
          drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[j]);
          if (prop && strcmp(prop->name, "type") == 0)
          {
            if (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
            {
              m_plane_id = plane->plane_id;
            }
            drmModeFreeProperty(prop);
            break;
          }
          if (prop)
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
      }

      if (m_plane_id)
      {
        drmModeFreePlane(plane);
        break;
      }
      drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planeResources);
  }

  drmModeFreeResources(resources);

  m_initialized = (m_crtc_id && m_connector_id);
  if (m_initialized)
  {
    CLog::Log(LOGINFO, "CAMLDRMUtils::{} - initialized: crtc={}, connector={}, plane={}",
      __FUNCTION__, m_crtc_id, m_connector_id, m_plane_id);
  }

  return m_initialized;
}

void CAMLDRMUtils::CloseDevice()
{
  if (m_fd >= 0)
  {
    close(m_fd);
    m_fd = -1;
  }
  m_crtc_id = 0;
  m_connector_id = 0;
  m_encoder_id = 0;
  m_plane_id = 0;
  m_initialized = false;
}

bool CAMLDRMUtils::SetCrtcActive(bool active)
{
  if (!m_initialized)
    return false;

  int ret = drmModeSetCrtc(m_fd, m_crtc_id, 0, 0, 0, &m_connector_id, 1, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to set crtc active: {}", __FUNCTION__, ret);
    return false;
  }

  return true;
}

bool CAMLDRMUtils::SetMode(unsigned int width, unsigned int height)
{
  if (!m_initialized)
    return false;

  // Find matching mode
  drmModeConnectorPtr connector = drmModeGetConnector(m_fd, m_connector_id);
  if (!connector)
    return false;

  drmModeModeInfoPtr mode = nullptr;
  for (int i = 0; i < connector->count_modes; i++)
  {
    if (connector->modes[i].hdisplay == width &&
        connector->modes[i].vdisplay == height)
    {
      mode = &connector->modes[i];
      break;
    }
  }

  if (!mode)
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - mode {}x{} not found", __FUNCTION__, width, height);
    drmModeFreeConnector(connector);
    return false;
  }

  int ret = drmModeSetCrtc(m_fd, m_crtc_id, 0, 0, 0, &m_connector_id, 1, mode);
  drmModeFreeConnector(connector);

  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to set mode: {}", __FUNCTION__, ret);
    return false;
  }

  return true;
}

int CAMLDRMUtils::GetProperty(const char* name, uint32_t obj_type)
{
  if (m_fd < 0)
    return -1;

  uint32_t obj_id;
  switch (obj_type)
  {
    case DRM_MODE_OBJECT_CRTC:
      obj_id = m_crtc_id;
      break;
    case DRM_MODE_OBJECT_PLANE:
      obj_id = m_plane_id;
      break;
    case DRM_MODE_OBJECT_CONNECTOR:
      obj_id = m_connector_id;
      break;
    default:
      return -1;
  }

  if (!obj_id)
    return -1;

  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(m_fd, obj_id, obj_type);
  if (!props)
    return -1;

  int value = -1;
  for (uint32_t i = 0; i < props->count_props; i++)
  {
    drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[i]);
    if (prop && strcmp(prop->name, name) == 0)
    {
      value = props->prop_values[i];
      drmModeFreeProperty(prop);
      break;
    }
    if (prop)
      drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return value;
}

bool CAMLDRMUtils::SetProperty(const char* name, uint32_t obj_type, uint32_t value)
{
  if (m_fd < 0)
    return false;

  uint32_t obj_id;
  switch (obj_type)
  {
    case DRM_MODE_OBJECT_CRTC:
      obj_id = m_crtc_id;
      break;
    case DRM_MODE_OBJECT_PLANE:
      obj_id = m_plane_id;
      break;
    case DRM_MODE_OBJECT_CONNECTOR:
      obj_id = m_connector_id;
      break;
    default:
      return false;
  }

  if (!obj_id)
    return false;

  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(m_fd, obj_id, obj_type);
  if (!props)
    return false;

  uint32_t prop_id = 0;
  for (uint32_t i = 0; i < props->count_props; i++)
  {
    drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[i]);
    if (prop && strcmp(prop->name, name) == 0)
    {
      prop_id = props->props[i];
      drmModeFreeProperty(prop);
      break;
    }
    if (prop)
      drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);

  if (!prop_id)
    return false;

  drmModeAtomicReqPtr req = drmModeAtomicAlloc();
  if (!req)
    return false;

  drmModeAtomicAddProperty(req, obj_id, prop_id, value);
  int ret = drmModeAtomicCommit(m_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
  drmModeAtomicFree(req);

  return ret >= 0;
}

/*
 *  Copyright (C) 2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
varying vec4 m_cord0;
uniform float m_sdrPeak;
uniform float m_sdrSaturation;

vec3 convertGuiForPqOutput(vec3 x)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;

  const mat3 matx = mat3(
      0.627402, 0.069095, 0.016394,
      0.329292, 0.919544, 0.088028,
      0.043306, 0.011360, 0.895578);

  x = max(x, vec3(0.0));

  // REC.709 to linear (approx)
  x = pow(x, vec3(1.0 / 0.45));

  // REC.709 to BT.2020
  x = matx * x;
  x = max(x, vec3(0.0));

  // Optional saturation adjustment for SDR GUI in PQ output
  vec3 luma = vec3(dot(x, vec3(0.2627, 0.6780, 0.0593)));
  x = mix(luma, x, m_sdrSaturation);
  x = max(x, vec3(0.0));

  // Scale SDR peak to nits, normalize to 10000 nits, PQ encode
  float peakNits = 100.0 * m_sdrPeak;
  x = pow(x * (peakNits / 10000.0), vec3(ST2084_m1));
  x = (ST2084_c1 + ST2084_c2 * x) / (1.0 + ST2084_c3 * x);
  x = pow(x, vec3(ST2084_m2));

  return x;
}

void main ()
{
  vec3 rgb = texture2D(m_samp0, m_cord0.xy).rgb;

#if defined(KODI_LIMITED_RANGE)
  rgb *= (235.0 - 16.0) / 255.0;
  rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  rgb.rgb = convertGuiForPqOutput(rgb);
#endif

  gl_FragColor = vec4(rgb, 1.0);
}

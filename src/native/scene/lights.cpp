#include "scene/blackbody.h"
#include "scene/ies.h"
#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace remix {
  using namespace scene;
  using namespace scene::detail;
  namespace {
    constexpr float pi = std::numbers::pi_v<float>;
    using Vec = std::array<float, 3>;
    Vec vec(remixapi_Float3D v) { return {v.x, v.y, v.z}; }
    Vec add(Vec a, Vec b) {
      for (int c = 0; c < 3; ++c)
        a[c] += b[c];
      return a;
    }
    Vec scale(Vec a, float s) {
      for (auto &v : a)
        v *= s;
      return a;
    }
    Vec cross(Vec a, Vec b) {
      return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }
    float length(Vec a) { return std::hypot(a[0], a[1], a[2]); }
    Vec unit(Vec a) {
      float n = length(a);
      if (n < 1e-8f || !std::isfinite(n))
        throw invalid;
      return scale(a, 1 / n);
    }
    float value(const float *v, float fallback) {
      float x = v ? *v : fallback;
      if (!std::isfinite(x))
        throw invalid;
      return x;
    }

  } // namespace

  remixapi_LightHandle NativeResources::overrideLight(remixapi_LightHandle original, uint64_t hash,
                                                      const remixapi_LightInfoUSDEXT &o,
                                                      bool replaceTransform, remixapi_Path texture) {
    auto found = m_lights.find(uint64_t(reinterpret_cast<uintptr_t>(original)));
    if (found == m_lights.end())
      throw invalid;
    const auto source = found->second.source;
    auto merged = o;
    merged.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    merged.pNext = nullptr;
    if (!merged.lightType)
      merged.lightType = source.type;
    if (!replaceTransform)
      merged.transform = source.transform;
    if (!merged.pColor)
      merged.pColor = &source.color;
    if (!merged.pIntensity)
      merged.pIntensity = &source.intensity;
    if (!merged.pExposure)
      merged.pExposure = &source.exposure;
    if (!merged.pEnableColorTemp)
      merged.pEnableColorTemp = &source.temperatureEnabled;
    if (!merged.pColorTemp)
      merged.pColorTemp = &source.temperature;
    if (!merged.pRadius)
      merged.pRadius = &source.radius;
    if (!merged.pWidth)
      merged.pWidth = &source.width;
    if (!merged.pHeight)
      merged.pHeight = &source.height;
    if (!merged.pLength)
      merged.pLength = &source.length;
    if (!merged.pAngleRadians)
      merged.pAngleRadians = &source.angle;
    if (!merged.pConeAngleRadians)
      merged.pConeAngleRadians = &source.cone;
    if (!merged.pConeSoftness)
      merged.pConeSoftness = &source.softness;
    if (!merged.pFocus)
      merged.pFocus = &source.focus;
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    if (merged.lightType == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT) {
      dome.transform = merged.transform;
      dome.colorTexture = texture ? texture : source.texture.c_str();
      merged.pNext = &dome;
    }
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = hash;
    info.pNext = &merged;
    remixapi_LightHandle handle{};
    createLight(info, handle);
    if (!replaceTransform) {
      auto &replacement = m_lights.at(hash);
      replacement.source.direction = source.direction;
      for (auto &emitter : replacement.emitters)
        std::copy_n(&source.direction.x, 3, emitter.direction);
    }
    return handle;
  }

  uint32_t NativeResources::iesProfile(std::string_view text) {
    const uint64_t hash = hashBytes(text.data(), text.size());
    if (auto found = m_iesIndex.find(hash); found != m_iesIndex.end())
      return found->second;
    IesTable table;
    if (!parseIes(text, table))
      throw invalid;
    auto next = std::make_shared<std::vector<float>>(m_iesProfiles ? *m_iesProfiles : std::vector<float>{});
    next->insert(next->end(), table.begin(), table.end());
    m_iesProfiles = std::move(next);
    const auto profile = uint32_t(m_iesProfiles->size() / kIesBins);
    m_iesIndex.emplace(hash, profile);
    return profile;
  }

  void NativeResources::createLight(const remixapi_LightInfo &info, remixapi_LightHandle &handle) {
    if (!info.hash)
      throw invalid;
    auto ext = [&](remixapi_StructType type) { return extension(info.pNext, type); };
    auto *usd = static_cast<const remixapi_LightInfoUSDEXT *>(ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT));
    Vec position{}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, -1}, radiance = vec(info.radiance);
    float radius = .5f, width = 1, height = 1, span = 1, angle = .53f;
    remixapi_LightInfoLightShaping shaping{};
    bool shaped = false;
    remixapi_StructType type{};
    if (usd) {
      type = usd->lightType;
      for (int c = 0; c < 3; ++c) {
        position[c] = usd->transform.matrix[c][3];
        x[c] = usd->transform.matrix[c][0];
        y[c] = usd->transform.matrix[c][1];
        z[c] = -usd->transform.matrix[c][2];
      }
      radius = value(usd->pRadius, .5f);
      width = value(usd->pWidth, 1);
      height = value(usd->pHeight, 1);
      span = value(usd->pLength, 1);
      angle = value(usd->pAngleRadians, .53f * pi / 180) * 180 / pi;
      radiance = usd->pColor ? vec(*usd->pColor) : Vec{1, 1, 1};
      float intensity = value(usd->pIntensity, 1) * std::exp2(value(usd->pExposure, 0));
      if (usd->pEnableColorTemp && *usd->pEnableColorTemp) {
        auto tint = blackbody(value(usd->pColorTemp, 6500));
        for (int c = 0; c < 3; ++c)
          radiance[c] *= tint[c];
      }
      radiance = scale(radiance, intensity);
      auto direction = unit(z);
      shaping = {{direction[0], direction[1], direction[2]},
                 value(usd->pConeAngleRadians, pi) * 180 / pi,
                 value(usd->pConeSoftness, 0),
                 value(usd->pFocus, 0)};
      shaped = shaping.coneAngleDegrees < 180 || shaping.focusExponent > 0;
    } else if (auto *v = static_cast<const remixapi_LightInfoSphereEXT *>(
                   ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT))) {
      type = v->sType;
      position = vec(v->position);
      radius = v->radius;
      shaped = v->shaping_hasvalue;
      shaping = v->shaping_value;
    } else if (auto *v = static_cast<const remixapi_LightInfoRectEXT *>(
                   ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT))) {
      type = v->sType;
      position = vec(v->position);
      x = vec(v->xAxis);
      y = vec(v->yAxis);
      z = vec(v->direction);
      width = v->xSize;
      height = v->ySize;
      shaped = v->shaping_hasvalue;
      shaping = v->shaping_value;
    } else if (auto *v = static_cast<const remixapi_LightInfoDiskEXT *>(
                   ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT))) {
      type = v->sType;
      position = vec(v->position);
      x = scale(vec(v->xAxis), v->xRadius);
      y = scale(vec(v->yAxis), v->yRadius);
      z = vec(v->direction);
      radius = 1;
      shaped = v->shaping_hasvalue;
      shaping = v->shaping_value;
    } else if (auto *v = static_cast<const remixapi_LightInfoCylinderEXT *>(
                   ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT))) {
      type = v->sType;
      position = vec(v->position);
      x = unit(vec(v->axis));
      y = unit(cross(x, std::abs(x[2]) < .9f ? Vec{0, 0, 1} : Vec{0, 1, 0}));
      z = cross(x, y);
      radius = v->radius;
      span = v->axisLength;
    } else if (auto *v = static_cast<const remixapi_LightInfoDistantEXT *>(
                   ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT))) {
      type = v->sType;
      z = vec(v->direction);
      angle = v->angularDiameterDegrees;
    } else if (ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT)) {
      // Dome data is handled below using the same retained light resource.
      type = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    } else
      throw invalid;
    finite(position.data(), 3);
    finite(radiance.data(), 3);
    finite(x.data(), 3);
    finite(y.data(), 3);
    finite(z.data(), 3);
    for (float v : {radius, width, height, span, angle})
      if (!std::isfinite(v) || v < 0)
        throw invalid;
    for (float c : radiance)
      if (c < 0 || c > 1e12f)
        throw invalid;
    LightDefinition definition;
    auto &source = definition.source;
    source.type = type;
    source.radius = radius;
    source.width = width;
    source.height = height;
    source.length = span;
    source.angle = angle * pi / 180;
    source.color = usd && usd->pColor ? *usd->pColor : info.radiance;
    if (usd) {
      source.transform = usd->transform;
      source.color = usd->pColor ? *usd->pColor : remixapi_Float3D{1, 1, 1};
      source.intensity = value(usd->pIntensity, 1);
      source.exposure = value(usd->pExposure, 0);
      source.temperature = value(usd->pColorTemp, 6500);
      source.temperatureEnabled = usd->pEnableColorTemp && *usd->pEnableColorTemp;
    } else {
      for (int c = 0; c < 3; ++c) {
        source.transform.matrix[c][0] = x[c];
        source.transform.matrix[c][1] = y[c];
        source.transform.matrix[c][2] = -z[c];
        source.transform.matrix[c][3] = position[c];
      }
    }
    if (shaped) {
      source.cone = shaping.coneAngleDegrees * pi / 180;
      source.softness = shaping.coneSoftness;
      source.focus = shaping.focusExponent;
    }
    if (auto *dome =
            static_cast<const remixapi_LightInfoDomeEXT *>(ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT))) {
      source.transform = dome->transform;
      if (dome->colorTexture)
        source.texture = dome->colorTexture;
    }
    SphereLight prototype;
    prototype.id = info.hash;
    std::copy(position.begin(), position.end(), prototype.position);
    std::copy(radiance.begin(), radiance.end(), prototype.radiance);
    if (shaped) {
      if (!std::isfinite(shaping.coneAngleDegrees) || shaping.coneAngleDegrees < 0 ||
          shaping.coneAngleDegrees > 180 || !std::isfinite(shaping.coneSoftness) ||
          shaping.coneSoftness < 0 || shaping.coneSoftness > 1 || !std::isfinite(shaping.focusExponent) ||
          shaping.focusExponent < 0)
        throw invalid;
      auto d = unit(vec(shaping.direction));
      std::copy(d.begin(), d.end(), prototype.direction);
      prototype.coneCos = std::cos(shaping.coneAngleDegrees * pi / 180);
      prototype.softness = shaping.coneSoftness;
      prototype.focus = shaping.focusExponent;
    }
    if (auto *ies = static_cast<const remixapi_LightInfoIesProfileEXT *>(
            ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT))) {
      if (!ies->data || !ies->size || type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT ||
          type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT)
        throw invalid;
      prototype.ies = iesProfile({ies->data, size_t(ies->size)});
      // The profile's axis: the shaping's direction, else the light's own.
      if (!shaped) {
        auto d = unit(z);
        std::copy(d.begin(), d.end(), prototype.direction);
      }
    }
    if (auto *cookie = static_cast<const remixapi_LightInfoCookieEXT *>(
            ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT))) {
      if (type != REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT || !std::isfinite(cookie->fovDegrees) ||
          cookie->fovDegrees <= 0 || cookie->fovDegrees >= 180)
        throw invalid;
      Vec axis = unit(shaped ? vec(shaping.direction) : z), up = vec(cookie->up);
      finite(up.data(), 3);
      auto side = cross(axis, up);
      if (length(side) < 1e-4f * std::max(length(up), 1e-30f))
        throw invalid;
      auto texture = texturePath(cookie->texture);
      auto slot = std::find(m_cookieTextures.begin(), m_cookieTextures.end(), texture);
      if (slot == m_cookieTextures.end())
        slot = m_cookieTextures.insert(m_cookieTextures.end(), texture);
      prototype.cookie = uint32_t(slot - m_cookieTextures.begin()) + 1;
      // Up made perpendicular to the axis.
      auto perpendicular = unit(cross(unit(side), axis));
      std::copy(perpendicular.begin(), perpendicular.end(), prototype.cookieUp);
      prototype.cookieTan = std::tan(cookie->fovDegrees * pi / 360);
      std::copy(axis.begin(), axis.end(), prototype.direction);
    }
    auto triangle = [&](Vec a, Vec b, Vec c, Vec direction) {
      if (length(cross(add(b, scale(a, -1)), add(c, scale(a, -1)))) < 1e-10f)
        return;
      if (auto normal = cross(add(b, scale(a, -1)), add(c, scale(a, -1)));
          normal[0] * direction[0] + normal[1] * direction[1] + normal[2] * direction[2] < 0)
        std::swap(b, c);
      auto light = prototype;
      light.triangle = 1;
      uint64_t identity[2] = {info.hash, definition.emitters.size() + 1};
      light.id = hashBytes(identity, sizeof(identity));
      auto center = scale(add(add(a, b), c), 1.f / 3);
      std::copy(center.begin(), center.end(), light.position);
      light.radius = std::max({length(add(a, scale(center, -1))), length(add(b, scale(center, -1))),
                               length(add(c, scale(center, -1)))});
      std::copy(a.begin(), a.end(), light.corners[0]);
      std::copy(b.begin(), b.end(), light.corners[1]);
      std::copy(c.begin(), c.end(), light.corners[2]);
      definition.emitters.push_back(light);
    };
    auto point = [&](float u, float v) { return add(position, add(scale(x, u), scale(y, v))); };
    if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT) {
      if (angle >= 180)
        throw invalid;
      auto d = unit(z);
      definition.distant.isDistant = true;
      definition.distant.angularDiameterDegrees = angle;
      std::copy(d.begin(), d.end(), definition.distant.direction);
      std::copy(radiance.begin(), radiance.end(), definition.distant.radiance);
    } else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT) {
      prototype.radius = radius * std::max({length(x), length(y), length(z)});
      if (prototype.radius > 0)
        definition.emitters.push_back(prototype);
    } else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT) {
      auto a = point(-width / 2, -height / 2), b = point(width / 2, -height / 2),
           c = point(width / 2, height / 2), d = point(-width / 2, height / 2);
      triangle(a, b, c, z);
      triangle(a, c, d, z);
    } else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT) {
      for (unsigned i = 0; i < 64; ++i) {
        float a = 2 * pi * i / 64, b = 2 * pi * (i + 1) / 64;
        triangle(position, point(radius * std::cos(a), radius * std::sin(a)),
                 point(radius * std::cos(b), radius * std::sin(b)), z);
      }
    } else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT) {
      // USD cylinders are centered along local X; lateral surface only.
      for (unsigned i = 0; i < 64; ++i) {
        float a = 2 * pi * i / 64, b = 2 * pi * (i + 1) / 64;
        auto ra = add(scale(y, radius * std::cos(a)), scale(z, radius * std::sin(a))),
             rb = add(scale(y, radius * std::cos(b)), scale(z, radius * std::sin(b)));
        auto aa = add(position, add(ra, scale(x, -span / 2))),
             ab = add(position, add(ra, scale(x, span / 2))),
             ba = add(position, add(rb, scale(x, -span / 2))),
             bb = add(position, add(rb, scale(x, span / 2)));
        triangle(aa, ab, bb, ra);
        triangle(aa, bb, ba, rb);
      }
    } else if (type == REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT) {
      definition.dome.enabled = true;
      definition.dome.radiance = radiance;
      if (auto *dome =
              static_cast<const remixapi_LightInfoDomeEXT *>(ext(REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT))) {
        definition.dome.worldToLocal = inverse(transform(dome->transform));
        if (dome->colorTexture && *dome->colorTexture)
          definition.dome.texture = texturePath(dome->colorTexture);
      } else if (usd)
        definition.dome.worldToLocal = inverse(transform(usd->transform));
    } else
      throw invalid;
    source.direction = shaped ? shaping.direction : remixapi_Float3D{z[0], z[1], z[2]};
    auto direction = unit(vec(source.direction));
    source.direction = {direction[0], direction[1], direction[2]};
    m_lights[info.hash] = std::move(definition);
    handle = reinterpret_cast<remixapi_LightHandle>(uintptr_t(info.hash));
  }
  void NativeResources::updateLight(remixapi_LightHandle handle, const remixapi_LightInfo &info) {
    if (!m_lights.count(key(handle)))
      throw invalid;
    auto replacement = info;
    replacement.hash = key(handle);
    createLight(replacement, handle);
  }
  void NativeResources::destroyLight(remixapi_LightHandle handle) { m_lights.erase(key(handle)); }
  void NativeResources::drawLight(remixapi_LightHandle handle) {
    auto it = m_lights.find(key(handle));
    if (it == m_lights.end())
      throw invalid;
    if (it->second.distant.isDistant)
      m_light = it->second.distant;
    if (it->second.dome.enabled)
      m_dome = it->second.dome;
    if (!it->second.emitters.empty())
      m_frameLights.insert(key(handle));
  }
  std::vector<SphereLight> NativeResources::sphereLights() const {
    if (m_lightsPublished)
      return m_publishedLights;
    std::vector<SphereLight> result;
    for (auto id : m_frameLights)
      if (auto it = m_lights.find(id); it != m_lights.end())
        result.insert(result.end(), it->second.emitters.begin(), it->second.emitters.end());
    return result;
  }
} // namespace remix

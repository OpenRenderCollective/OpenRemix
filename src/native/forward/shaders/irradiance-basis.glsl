#ifndef OPENREMIX_IRRADIANCE_BASIS
#define OPENREMIX_IRRADIANCE_BASIS

// Real SH through band two, in the order used by the bake's irradiance fields.
void irradianceBasis(vec3 normal, out float sh[9]) {
  sh = float[9](.2820947918, .4886025119 * normal.y, .4886025119 * normal.z, .4886025119 * normal.x,
                1.092548431 * normal.x * normal.y, 1.092548431 * normal.y * normal.z,
                .3153915653 * (3 * normal.z * normal.z - 1), 1.092548431 * normal.x * normal.z,
                .5462742153 * (normal.x * normal.x - normal.y * normal.y));
}

#endif

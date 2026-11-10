// Low control bits retain dielectric, importance and aggregate policy flags.
// Component mode uses bit 3 and a selector in bits 8..15; normal callers leave
// both zero and retain the original shared-unstyled transport behavior.
bool lightingComponentsEnabled() { return (rayFrame.control.w & 8u) != 0u; }
bool lightingStyleSelected(uint style) {
  if (!lightingComponentsEnabled())
    return style == 0u;
  uint selected = (rayFrame.control.w >> 8u) & 255u;
  return selected == 128u || selected == style;
}

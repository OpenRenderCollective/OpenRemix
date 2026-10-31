// Apply only after all sampling/visibility weights, before sample averaging.
// Match RTX Remix luminance scaling, measuring outgoing (remodulated) radiance.
// Saturated red and blue count by their channel average where it is higher:
// luminance alone weighs blue at .07, so a pure blue firefly passed at about
// 14 times the threshold.
float fireflyScale(Hit hit,vec3 total,float threshold) {
  if(threshold<=0)return 1;
  vec3 outgoing=total*materialFactor(hit);
  float level=max(dot(outgoing,vec3(.2126,.7152,.0722)),dot(outgoing,vec3(1.0/3.0)));
  return level>threshold?threshold/level:1;
}
void clampLighting(Hit hit,inout vec3 total,inout vec3 specular,float threshold) {
  if(threshold<=0)return;
  float scale=fireflyScale(hit,total,threshold);
  total*=scale;specular*=scale;
}

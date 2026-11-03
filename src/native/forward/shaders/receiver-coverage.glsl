#ifndef OPENREMIX_RECEIVER_COVERAGE
#define OPENREMIX_RECEIVER_COVERAGE
// Pass-owned receiver coverage. These flags also apply to replacement lighting
// sources that do not use the default lightmap/probe storage.
layout(constant_id = 3) const bool FullyMappedReceiver = false;
// The ordinary vertex layout always supplies the invalid chart coordinate.
layout(constant_id = 4) const bool UnmappedReceiver = false;
#endif

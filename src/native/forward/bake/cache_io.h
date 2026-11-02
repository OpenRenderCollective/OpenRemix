#pragma once
#include "forward/cache_io.h"

// Existing bake/cache users share the same bounded stream implementation.
namespace remix::forward::bake::cache::detail {
  using cache_io::Header;
  using cache_io::Reader;
  using cache_io::Writer;
  using cache_io::check;
  using cache_io::maximumFileBytes;
}

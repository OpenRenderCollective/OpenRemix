/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "api/state.h"
#include "api/entrypoints.h"
#include <cstring>

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetGameValue(const char *key, const char *value) {
  if (key == nullptr || key[0] == '\0' || value == nullptr) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  std::lock_guard lock(g_apiMutex);
  try {
    g_gameValues[key] = value;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
  return REMIXAPI_ERROR_CODE_SUCCESS;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGameValue(const char *key, char *outBuffer, uint32_t bufferSize,
                                                                uint32_t *outActualSize) {
  if (key == nullptr || key[0] == '\0' || outActualSize == nullptr || (bufferSize != 0 && outBuffer == nullptr)) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }

  *outActualSize = 0;
  if (outBuffer && bufferSize) {
    outBuffer[0] = '\0';
  }
  std::lock_guard lock(g_apiMutex);
  const auto found = g_gameValues.find(key);
  if (found == g_gameValues.end()) {
    *outActualSize = 0;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  const auto &value = found->second;
  const uint32_t requiredSize = static_cast<uint32_t>(value.size()) + 1;
  *outActualSize = requiredSize;
  if (bufferSize >= requiredSize) {
    std::memcpy(outBuffer, value.c_str(), requiredSize);
  }
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}

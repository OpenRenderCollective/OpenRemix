#include "runtime/jxr_writer.h"
#include <cstdio>
#include <limits>

// jxrlib's ANSI build (third_party/jxrlib/meson.build). On Windows the SDK's
// sal.h hides wmsal.h, which defines these two annotations.
#define __ANSI__
#ifdef _WIN32
#define __in_win
#define __out_win
#endif
#include <JXRGlue.h>
#undef min
#undef max

namespace remix {
  namespace {
    // A WMPStream over a FILE* opened with a wide path on Windows. Close only
    // closes; the owner frees it.
    struct FileStream {
      WMPStream stream{};
      FILE *file = nullptr;
      bool failed = false;
    };

    FileStream *fileStream(WMPStream *stream) { return reinterpret_cast<FileStream *>(stream); }

    ERR closeFile(WMPStream **stream) {
      auto *s = fileStream(*stream);
      if (s->file && std::fclose(s->file) != 0) s->failed = true;
      s->file = nullptr;
      return WMP_errSuccess;
    }

    Bool eosFile(WMPStream *stream) { return std::feof(fileStream(stream)->file); }

    ERR readFile(WMPStream *stream, void *data, size_t size) {
      return std::fread(data, 1, size, fileStream(stream)->file) == size ? WMP_errSuccess : WMP_errFileIO;
    }

    ERR writeFile(WMPStream *stream, const void *data, size_t size) {
      auto *s = fileStream(stream);
      if (size && std::fwrite(data, 1, size, s->file) != size) {
        s->failed = true;
        return WMP_errFileIO;
      }
      return WMP_errSuccess;
    }

    ERR setPosFile(WMPStream *stream, size_t offset) {
#ifdef _WIN32
      const int r = _fseeki64(fileStream(stream)->file, static_cast<int64_t>(offset), SEEK_SET);
#else
      const int r = fseeko(fileStream(stream)->file, static_cast<off_t>(offset), SEEK_SET);
#endif
      return r == 0 ? WMP_errSuccess : WMP_errFileIO;
    }

    ERR getPosFile(WMPStream *stream, size_t *offset) {
#ifdef _WIN32
      const int64_t p = _ftelli64(fileStream(stream)->file);
#else
      const off_t p = ftello(fileStream(stream)->file);
#endif
      if (p < 0) return WMP_errFileIO;
      *offset = static_cast<size_t>(p);
      return WMP_errSuccess;
    }
  } // namespace

  bool writeJxrScRgb(const std::filesystem::path &path, uint32_t width, uint32_t height,
    const uint16_t *rgbaHalf, std::string &error) {
    constexpr uint64_t stride = 8; // bytes per RGBA half pixel
    if (!width || !height || !rgbaHalf || width > uint32_t(std::numeric_limits<I32>::max()) ||
        height > uint32_t(std::numeric_limits<I32>::max()) || uint64_t(width) * stride > UINT32_MAX) {
      error = "invalid JPEG XR image size";
      return false;
    }
    FileStream file;
#ifdef _WIN32
    file.file = _wfopen(path.c_str(), L"wb");
#else
    file.file = std::fopen(path.c_str(), "wb");
#endif
    if (!file.file) {
      error = "cannot create file";
      return false;
    }
    file.stream.Close = closeFile;
    file.stream.EOS = eosFile;
    file.stream.Read = readFile;
    file.stream.Write = writeFile;
    file.stream.SetPos = setPosFile;
    file.stream.GetPos = getPosFile;

    PKImageEncode *encoder = nullptr;
    ERR err = PKImageEncode_Create_WMP(&encoder);
    if (err >= 0) {
      // Lossless (QP index 1): 4:4:4, one overlap level, alpha as its own plane.
      CWMIStrCodecParam params{};
      params.bVerbose = FALSE;
      params.cfColorFormat = YUV_444;
      params.bdBitDepth = BD_LONG;
      params.bfBitstreamFormat = FREQUENCY;
      params.olOverlap = OL_ONE;
      params.sbSubband = SB_ALL;
      params.uAlphaMode = 2;
      params.uiDefaultQPIndex = 1;
      params.uiDefaultQPIndexAlpha = 1;
      err = encoder->Initialize(encoder, &file.stream, &params, sizeof(params));
    }
    if (err >= 0) err = encoder->SetPixelFormat(encoder, GUID_PKPixelFormat64bppRGBAHalf);
    if (err >= 0) err = encoder->SetSize(encoder, I32(width), I32(height));
    if (err >= 0) err = encoder->SetResolution(encoder, 96.f, 96.f);
    // The encoder only reads the pixels.
    if (err >= 0)
      err = encoder->WritePixels(encoder, height,
        reinterpret_cast<U8 *>(const_cast<uint16_t *>(rgbaHalf)), U32(width * stride));
    if (encoder) {
      // Release closes the stream.
      encoder->pStream = &file.stream;
      encoder->Release(&encoder);
    } else {
      WMPStream *stream = &file.stream;
      closeFile(&stream);
    }
    if (err < 0 || file.failed) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      error = err < 0 ? "JPEG XR encoding failed (" + std::to_string(err) + ")" : "cannot write file";
      return false;
    }
    return true;
  }
} // namespace remix

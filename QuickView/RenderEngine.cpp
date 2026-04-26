#include "pch.h"
#include <initguid.h>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include "RenderEngine.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "RenderEngine";
#include "DebugMetrics.h"
#include "EditState.h"
#include "ImageTypes.h" // [Direct D2D] RawImageFrame
#include "ImageLoaderSimd.h"


// 核心修复：引入 DirectX GUID 定义库 与 必要库
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mscms.lib")
#include "resource.h"
#include <icm.h>
#include <lcms2.h>


extern AppConfig g_config;

struct CRenderEngine::GamutProgram {
  struct AnalyticData {
    std::array<float, 9> srcToXyz = {};
    std::array<float, 9> xyzToDst = {};
    std::vector<float> trcR;
    std::vector<float> trcG;
    std::vector<float> trcB;
    ComPtr<ID3D11ShaderResourceView> trcSrvR;
    ComPtr<ID3D11ShaderResourceView> trcSrvG;
    ComPtr<ID3D11ShaderResourceView> trcSrvB;
  };

  struct LutData {
    int edge = 0;
    std::vector<uint8_t> overflowLut;
    ComPtr<ID3D11Texture3D> overflowTexture;
    ComPtr<ID3D11ShaderResourceView> overflowSrv;
  };

  GamutBackendKind backend = GamutBackendKind::Unknown;
  std::wstring srcName;
  std::wstring dstName;
  AnalyticData analytic;
  LutData lut;
};

namespace {
template <typename T>
bool LoadIccFromResource(T *d2dContext, int resourceId,
                         ID2D1ColorContext **dstContext) {
  HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr),
                             MAKEINTRESOURCEW(resourceId), L"ICC");
  if (!hRes)
    return false;
  HGLOBAL hMem = LoadResource(GetModuleHandleW(nullptr), hRes);
  if (!hMem)
    return false;
  DWORD size = SizeofResource(GetModuleHandleW(nullptr), hRes);
  void *data = LockResource(hMem);
  if (!data)
    return false;
  return SUCCEEDED(d2dContext->CreateColorContext(
      D2D1_COLOR_SPACE_CUSTOM, static_cast<const BYTE *>(data), size,
      dstContext));
}

float ToneMapAces(float value) {
  value = (value > 0.0f) ? value : 0.0f;
  const float numerator = value * (2.51f * value + 0.03f);
  const float denominator = value * (2.43f * value + 0.59f) + 0.14f;
  if (denominator <= 0.0f)
    return 0.0f;
  float res = numerator / denominator;
  return (res < 0.0f) ? 0.0f : (res > 1.0f ? 1.0f : res);
}

uint8_t EncodeLinearToSdr8(float value) {
  value = powf((value > 0.0f ? value : 0.0f), 1.0f / 2.2f);
  value = (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

struct ColorMatrix3x3 {
  float m[3][3];
};

struct ScopedColorProfile {
  HPROFILE handle = nullptr;

  ~ScopedColorProfile() {
    if (handle) {
      CloseColorProfile(handle);
    }
  }

  ScopedColorProfile() = default;
  ScopedColorProfile(const ScopedColorProfile &) = delete;
  ScopedColorProfile &operator=(const ScopedColorProfile &) = delete;

  ScopedColorProfile(ScopedColorProfile &&other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
  }

  ScopedColorProfile &operator=(ScopedColorProfile &&other) noexcept {
    if (this != &other) {
      if (handle) {
        CloseColorProfile(handle);
      }
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  void Reset(HPROFILE nextHandle = nullptr) {
    if (handle) {
      CloseColorProfile(handle);
    }
    handle = nextHandle;
  }
};

struct ScopedColorTransform {
  HTRANSFORM handle = nullptr;

  ~ScopedColorTransform() {
    if (handle) {
      DeleteColorTransform(handle);
    }
  }

  ScopedColorTransform() = default;
  ScopedColorTransform(const ScopedColorTransform &) = delete;
  ScopedColorTransform &operator=(const ScopedColorTransform &) = delete;

  void Reset(HTRANSFORM nextHandle = nullptr) {
    if (handle) {
      DeleteColorTransform(handle);
    }
    handle = nextHandle;
  }
};

bool LoadIccBytesFromResource(int resourceId, std::vector<uint8_t> *outBytes) {
  if (!outBytes)
    return false;
  outBytes->clear();

  HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr),
                             MAKEINTRESOURCEW(resourceId), L"ICC");
  if (!hRes)
    return false;
  HGLOBAL hMem = LoadResource(GetModuleHandleW(nullptr), hRes);
  if (!hMem)
    return false;
  DWORD size = SizeofResource(GetModuleHandleW(nullptr), hRes);
  void *data = LockResource(hMem);
  if (!data || size == 0)
    return false;

  const auto *bytes = static_cast<const uint8_t *>(data);
  outBytes->assign(bytes, bytes + size);
  return true;
}

bool BuildSrgbProfileBytes(std::vector<uint8_t> *outBytes) {
  if (!outBytes)
    return false;
  outBytes->clear();

  DWORD pathLen = 0;
  if (!GetStandardColorSpaceProfileW(nullptr, LCS_sRGB, nullptr, &pathLen) ||
      pathLen == 0) {
    return false;
  }

  std::wstring profilePath(pathLen, L'\0');
  if (!GetStandardColorSpaceProfileW(nullptr, LCS_sRGB, profilePath.data(),
                                     &pathLen) ||
      pathLen == 0) {
    return false;
  }
  profilePath.resize(wcsnlen(profilePath.c_str(), pathLen));

  std::ifstream stream(profilePath, std::ios::binary);
  if (!stream) {
    return false;
  }

  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size <= 0) {
    return false;
  }

  outBytes->resize(static_cast<size_t>(size));
  stream.seekg(0, std::ios::beg);
  stream.read(reinterpret_cast<char *>(outBytes->data()), size);
  return stream.good() || stream.eof();
}

bool TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries primaries,
                                     std::vector<uint8_t> *outBytes);

size_t HashBytes(size_t seed, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t hash = seed ? seed : 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<size_t>(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

size_t HashWString(size_t seed, const std::wstring& value) {
  return HashBytes(seed, value.data(), value.size() * sizeof(wchar_t));
}

std::wstring ToLowerCopy(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), towlower);
  return value;
}

bool IsGenericSrgbProfilePath(const std::wstring& path) {
  const std::wstring lower = ToLowerCopy(path);
  return lower.find(L"srgb color space profile.icm") != std::wstring::npos;
}

std::wstring DescribeLcmsProfile(cmsHPROFILE profile, const wchar_t* fallback) {
  if (!profile) {
    return fallback ? fallback : L"Unknown";
  }
  char buffer[256] = {};
  if (cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US",
                             buffer, static_cast<cmsUInt32Number>(sizeof(buffer))) > 0 &&
      buffer[0] != '\0') {
    const int size = MultiByteToWideChar(CP_UTF8, 0, buffer, -1, nullptr, 0);
    if (size > 1) {
      std::wstring wide(size - 1, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, buffer, -1, wide.data(), size);
      return wide;
    }
  }
  return fallback ? fallback : L"Unknown";
}

struct ScopedCmsProfile {
  cmsHPROFILE handle = nullptr;
  ~ScopedCmsProfile() {
    if (handle) cmsCloseProfile(handle);
  }
  void Reset(cmsHPROFILE next = nullptr) {
    if (handle) cmsCloseProfile(handle);
    handle = next;
  }
};

struct ScopedCmsTransform {
  cmsHTRANSFORM handle = nullptr;
  ~ScopedCmsTransform() {
    if (handle) cmsDeleteTransform(handle);
  }
  void Reset(cmsHTRANSFORM next = nullptr) {
    if (handle) cmsDeleteTransform(handle);
    handle = next;
  }
};

struct ProfileBlob {
  std::vector<uint8_t> bytes;
  std::wstring name;
};

struct Matrix3 {
  float m[3][3];
};

bool InvertMatrix3(const Matrix3& in, Matrix3* out) {
  if (!out) return false;
  const float a = in.m[0][0], b = in.m[0][1], c = in.m[0][2];
  const float d = in.m[1][0], e = in.m[1][1], f = in.m[1][2];
  const float g = in.m[2][0], h = in.m[2][1], i = in.m[2][2];
  const float A = e * i - f * h;
  const float B = -(d * i - f * g);
  const float C = d * h - e * g;
  const float D = -(b * i - c * h);
  const float E = a * i - c * g;
  const float F = -(a * h - b * g);
  const float G = b * f - c * e;
  const float H = -(a * f - c * d);
  const float I = a * e - b * d;
  const float det = a * A + b * B + c * C;
  if (fabsf(det) < 1e-8f) return false;
  const float inv = 1.0f / det;
  *out = {{{A * inv, D * inv, G * inv},
           {B * inv, E * inv, H * inv},
           {C * inv, F * inv, I * inv}}};
  return true;
}

bool BuildSyntheticDisplayProfile(const QuickView::DisplayColorState& displayState,
                                  ProfileBlob* outBlob) {
  if (!outBlob || !displayState.HasChromaticities()) return false;
  cmsToneCurve* curves[3] = {
      cmsBuildGamma(nullptr, 2.2),
      cmsBuildGamma(nullptr, 2.2),
      cmsBuildGamma(nullptr, 2.2),
  };
  if (!curves[0] || !curves[1] || !curves[2]) {
    cmsFreeToneCurveTriple(curves);
    return false;
  }

  cmsCIExyY white = { displayState.whitePoint[0], displayState.whitePoint[1], 1.0 };
  cmsCIExyYTRIPLE primaries = {};
  primaries.Red.x = displayState.redPrimary[0];
  primaries.Red.y = displayState.redPrimary[1];
  primaries.Red.Y = 1.0;
  primaries.Green.x = displayState.greenPrimary[0];
  primaries.Green.y = displayState.greenPrimary[1];
  primaries.Green.Y = 1.0;
  primaries.Blue.x = displayState.bluePrimary[0];
  primaries.Blue.y = displayState.bluePrimary[1];
  primaries.Blue.Y = 1.0;

  ScopedCmsProfile profile;
  profile.Reset(cmsCreateRGBProfile(&white, &primaries, curves));
  cmsFreeToneCurveTriple(curves);
  if (!profile.handle) return false;

  cmsUInt32Number bytesNeeded = 0;
  if (!cmsSaveProfileToMem(profile.handle, nullptr, &bytesNeeded) || bytesNeeded == 0) {
    return false;
  }
  outBlob->bytes.resize(bytesNeeded);
  if (!cmsSaveProfileToMem(profile.handle, outBlob->bytes.data(), &bytesNeeded)) {
    outBlob->bytes.clear();
    return false;
  }
  outBlob->name = L"Synthetic Display RGB";
  return true;
}

bool BuildProfileBlobForPrimaries(QuickView::ColorPrimaries primaries, ProfileBlob* outBlob) {
  if (!outBlob) return false;
  outBlob->bytes.clear();
  outBlob->name.clear();
  if (!TryLoadProfileBytesForPrimaries(primaries, &outBlob->bytes)) {
    return false;
  }
  switch (primaries) {
  case QuickView::ColorPrimaries::DisplayP3:
    outBlob->name = L"Display P3";
    break;
  case QuickView::ColorPrimaries::AdobeRGB:
    outBlob->name = L"Adobe RGB";
    break;
  case QuickView::ColorPrimaries::ProPhotoRGB:
    outBlob->name = L"ProPhoto RGB";
    break;
  case QuickView::ColorPrimaries::SRGB:
  case QuickView::ColorPrimaries::Unknown:
  default:
    outBlob->name = L"sRGB";
    break;
  }
  return !outBlob->bytes.empty();
}

bool TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries primaries,
                                     std::vector<uint8_t> *outBytes) {
  if (!outBytes)
    return false;

  switch (primaries) {
  case QuickView::ColorPrimaries::SRGB:
  case QuickView::ColorPrimaries::Unknown:
    return BuildSrgbProfileBytes(outBytes);
  case QuickView::ColorPrimaries::DisplayP3:
    return LoadIccBytesFromResource(IDR_ICC_P3, outBytes);
  case QuickView::ColorPrimaries::AdobeRGB:
    return LoadIccBytesFromResource(IDR_ICC_ADOBERGB, outBytes);
  case QuickView::ColorPrimaries::ProPhotoRGB:
    return LoadIccBytesFromResource(IDR_ICC_PROPHOTO, outBytes);
  default:
    return false;
  }
}

HPROFILE OpenProfileFromBytes(const void *data, DWORD size) {
  if (!data || size == 0)
    return nullptr;

  PROFILE profile = {};
  profile.dwType = PROFILE_MEMBUFFER;
  profile.pProfileData = const_cast<void *>(data);
  profile.cbDataSize = size;
  return OpenColorProfileW(&profile, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
}

HPROFILE OpenProfileFromPath(const std::wstring &path) {
  if (path.empty())
    return nullptr;

  PROFILE profile = {};
  profile.dwType = PROFILE_FILENAME;
  profile.pProfileData = const_cast<wchar_t *>(path.c_str());
  profile.cbDataSize =
      static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
  return OpenColorProfileW(&profile, PROFILE_READ, FILE_SHARE_READ, OPEN_EXISTING);
}

bool TryGetMonitorProfilePath(const QuickView::DisplayColorState &displayState,
                              std::wstring *outPath) {
  if (!outPath)
    return false;
  outPath->clear();

  if (displayState.gdiDeviceName.empty())
    return false;

  HDC hdcMon =
      CreateDCW(L"DISPLAY", displayState.gdiDeviceName.c_str(), NULL, NULL);
  if (!hdcMon)
    return false;

  DWORD dwLen = 0;
  GetICMProfileW(hdcMon, &dwLen, NULL);
  if (dwLen == 0) {
    DeleteDC(hdcMon);
    return false;
  }

  std::wstring profilePath(dwLen, L'\0');
  const BOOL ok = GetICMProfileW(hdcMon, &dwLen, &profilePath[0]);
  DeleteDC(hdcMon);
  if (!ok || dwLen == 0)
    return false;

  profilePath.resize(dwLen - 1);
  *outPath = std::move(profilePath);
  return true;
}

QuickView::ColorPrimaries ResolveFrameProfilePrimaries(
    const QuickView::RawImageFrame &frame) {
  if (frame.colorInfo.primaries != QuickView::ColorPrimaries::Unknown) {
    return frame.colorInfo.primaries;
  }
  return frame.hdrMetadata.primaries;
}

bool BuildGamutCheckSampleFrame(const QuickView::RawImageFrame &frame,
                                int maxDimension,
                                QuickView::RawImageFrame *outSample,
                                int *outCols, int *outRows) {
  if (!outSample || !outCols || !outRows || !frame.IsValid() || !frame.pixels ||
      frame.width <= 0 || frame.height <= 0) {
    return false;
  }

  if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT ||
      frame.format == QuickView::PixelFormat::SVG_XML) {
    return false;
  }

  outSample->Release();
  outSample->format = QuickView::PixelFormat::RGBA8888;
  const int safeMaxDim = std::clamp(maxDimension, 48, 1024);
  const float scale =
      static_cast<float>(safeMaxDim) /
      static_cast<float>((std::max)(frame.width, frame.height));
  outSample->width =
      (frame.width > safeMaxDim) ? (std::max)(48, static_cast<int>(frame.width * scale)) : frame.width;
  outSample->height =
      (frame.height > safeMaxDim) ? (std::max)(48, static_cast<int>(frame.height * scale)) : frame.height;
  outSample->stride = outSample->width * 4;
  outSample->iccProfile = frame.iccProfile;
  outSample->colorInfo = frame.colorInfo;
  outSample->hdrMetadata = frame.hdrMetadata;
  outSample->pixels =
      static_cast<uint8_t *>(malloc(static_cast<size_t>(outSample->stride) *
                                    static_cast<size_t>(outSample->height)));
  outSample->memoryDeleter = [](uint8_t *p) { free(p); };
  if (!outSample->pixels) {
    outSample->Release();
    return false;
  }

  for (int row = 0; row < outSample->height; ++row) {
    uint8_t *dstRow =
        outSample->pixels + static_cast<size_t>(row) * outSample->stride;
    const float v = (static_cast<float>(row) + 0.5f) /
                    static_cast<float>(outSample->height);
    const int sy = std::clamp(
        static_cast<int>(v * static_cast<float>(frame.height - 1)), 0,
        frame.height - 1);
    const uint8_t *srcRow =
        frame.pixels + static_cast<size_t>(sy) * frame.stride;

    for (int col = 0; col < outSample->width; ++col) {
      const float u = (static_cast<float>(col) + 0.5f) /
                      static_cast<float>(outSample->width);
      const int sx = std::clamp(
          static_cast<int>(u * static_cast<float>(frame.width - 1)), 0,
          frame.width - 1);

      const uint8_t *src = srcRow + static_cast<size_t>(sx) * 4;
      uint8_t *dst = dstRow + static_cast<size_t>(col) * 4;
      switch (frame.format) {
      case QuickView::PixelFormat::BGRA8888:
      case QuickView::PixelFormat::BGRX8888:
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = 255;
        break;
      case QuickView::PixelFormat::RGBA8888:
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        break;
      default:
        outSample->Release();
        return false;
      }
    }
  }

  *outCols = outSample->width;
  *outRows = outSample->height;
  return true;
}

void DilateBinaryMask(std::vector<uint8_t> &mask, int cols, int rows) {
  if (mask.empty() || cols <= 0 || rows <= 0)
    return;

  std::vector<uint8_t> expanded = mask;
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      if (!mask[static_cast<size_t>(row * cols + col)])
        continue;
      for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
          const int nx = col + ox;
          const int ny = row + oy;
          if (nx >= 0 && nx < cols && ny >= 0 && ny < rows) {
            expanded[static_cast<size_t>(ny * cols + nx)] = 255;
          }
        }
      }
    }
  }
  mask.swap(expanded);
}

ColorMatrix3x3 MakeIdentityMatrix() {
  return {{{1.0f, 0.0f, 0.0f},
           {0.0f, 1.0f, 0.0f},
           {0.0f, 0.0f, 1.0f}}};
}

bool TryGetLinearPrimariesToScRgbMatrix(QuickView::ColorPrimaries primaries,
                                        ColorMatrix3x3 *outMatrix) {
  if (!outMatrix)
    return false;

  switch (primaries) {
  case QuickView::ColorPrimaries::Unknown:
  case QuickView::ColorPrimaries::SRGB:
    *outMatrix = MakeIdentityMatrix();
    return true;
  case QuickView::ColorPrimaries::DisplayP3:
    *outMatrix = {{{1.2249401f, -0.2249404f, 0.0f},
                   {-0.0420569f, 1.0420569f, 0.0f},
                   {-0.0196376f, -0.0786361f, 1.0982735f}}};
    return true;
  case QuickView::ColorPrimaries::Rec2020:
    *outMatrix = {{{1.6605f, -0.5876f, -0.0728f},
                   {-0.1246f, 1.1329f, -0.0083f},
                   {-0.0182f, -0.1006f, 1.1187f}}};
    return true;
  default:
    return false;
  }
}

void TransformLinearPixelToScRgb(const ColorMatrix3x3 &matrix, float &r, float &g,
                                 float &b) {
  const float rr = matrix.m[0][0] * r + matrix.m[0][1] * g + matrix.m[0][2] * b;
  const float gg = matrix.m[1][0] * r + matrix.m[1][1] * g + matrix.m[1][2] * b;
  const float bb = matrix.m[2][0] * r + matrix.m[2][1] * g + matrix.m[2][2] * b;
  r = rr;
  g = gg;
  b = bb;
}

bool BuildLinearScRgbFloatBuffer(const QuickView::RawImageFrame &frame,
                                 std::vector<uint8_t> &convertedPixels,
                                 const uint8_t **pixelsOut,
                                 UINT *strideOut) {
  if (!pixelsOut || !strideOut ||
      frame.format != QuickView::PixelFormat::R32G32B32A32_FLOAT ||
      !frame.pixels || frame.width <= 0 || frame.height <= 0) {
    return false;
  }

  *pixelsOut = frame.pixels;
  *strideOut = static_cast<UINT>(frame.stride);

  const QuickView::ColorPrimaries primaries =
      frame.colorInfo.primaries != QuickView::ColorPrimaries::Unknown
          ? frame.colorInfo.primaries
          : frame.hdrMetadata.primaries;

  ColorMatrix3x3 matrix = {};
  if (!TryGetLinearPrimariesToScRgbMatrix(primaries, &matrix)) {
    return false;
  }

  const bool needsConversion =
      primaries == QuickView::ColorPrimaries::DisplayP3 ||
      primaries == QuickView::ColorPrimaries::Rec2020;
  if (!needsConversion) {
    return true;
  }

  convertedPixels.resize(static_cast<size_t>(frame.stride) * frame.height);
  memcpy(convertedPixels.data(), frame.pixels,
         static_cast<size_t>(frame.stride) * frame.height);

  // [Highway] Use ImageLoaderSimd for color matrix transformation
  const float matrixArr[9] = {
      matrix.m[0][0], matrix.m[0][1], matrix.m[0][2],
      matrix.m[1][0], matrix.m[1][1], matrix.m[1][2],
      matrix.m[2][0], matrix.m[2][1], matrix.m[2][2],
  };
  ImageLoaderSimd::TransformColorMatrix3x3(
      reinterpret_cast<float*>(convertedPixels.data()),
      frame.width, frame.height, frame.stride, matrixArr);

  *pixelsOut = convertedPixels.data();
  *strideOut = static_cast<UINT>(frame.stride);
  return true;
}

HRESULT CreateScRgbColorContext(ID2D1DeviceContext *dc,
                                ID2D1ColorContext **outContext) {
  if (!dc || !outContext)
    return E_INVALIDARG;
  return dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, outContext);
}

bool IsSceneLinearFrame(const QuickView::RawImageFrame &frame) {
  return frame.colorInfo.IsSceneLinear();
}

bool IsHdrLikeFrame(const QuickView::RawImageFrame &frame) {
  return frame.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr ||
         frame.colorInfo.IsSceneLinear() || frame.hdrMetadata.hasGainMap;
}

float InternalEstimateFramePeakScRgb(const QuickView::RawImageFrame &frame) {
  if (frame.format != QuickView::PixelFormat::R32G32B32A32_FLOAT ||
      !frame.pixels || frame.width <= 0 || frame.height <= 0) {
    return 1.0f;
  }

  // [Universe's Strongest] Blazing fast SIMD full image scan.
  // Replaces 64x64 sampling with 100% accurate peak retrieval using AVX2/AVX512.
  return ImageLoaderSimd::FindPeakFloat(
      reinterpret_cast<const float *>(frame.pixels),
      static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height));
}

QuickView::ToneMapSettings
BuildToneMapSettings(const QuickView::RawImageFrame &frame,
                     const QuickView::DisplayColorState &displayState) {
  QuickView::ToneMapSettings settings = {};

  const float paperWhiteScRgb =
      (displayState.GetSdrWhiteScale() > 1.0f ? displayState.GetSdrWhiteScale() : 1.0f);
  float peakNits = displayState.GetEffectivePeakNits(g_config.HdrPeakNitsOverride);
      
  const float displayPeakScRgb = (peakNits / 80.0f > 1.0f ? peakNits / 80.0f : 1.0f);

  float contentPeakScRgb = 1.0f;
  float contentAverageScRgb = 0.0f;
  const char* contentPeakSource = "Default_1.0";
  if (frame.hdrMetadata.hasNitsMetadata) {
    if (frame.hdrMetadata.maxCLLNits > 0.0f) {
      contentPeakScRgb = frame.hdrMetadata.maxCLLNits / 80.0f;
      contentPeakSource = "Metadata_MaxCLL";
    } else if (frame.hdrMetadata.masteringMaxNits > 0.0f) {
      contentPeakScRgb = frame.hdrMetadata.masteringMaxNits / 80.0f;
      contentPeakSource = "Metadata_MasteringMax";
    }

    if (frame.hdrMetadata.maxFALLNits > 0.0f) {
      contentAverageScRgb = frame.hdrMetadata.maxFALLNits / 80.0f;
    }
  }

  if (contentPeakScRgb <= 1.0f) {
    contentPeakScRgb = InternalEstimateFramePeakScRgb(frame);
    if (contentPeakScRgb > 1.0f) contentPeakSource = "Estimated_PixelScan";
  }

  if (contentPeakScRgb <= 1.0f && IsHdrLikeFrame(frame)) {
    const QuickView::TransferFunction transfer =
        frame.colorInfo.transfer != QuickView::TransferFunction::Unknown
            ? frame.colorInfo.transfer
            : frame.hdrMetadata.transfer;
    switch (transfer) {
    case QuickView::TransferFunction::PQ:
    case QuickView::TransferFunction::HLG:
      contentPeakScRgb = 12.5f; // 1000 nits reference fallback
      contentPeakSource = "Fallback_PQ_HLG_1000nits";
      break;
    case QuickView::TransferFunction::Linear:
      contentPeakScRgb = 4.0f;
      contentPeakSource = "Fallback_Linear_320nits";
      break;
    default:
      contentPeakScRgb = 2.0f;
      contentPeakSource = "Fallback_Generic_160nits";
      break;
    }
  }

  QV_LOG("Render_ContentPeakSource",
      TraceLoggingString(contentPeakSource, "Source"),
      TraceLoggingFloat32(contentPeakScRgb, "ContentPeakScRgb"),
      TraceLoggingFloat32(contentPeakScRgb * 80.0f, "ContentPeakNits"),
      TraceLoggingFloat32(frame.hdrMetadata.maxCLLNits, "RawMaxCLL"),
      TraceLoggingFloat32(frame.hdrMetadata.masteringMaxNits, "RawMasteringMax"));

  settings.contentPeakScRgb = (contentPeakScRgb > 1.0f ? contentPeakScRgb : 1.0f);
  settings.displayPeakScRgb = displayPeakScRgb;
  settings.paperWhiteScRgb = paperWhiteScRgb;
  settings.toneMappingMode = g_config.HdrToneMappingMode;

  const float headroom = settings.displayPeakScRgb / settings.paperWhiteScRgb;
  settings.exposure = 1.0f;
  if (frame.hdrMetadata.hasGainMap) {
    settings.exposure = 1.0f;
  }

  if (frame.hdrMetadata.gainMapApplied) {
    const float displayHeadroom = displayState.GetHdrHeadroomStops();
    const float appliedHeadroom = frame.hdrMetadata.gainMapAppliedHeadroom;
    if (appliedHeadroom > 0.0f) {
      if (displayHeadroom + 0.05f < appliedHeadroom) {
        const float mismatch = appliedHeadroom - displayHeadroom;
        settings.exposure /= (1.0f + mismatch * 0.22f);
      } else if (displayHeadroom > appliedHeadroom + 0.25f &&
                 displayState.advancedColorActive) {
        const float recovery =
            (displayHeadroom - appliedHeadroom < 1.5f ? displayHeadroom - appliedHeadroom : 1.5f);
        settings.exposure *= (1.0f + recovery * 0.08f);
      }
    }
  } else if (frame.hdrMetadata.hasGainMap &&
             frame.hdrMetadata.gainMapAlternateHeadroom > 0.0f) {
    const float displayHeadroom = displayState.GetHdrHeadroomStops();
    if (displayHeadroom + 0.1f < frame.hdrMetadata.gainMapAlternateHeadroom) {
      const float mismatch =
          frame.hdrMetadata.gainMapAlternateHeadroom - displayHeadroom;
      settings.exposure /= (1.0f + mismatch * 0.16f);
    }
  }

  settings.exposure = (settings.exposure < 0.18f) ? 0.18f : ((settings.exposure > 1.0f) ? 1.0f : settings.exposure);

  return settings;
}

bool OpenCmsProfileFromBlob(const ProfileBlob& blob, ScopedCmsProfile* outProfile) {
  if (!outProfile || blob.bytes.empty()) return false;
  outProfile->Reset(cmsOpenProfileFromMem(blob.bytes.data(),
                                          static_cast<cmsUInt32Number>(blob.bytes.size())));
  return outProfile->handle != nullptr;
}

bool ResolveSourceProfileBlob(const QuickView::RawImageFrame& frame,
                              int effectiveCmsMode,
                              ProfileBlob* outBlob) {
  if (!outBlob) return false;
  outBlob->bytes.clear();
  outBlob->name.clear();

  if (effectiveCmsMode == 2) {
    return BuildProfileBlobForPrimaries(QuickView::ColorPrimaries::SRGB, outBlob);
  }
  if (effectiveCmsMode == 3) {
    return BuildProfileBlobForPrimaries(QuickView::ColorPrimaries::DisplayP3, outBlob);
  }
  if (effectiveCmsMode == 4) {
    return BuildProfileBlobForPrimaries(QuickView::ColorPrimaries::AdobeRGB, outBlob);
  }
  if (effectiveCmsMode == 6) {
    return BuildProfileBlobForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB, outBlob);
  }

  if (!frame.iccProfile.empty()) {
    outBlob->bytes = frame.iccProfile;
    outBlob->name = L"Embedded ICC";
    return true;
  }

  return BuildProfileBlobForPrimaries(ResolveFrameProfilePrimaries(frame), outBlob);
}

bool ResolveTargetProfileBlob(const CRenderEngine::GamutWarningAnalysisOptions& options,
                              ProfileBlob* outBlob) {
  if (!outBlob) return false;
  outBlob->bytes.clear();
  outBlob->name.clear();

  if (options.targetKind == CRenderEngine::GamutTargetKind::ProofTarget &&
      options.enableSoftProofing && !options.softProofProfilePath.empty()) {
    std::ifstream stream(options.softProofProfilePath, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size <= 0) return false;
    outBlob->bytes.resize(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(outBlob->bytes.data()), size);
    outBlob->name = options.softProofProfilePath.substr(
        options.softProofProfilePath.find_last_of(L"\\/") == std::wstring::npos
            ? 0
            : options.softProofProfilePath.find_last_of(L"\\/") + 1);
    return !outBlob->bytes.empty();
  }

  std::wstring monitorProfilePath;
  const bool preferSynthetic =
      options.acmAware ||
      options.displayProfilePolicy == CRenderEngine::DisplayProfilePolicy::SyntheticOnly ||
      options.displayProfilePolicy == CRenderEngine::DisplayProfilePolicy::PreferSyntheticWideGamut;
  if (!preferSynthetic &&
      TryGetMonitorProfilePath(options.displayState, &monitorProfilePath) &&
      !monitorProfilePath.empty() &&
      !IsGenericSrgbProfilePath(monitorProfilePath)) {
    std::ifstream stream(monitorProfilePath, std::ios::binary);
    if (stream) {
      stream.seekg(0, std::ios::end);
      const std::streamoff size = stream.tellg();
      if (size > 0) {
        outBlob->bytes.resize(static_cast<size_t>(size));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(outBlob->bytes.data()), size);
        outBlob->name = monitorProfilePath.substr(
            monitorProfilePath.find_last_of(L"\\/") == std::wstring::npos
                ? 0
                : monitorProfilePath.find_last_of(L"\\/") + 1);
        return !outBlob->bytes.empty();
      }
    }
  }

  if (BuildSyntheticDisplayProfile(options.displayState, outBlob)) {
    return true;
  }

  return BuildProfileBlobForPrimaries(QuickView::ColorPrimaries::SRGB, outBlob);
}

bool ExtractMatrixShaperProgram(cmsHPROFILE srcProfile,
                                cmsHPROFILE dstProfile,
                                QuickView::ComputeEngine* computeEngine,
                                CRenderEngine::GamutProgram* program) {
  if (!srcProfile || !dstProfile || !computeEngine || !program) return false;
  if (!cmsIsMatrixShaper(srcProfile) || !cmsIsMatrixShaper(dstProfile)) return false;
  if (cmsChannelsOfColorSpace(cmsGetColorSpace(srcProfile)) != 3 ||
      cmsChannelsOfColorSpace(cmsGetColorSpace(dstProfile)) != 3) {
    return false;
  }

  const auto* srcR = static_cast<const cmsCIEXYZ*>(cmsReadTag(srcProfile, cmsSigRedColorantTag));
  const auto* srcG = static_cast<const cmsCIEXYZ*>(cmsReadTag(srcProfile, cmsSigGreenColorantTag));
  const auto* srcB = static_cast<const cmsCIEXYZ*>(cmsReadTag(srcProfile, cmsSigBlueColorantTag));
  const auto* dstR = static_cast<const cmsCIEXYZ*>(cmsReadTag(dstProfile, cmsSigRedColorantTag));
  const auto* dstG = static_cast<const cmsCIEXYZ*>(cmsReadTag(dstProfile, cmsSigGreenColorantTag));
  const auto* dstB = static_cast<const cmsCIEXYZ*>(cmsReadTag(dstProfile, cmsSigBlueColorantTag));
  auto* trcR = static_cast<cmsToneCurve*>(cmsReadTag(srcProfile, cmsSigRedTRCTag));
  auto* trcG = static_cast<cmsToneCurve*>(cmsReadTag(srcProfile, cmsSigGreenTRCTag));
  auto* trcB = static_cast<cmsToneCurve*>(cmsReadTag(srcProfile, cmsSigBlueTRCTag));
  if (!srcR || !srcG || !srcB || !dstR || !dstG || !dstB || !trcR || !trcG || !trcB) {
    return false;
  }

  Matrix3 srcToXyz = {{{static_cast<float>(srcR->X), static_cast<float>(srcG->X), static_cast<float>(srcB->X)},
                       {static_cast<float>(srcR->Y), static_cast<float>(srcG->Y), static_cast<float>(srcB->Y)},
                       {static_cast<float>(srcR->Z), static_cast<float>(srcG->Z), static_cast<float>(srcB->Z)}}};
  Matrix3 dstToXyz = {{{static_cast<float>(dstR->X), static_cast<float>(dstG->X), static_cast<float>(dstB->X)},
                       {static_cast<float>(dstR->Y), static_cast<float>(dstG->Y), static_cast<float>(dstB->Y)},
                       {static_cast<float>(dstR->Z), static_cast<float>(dstG->Z), static_cast<float>(dstB->Z)}}};
  Matrix3 xyzToDst = {};
  if (!InvertMatrix3(dstToXyz, &xyzToDst)) return false;

  constexpr int kTrcEntries = 1024;
  program->analytic.trcR.resize(kTrcEntries);
  program->analytic.trcG.resize(kTrcEntries);
  program->analytic.trcB.resize(kTrcEntries);
  for (int i = 0; i < kTrcEntries; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kTrcEntries - 1);
    program->analytic.trcR[i] = cmsEvalToneCurveFloat(trcR, t);
    program->analytic.trcG[i] = cmsEvalToneCurveFloat(trcG, t);
    program->analytic.trcB[i] = cmsEvalToneCurveFloat(trcB, t);
  }

  memcpy(program->analytic.srcToXyz.data(), &srcToXyz.m[0][0], sizeof(float) * 9);
  memcpy(program->analytic.xyzToDst.data(), &xyzToDst.m[0][0], sizeof(float) * 9);

  if (FAILED(computeEngine->CreateTrcTexture1D(program->analytic.trcR.data(), kTrcEntries,
                                               &program->analytic.trcSrvR)) ||
      FAILED(computeEngine->CreateTrcTexture1D(program->analytic.trcG.data(), kTrcEntries,
                                               &program->analytic.trcSrvG)) ||
      FAILED(computeEngine->CreateTrcTexture1D(program->analytic.trcB.data(), kTrcEntries,
                                               &program->analytic.trcSrvB))) {
    return false;
  }

  program->backend = CRenderEngine::GamutBackendKind::AnalyticMatrixTrc;
  return true;
}

bool BuildLutProgram(cmsHPROFILE srcProfile,
                     cmsHPROFILE dstProfile,
                     int renderingIntent,
                     QuickView::ComputeEngine* computeEngine,
                     CRenderEngine::GamutProgram* program) {
  if (!srcProfile || !dstProfile || !computeEngine || !program) return false;

  constexpr int kEdge = 33;
  const size_t voxelCount = static_cast<size_t>(kEdge) * kEdge * kEdge;
  std::vector<cmsUInt16Number> input(voxelCount * 3);
  for (int b = 0; b < kEdge; ++b) {
    for (int g = 0; g < kEdge; ++g) {
      for (int r = 0; r < kEdge; ++r) {
        const size_t idx = static_cast<size_t>((b * kEdge + g) * kEdge + r);
        input[idx * 3 + 0] = static_cast<cmsUInt16Number>((r * 65535) / (kEdge - 1));
        input[idx * 3 + 1] = static_cast<cmsUInt16Number>((g * 65535) / (kEdge - 1));
        input[idx * 3 + 2] = static_cast<cmsUInt16Number>((b * 65535) / (kEdge - 1));
      }
    }
  }

  ScopedCmsTransform standard;
  standard.Reset(cmsCreateTransform(srcProfile, TYPE_RGB_16, dstProfile, TYPE_RGB_16,
                                    renderingIntent, cmsFLAGS_HIGHRESPRECALC));
  if (!standard.handle) return false;

  ScopedCmsTransform gamut;
  static std::mutex s_alarmMutex;
  {
    std::scoped_lock lock(s_alarmMutex);
    const cmsUInt16Number alarm[cmsMAXCHANNELS] = { 0, 65535, 0 };
    cmsSetAlarmCodes(alarm);
    gamut.Reset(cmsCreateProofingTransform(
        srcProfile, TYPE_RGB_16, dstProfile, TYPE_RGB_16, dstProfile,
        renderingIntent, renderingIntent,
        cmsFLAGS_SOFTPROOFING | cmsFLAGS_GAMUTCHECK | cmsFLAGS_HIGHRESPRECALC));
  }
  if (!gamut.handle) return false;

  std::vector<cmsUInt16Number> standardOut(voxelCount * 3);
  std::vector<cmsUInt16Number> gamutOut(voxelCount * 3);
  cmsDoTransform(standard.handle, input.data(), standardOut.data(),
                 static_cast<cmsUInt32Number>(voxelCount));
  cmsDoTransform(gamut.handle, input.data(), gamutOut.data(),
                 static_cast<cmsUInt32Number>(voxelCount));

  program->lut.edge = kEdge;
  program->lut.overflowLut.assign(voxelCount, 0);
  for (size_t i = 0; i < voxelCount; ++i) {
    const size_t base = i * 3;
    const bool overflow =
        standardOut[base + 0] != gamutOut[base + 0] ||
        standardOut[base + 1] != gamutOut[base + 1] ||
        standardOut[base + 2] != gamutOut[base + 2];
    program->lut.overflowLut[i] = overflow ? 255 : 0;
  }

  if (FAILED(computeEngine->UploadOverflowLut(program->lut.overflowLut.data(), kEdge,
                                              &program->lut.overflowTexture))) {
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R8_UNORM;
  srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  srvDesc.Texture3D.MostDetailedMip = 0;
  srvDesc.Texture3D.MipLevels = 1;
  if (FAILED(computeEngine->GetD3DDevice()->CreateShaderResourceView(
          program->lut.overflowTexture.Get(), &srvDesc, &program->lut.overflowSrv))) {
    return false;
  }

  program->backend = CRenderEngine::GamutBackendKind::Lut3DCompiled;
  return true;
}
} // namespace

float CRenderEngine::EstimateFramePeakScRgb(const QuickView::RawImageFrame &frame) {
  return InternalEstimateFramePeakScRgb(frame);
}

HRESULT CRenderEngine::AnalyzeGamutWarningIcc(
    const QuickView::RawImageFrame &frame,
    const GamutWarningAnalysisOptions &options,
    GamutWarningAnalysisResult *outResult) const {
  if (!outResult)
    return E_INVALIDARG;

  *outResult = {};
  outResult->width = frame.width;
  outResult->height = frame.height;

  if (!frame.IsValid() || !frame.pixels || frame.width <= 0 || frame.height <= 0) {
    return E_INVALIDARG;
  }

  if (options.renderingIntent != 1 || !m_computeEngine || !m_computeEngine->IsAvailable()) {
    return S_FALSE;
  }

  ProfileBlob srcBlob;
  ProfileBlob dstBlob;
  if (!ResolveSourceProfileBlob(frame, options.effectiveCmsMode, &srcBlob) ||
      !ResolveTargetProfileBlob(options, &dstBlob)) {
    return S_FALSE;
  }

  size_t key = 0;
  key = HashBytes(key, srcBlob.bytes.data(), srcBlob.bytes.size());
  key = HashBytes(key, dstBlob.bytes.data(), dstBlob.bytes.size());
  key = HashBytes(key, &options.renderingIntent, sizeof(options.renderingIntent));
  key = HashBytes(key, &options.targetKind, sizeof(options.targetKind));
  key = HashBytes(key, &options.acmAware, sizeof(options.acmAware));
  key = HashWString(key, options.displayState.gdiDeviceName);

  std::shared_ptr<GamutProgram> program;
  {
    std::scoped_lock lock(m_gamutProgramCacheMutex);
    auto it = m_gamutProgramCache.find(key);
    if (it != m_gamutProgramCache.end()) {
      program = it->second;
    }
  }

  if (!program) {
    auto compiled = std::make_shared<GamutProgram>();
    ScopedCmsProfile srcProfile;
    ScopedCmsProfile dstProfile;
    if (!OpenCmsProfileFromBlob(srcBlob, &srcProfile) ||
        !OpenCmsProfileFromBlob(dstBlob, &dstProfile)) {
      return S_FALSE;
    }

    compiled->srcName = srcBlob.name.empty() ? DescribeLcmsProfile(srcProfile.handle, L"Source ICC") : srcBlob.name;
    compiled->dstName = dstBlob.name.empty() ? DescribeLcmsProfile(dstProfile.handle, L"Target ICC") : dstBlob.name;

    if (!ExtractMatrixShaperProgram(srcProfile.handle, dstProfile.handle, m_computeEngine.get(),
                                    compiled.get())) {
      if (!options.allowGpuLutFallback ||
          !BuildLutProgram(srcProfile.handle, dstProfile.handle, options.renderingIntent,
                           m_computeEngine.get(), compiled.get())) {
        return S_FALSE;
      }
    }

    {
      std::scoped_lock lock(m_gamutProgramCacheMutex);
      m_gamutProgramCache[key] = compiled;
    }
    program = std::move(compiled);
  }

  auto dispatchWithProgram = [&](int maxDimension,
                                 QuickView::GamutMaskReadback* readback) -> HRESULT {
    QuickView::RawImageFrame sampledFrame;
    int cols = 0;
    int rows = 0;
    if (!BuildGamutCheckSampleFrame(frame, maxDimension, &sampledFrame, &cols, &rows)) {
      return E_FAIL;
    }

    ComPtr<ID3D11Texture2D> srcTexture;
    HRESULT hr = m_computeEngine->UploadAndConvert(sampledFrame.pixels, sampledFrame.width,
                                                   sampledFrame.height, sampledFrame.format,
                                                   &srcTexture);
    if (FAILED(hr)) return hr;

    if (program->backend == GamutBackendKind::AnalyticMatrixTrc) {
      QuickView::GamutAnalyticParams params = {};
      auto fillMatrix = [](const std::array<float, 9>& src, float dst[12]) {
        memset(dst, 0, sizeof(float) * 12);
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst[4] = src[3]; dst[5] = src[4]; dst[6] = src[5];
        dst[8] = src[6]; dst[9] = src[7]; dst[10] = src[8];
      };
      fillMatrix(program->analytic.srcToXyz, params.srcToXyz);
      fillMatrix(program->analytic.xyzToDst, params.xyzToDst);
      params.epsilon = 0.005f;
      params.trcEntries = static_cast<uint32_t>(program->analytic.trcR.size());
      params.width = static_cast<uint32_t>(sampledFrame.width);
      params.height = static_cast<uint32_t>(sampledFrame.height);
      hr = m_computeEngine->DispatchGamutMaskAnalytic(
          srcTexture.Get(),
          program->analytic.trcSrvR.Get(),
          program->analytic.trcSrvG.Get(),
          program->analytic.trcSrvB.Get(),
          params, readback);
    } else {
      hr = m_computeEngine->DispatchGamutMaskLut(
          srcTexture.Get(), program->lut.overflowSrv.Get(),
          program->lut.edge, 0.1f, readback);
    }
    return hr;
  };

  QuickView::GamutMaskReadback coarse = {};
  HRESULT hr = dispatchWithProgram(256, &coarse);
  if (FAILED(hr)) {
    return hr;
  }

  outResult->backendKind = program->backend;
  if (!coarse.hasOverflow) {
    outResult->cols = coarse.width;
    outResult->rows = coarse.height;
    outResult->mask = std::move(coarse.mask);
    outResult->hasOverflow = false;
  } else {
    QuickView::GamutMaskReadback fine = {};
    hr = dispatchWithProgram(1024, &fine);
    if (FAILED(hr)) {
      return hr;
    }
    outResult->cols = fine.width;
    outResult->rows = fine.height;
    outResult->mask = std::move(fine.mask);
    outResult->hasOverflow = fine.hasOverflow;
    if (outResult->hasOverflow) {
      DilateBinaryMask(outResult->mask, outResult->cols, outResult->rows);
    }
  }

  std::wstringstream summary;
  summary << L"Gamut "
          << (options.targetKind == GamutTargetKind::ProofTarget ? L"Proof" : L"Screen")
          << L" / "
          << (program->backend == GamutBackendKind::AnalyticMatrixTrc ? L"Analytic" :
              (program->backend == GamutBackendKind::Lut3DCompiled ? L"LUT" : L"CPU"))
          << L" / src=" << program->srcName
          << L" / dst=" << program->dstName
          << L" / ACM=" << (options.acmAware ? L"On" : L"Off")
          << L" / mask=" << outResult->cols << L"x" << outResult->rows;
  outResult->debugSummary = summary.str();
  return S_OK;
}

CRenderEngine::~CRenderEngine() {
  // ComPtr automatically releases resources
}

HRESULT CRenderEngine::Initialize(HWND hwnd) {
  m_hwnd = hwnd;

  HRESULT hr = S_OK;

  // 1. Create WIC factory
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&m_wicFactory));
  if (FAILED(hr))
    return hr;

  // 1.5 Create DWrite Factory
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           (IUnknown **)(&m_dwriteFactory));
  if (FAILED(hr))
    return hr;

  // 2. Create D3D11 device and D2D factory
  hr = CreateDeviceResources();
  if (FAILED(hr))
    return hr;

  // 3. Compute Engine Initialization
  m_computeEngine = std::make_unique<QuickView::ComputeEngine>();
  hr = m_computeEngine->Initialize(m_d3dDevice.Get());
  if (FAILED(hr)) {
    QV_LOG("Init_Warning", TraceLoggingString("Failed to initialize ComputeEngine", "Action"));
  }

  return S_OK;
}

HRESULT CRenderEngine::CreateDeviceResources() {
  HRESULT hr = S_OK;

  // D3D11 device creation flags
  // Note: BGRA_SUPPORT is required for D2D/DComp
  UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  // Feature levels
  D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  D3D_FEATURE_LEVEL featureLevel;

  // Create D3D11 device
  hr = D3D11CreateDevice(nullptr,                  // Default adapter
                         D3D_DRIVER_TYPE_HARDWARE, // Hardware acceleration
                         nullptr,                  // Software rasterizer
                         creationFlags, featureLevels, ARRAYSIZE(featureLevels),
                         D3D11_SDK_VERSION, &m_d3dDevice, &featureLevel,
                         &m_d3dContext);
  if (FAILED(hr))
    return hr;

  // Get DXGI device
  ComPtr<IDXGIDevice1> dxgiDevice;
  hr = m_d3dDevice.As(&dxgiDevice);
  if (FAILED(hr))
    return hr;

  // Create D2D factory
  D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
  options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options,
                         m_d2dFactory.GetAddressOf());
  if (FAILED(hr))
    return hr;

  // Create D2D device
  hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
  if (FAILED(hr))
    return hr;

  // Create D2D device context (Unbound, for resource creation)
  hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                        &m_d2dContext);
  if (FAILED(hr))
    return hr;

  return S_OK;
}

HRESULT CRenderEngine::LoadColorContextForPrimaries(
    QuickView::ColorPrimaries primaries,
    ID2D1ColorContext **outContext) const {
  if (!outContext)
    return E_INVALIDARG;

  *outContext = nullptr;
  switch (primaries) {
  case QuickView::ColorPrimaries::SRGB:
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  case QuickView::ColorPrimaries::DisplayP3:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_P3, outContext)
               ? S_OK
               : E_FAIL;
  case QuickView::ColorPrimaries::AdobeRGB:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_ADOBERGB, outContext)
               ? S_OK
               : E_FAIL;
  case QuickView::ColorPrimaries::ProPhotoRGB:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_PROPHOTO, outContext)
               ? S_OK
               : E_FAIL;
  default:
    return E_FAIL;
  }
}

HRESULT CRenderEngine::ResolveSourceColorContext(
    const QuickView::RawImageFrame &frame, int effectiveCmsMode,
    ID2D1ColorContext **outContext) {
  if (!outContext)
    return E_INVALIDARG;
  *outContext = nullptr;

  if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
    const QuickView::ColorPrimaries primaries =
        frame.colorInfo.primaries != QuickView::ColorPrimaries::Unknown
            ? frame.colorInfo.primaries
            : frame.hdrMetadata.primaries;
    if (IsSceneLinearFrame(frame)) {
      if (primaries == QuickView::ColorPrimaries::SRGB ||
          primaries == QuickView::ColorPrimaries::Unknown) {
        return CreateScRgbColorContext(m_d2dContext.Get(), outContext);
      }
      return LoadColorContextForPrimaries(primaries, outContext);
    }
    return CreateScRgbColorContext(m_d2dContext.Get(), outContext);
  }

  // [Fix] Manual overrides (modes 2,3,4,6) take absolute priority over embedded ICC.
  // Without this, images with embedded ICC profiles silently ignore user's color space selection.
  if (effectiveCmsMode == 2)
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  if (effectiveCmsMode == 3)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::DisplayP3,
                                        outContext);
  if (effectiveCmsMode == 4)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::AdobeRGB,
                                        outContext);
  if (effectiveCmsMode == 6)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB,
                                        outContext);

  // Auto (1) and Gray (5): Use embedded ICC profile if available
  if (!frame.iccProfile.empty()) {
    ColorContextCacheKey key{frame.iccProfile};
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_colorContextCache.find(key);
    if (it != m_colorContextCache.end()) {
      *outContext = it->second.Get();
      if (*outContext)
        (*outContext)->AddRef();
      return S_OK;
    }

    ComPtr<ID2D1ColorContext> embeddedContext;
    if (SUCCEEDED(m_d2dContext->CreateColorContext(
            D2D1_COLOR_SPACE_CUSTOM, frame.iccProfile.data(),
            static_cast<UINT32>(frame.iccProfile.size()), &embeddedContext))) {
      m_colorContextCache[key] = embeddedContext;
      *outContext = embeddedContext.Detach();
      return S_OK;
    }
  }

  if (effectiveCmsMode == 1 || effectiveCmsMode == 5) {
    if (frame.hdrMetadata.primaries == QuickView::ColorPrimaries::DisplayP3 ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::AdobeRGB ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::ProPhotoRGB) {
      if (SUCCEEDED(
              LoadColorContextForPrimaries(frame.hdrMetadata.primaries, outContext))) {
        return S_OK;
      }
    }

    if (frame.colorInfo.IsSrgb() ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::SRGB) {
      return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                              outContext);
    }

    const int fallback = g_config.CmsDefaultFallback;
    if (fallback == 1)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::DisplayP3,
                                          outContext);
    if (fallback == 2)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::AdobeRGB,
                                          outContext);
    if (fallback == 3)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB,
                                          outContext);
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  }

  return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                          outContext);
}

HRESULT CRenderEngine::ResolveDestinationColorContext(
    const QuickView::RawImageFrame &frame, ID2D1ColorContext **outContext) const {
  if (!outContext)
    return E_INVALIDARG;
  *outContext = nullptr;

  if (ShouldUseHdrOutputForFrame(frame)) {
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0,
                                            outContext);
  }

  // Windows 11 ACM / WCG can already apply the display transform after
  // composition. In that mode we should output a standard SDR working space
  // instead of pre-baking monitor correction into the bitmap, otherwise
  // soft-proof previews can become muted or effectively double-managed.
  if (m_displayColorState.ShouldBypassMonitorProfileForSdr()) {
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  }

  if (!m_displayColorState.gdiDeviceName.empty()) {
    HDC hdcMon =
        CreateDCW(L"DISPLAY", m_displayColorState.gdiDeviceName.c_str(), NULL, NULL);
    if (hdcMon) {
      DWORD dwLen = 0;
      GetICMProfileW(hdcMon, &dwLen, NULL);
      if (dwLen > 0) {
        std::wstring profilePath(dwLen, L'\0');
        if (GetICMProfileW(hdcMon, &dwLen, &profilePath[0])) {
          profilePath.resize(dwLen - 1);
          const HRESULT hr = m_d2dContext->CreateColorContextFromFilename(
              profilePath.c_str(), outContext);
          DeleteDC(hdcMon);
          if (SUCCEEDED(hr)) {
            return hr;
          }
        }
      }
      DeleteDC(hdcMon);
    }
  }

  return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                          outContext);
}

bool CRenderEngine::ShouldUseHdrOutputForFrame(
    const QuickView::RawImageFrame &frame) const {
  return IsHdrLikeFrame(frame) && m_isAdvancedColor &&
         g_config.IsAdvancedColorEnabled(m_displayColorState.advancedColorActive);
}

// Helper to standardize D2D1 bitmap properties creation
static inline D2D1_BITMAP_PROPERTIES1 GetDefaultBitmapProps(
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED) {
  return D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                 D2D1::PixelFormat(format, alphaMode), 96.0f,
                                 96.0f);
}

HRESULT CRenderEngine::CreateBitmapFromWIC(IWICBitmapSource *wicBitmap,
                                           ID2D1Bitmap **d2dBitmap) {
  if (!wicBitmap || !d2dBitmap)
    return E_INVALIDARG;

  HRESULT hr = S_OK;

  // Check source format first
  WICPixelFormatGUID srcFormat;
  hr = wicBitmap->GetPixelFormat(&srcFormat);
  if (FAILED(hr))
    return hr;

  IWICBitmapSource *srcToUse = wicBitmap;
  ComPtr<IWICFormatConverter> converter;

  // Preserve float HDR surfaces instead of collapsing them to 8-bit WIC BGRA.
  if (srcFormat == GUID_WICPixelFormat128bppRGBAFloat) {
    D2D1_BITMAP_PROPERTIES1 floatProps = GetDefaultBitmapProps(
        DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
    return m_d2dContext->CreateBitmapFromWicBitmap(
        wicBitmap, &floatProps, reinterpret_cast<ID2D1Bitmap1 **>(d2dBitmap));
  }

  // Only convert if source is NOT already PBGRA or BGRA
  bool needConvert = !(srcFormat == GUID_WICPixelFormat32bppPBGRA ||
                       srcFormat == GUID_WICPixelFormat32bppBGRA);

  if (needConvert) {
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
      return hr;

    hr = converter->Initialize(
        wicBitmap,
        GUID_WICPixelFormat32bppBGRA, // Use straight BGRA, not PBGRA
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
      return hr;
    srcToUse = converter.Get();
  }

  // Use PREMULTIPLIED mode for proper transparency support
  D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps();

  // Create D2D bitmap from WIC using Resource Context
  hr = m_d2dContext->CreateBitmapFromWicBitmap(
      srcToUse, &props, reinterpret_cast<ID2D1Bitmap1 **>(d2dBitmap));

  return hr;
}

HRESULT
CRenderEngine::UploadRawFrameToGPU(const QuickView::RawImageFrame &frame,
                                   ID2D1Bitmap **outBitmap) {
  if (!m_d2dContext)
    return E_POINTER;
  if (!outBitmap)
    return E_INVALIDARG;
  if (!frame.IsValid())
    return E_INVALIDARG;

  // ====================================================================
  // [GPU Pipeline] Multi-Layer Composition (GPU Bake)
  // ====================================================================
  // If the frame carries a blend operation (e.g. Ultra HDR Gain Map),
  // compose the layers on GPU and produce an FP16 texture directly.
  // The baked result is treated as a standard HDR bitmap.
  // ====================================================================
  if (frame.blendOp != QuickView::GpuBlendOp::None &&
      frame.auxLayer && frame.auxLayer->pixels &&
      m_computeEngine && m_computeEngine->IsAvailable())
  {
      switch (frame.blendOp) {
      case QuickView::GpuBlendOp::UltraHdrGainMap: {
          QuickView::GpuShaderPayload payload = frame.shaderPayload;
          // Calculate target headroom using the global override to allow simulation on SDR screens.
          payload.targetHeadroom = m_displayColorState.GetHdrHeadroomStops(g_config.HdrPeakNitsOverride);

          // Only trigger GPU Bake if we actually need to apply HDR gain (Headroom > 0).
          // For SDR displays, we fall back to the standard base-layer rendering
          // which preserves the exact color/brightness of the original JPEG via its ICC profile.
          if (payload.targetHeadroom <= 0.01f) {
              break; 
          }

          QV_LOG("Render_GpuBake",
              TraceLoggingString("UltraHDR GainMap Composition", "Action"),
              TraceLoggingFloat32(g_config.HdrPeakNitsOverride, "HdrPeakNitsOverride"),
              TraceLoggingFloat32(m_displayColorState.GetEffectivePeakNits(g_config.HdrPeakNitsOverride), "EffectivePeakNits"),
              TraceLoggingFloat32(payload.targetHeadroom, "TargetHeadroom"),
              TraceLoggingBool(m_displayColorState.advancedColorActive, "AdvancedColorActive"));

          // [Optimization] Check Cache to avoid re-uploading pixels during slider drag
          bool useCachedTextures = (m_bakeCache.lastBasePixels == frame.pixels && 
                                    m_bakeCache.lastAuxPixels == frame.auxLayer->pixels &&
                                    m_bakeCache.lastBaseW == frame.width &&
                                    m_bakeCache.lastBaseH == frame.height &&
                                    m_bakeCache.baseTexture != nullptr &&
                                    m_bakeCache.auxTexture != nullptr);

          if (useCachedTextures && fabsf(m_bakeCache.lastHeadroom - payload.targetHeadroom) < 0.001f && m_bakeCache.bakedBitmap) {
              QV_LOG("Render_BakeCache",
                  TraceLoggingString("CACHE HIT - No recomposition", "Action"),
                  TraceLoggingFloat32(m_bakeCache.lastHeadroom, "CachedHeadroom"),
                  TraceLoggingFloat32(payload.targetHeadroom, "RequestedHeadroom"));
              m_bakeCache.bakedBitmap.CopyTo(outBitmap);
              return S_OK;
          }

          QV_LOG("Render_BakeCache",
              TraceLoggingString(useCachedTextures ? "CACHE MISS - Recompose (headroom changed)" : "CACHE MISS - Full Upload + Bake", "Action"),
              TraceLoggingFloat32(m_bakeCache.lastHeadroom, "PrevHeadroom"),
              TraceLoggingFloat32(payload.targetHeadroom, "NewHeadroom"),
              TraceLoggingBool(useCachedTextures, "TexturesCached"));

          ComPtr<ID3D11Texture2D> pBaked;
          HRESULT hrBake = S_OK;

          if (useCachedTextures) {
              // Only re-run the shader with new headroom
              hrBake = m_computeEngine->ComposeGainMap(
                  m_bakeCache.baseTexture.Get(),
                  m_bakeCache.auxTexture.Get(),
                  payload, &pBaked);
          } else {
              // Full Upload + Bake
              hrBake = m_computeEngine->ComposeGainMap(
                  frame.pixels, frame.width, frame.height, frame.stride,
                  frame.format,
                  frame.auxLayer->pixels,
                  frame.auxLayer->width, frame.auxLayer->height,
                  frame.auxLayer->stride,
                  payload, &pBaked,
                  &m_bakeCache.baseTexture, &m_bakeCache.auxTexture);
              
              if (SUCCEEDED(hrBake)) {
                  m_bakeCache.lastBasePixels = frame.pixels;
                  m_bakeCache.lastAuxPixels = frame.auxLayer->pixels;
                  m_bakeCache.lastBaseW = frame.width;
                  m_bakeCache.lastBaseH = frame.height;
                  m_bakeCache.lastAuxW = frame.auxLayer->width;
                  m_bakeCache.lastAuxH = frame.auxLayer->height;
              }
          }

          if (SUCCEEDED(hrBake)) {
              m_bakeCache.lastHeadroom = payload.targetHeadroom;
              ComPtr<IDXGISurface> dxgiSurface;
              if (SUCCEEDED(pBaked.As(&dxgiSurface))) {
                  D2D1_BITMAP_PROPERTIES1 hdrProps = {};
                  hdrProps.pixelFormat.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                  hdrProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED; 
                  hdrProps.dpiX = 96.0f;
                  hdrProps.dpiY = 96.0f;
                  
                  ComPtr<ID2D1ColorContext> scRgbContext;
                  if (SUCCEEDED(m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scRgbContext))) {
                      hdrProps.colorContext = scRgbContext.Get();
                  }
                  
                  ComPtr<ID2D1Bitmap1> result;
                  HRESULT hrResult = m_d2dContext->CreateBitmapFromDxgiSurface(
                      dxgiSurface.Get(), &hdrProps, &result);
                  
                  if (SUCCEEDED(hrResult)) {
                      m_bakeCache.bakedBitmap = result;
                      result.CopyTo(outBitmap);
                      return S_OK;
                  }
              }
          }
          break;
      }
      default:
          break;
      }
  }

  // Map PixelFormat to DXGI_FORMAT and D2D1_ALPHA_MODE
  DXGI_FORMAT dxgiFormat;
  D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  const uint8_t *uploadPixels = frame.pixels;
  UINT uploadStride = static_cast<UINT>(frame.stride);
  std::vector<uint8_t> linearScRgbPixels;

  QV_LOG("Render_Upload",
      TraceLoggingInt32((int)frame.width, "Width"),
      TraceLoggingInt32((int)frame.height, "Height"),
      TraceLoggingInt32((int)frame.format, "Format"),
      TraceLoggingInt32((int)frame.blendOp, "BlendOp"),
      TraceLoggingBool(m_isAdvancedColor, "AdvColor"));

  // [Diagnostic] Full HDR metadata dump - critical for remote debugging
  if (frame.hdrMetadata.isValid || frame.hdrMetadata.isHdr || frame.hdrMetadata.hasGainMap) {
      QV_LOG("Render_ImageHdrMeta",
          TraceLoggingBool(frame.hdrMetadata.isHdr, "IsHdr"),
          TraceLoggingBool(frame.hdrMetadata.hasGainMap, "HasGainMap"),
          TraceLoggingBool(frame.hdrMetadata.gainMapApplied, "GainMapApplied"),
          TraceLoggingBool(frame.hdrMetadata.isSceneLinear, "IsSceneLinear"),
          TraceLoggingBool(frame.hdrMetadata.hasNitsMetadata, "HasNitsMetadata"),
          TraceLoggingInt32((int)frame.hdrMetadata.transfer, "Transfer"),
          TraceLoggingInt32((int)frame.hdrMetadata.primaries, "Primaries"),
          TraceLoggingFloat32(frame.hdrMetadata.maxCLLNits, "MaxCLL"),
          TraceLoggingFloat32(frame.hdrMetadata.maxFALLNits, "MaxFALL"),
          TraceLoggingFloat32(frame.hdrMetadata.masteringMinNits, "MasteringMin"),
          TraceLoggingFloat32(frame.hdrMetadata.masteringMaxNits, "MasteringMax"),
          TraceLoggingFloat32(frame.hdrMetadata.gainMapBaseHeadroom, "GainMapBaseHeadroom"),
          TraceLoggingFloat32(frame.hdrMetadata.gainMapAlternateHeadroom, "GainMapAltHeadroom"),
          TraceLoggingFloat32(frame.hdrMetadata.gainMapAppliedHeadroom, "GainMapAppliedHeadroom"));
  }

  // [Diagnostic] Pixel color info
  QV_LOG("Render_PixelColorInfo",
      TraceLoggingInt32((int)frame.colorInfo.dataSpace, "DataSpace"),
      TraceLoggingInt32((int)frame.colorInfo.transfer, "Transfer"),
      TraceLoggingInt32((int)frame.colorInfo.primaries, "Primaries"),
      TraceLoggingInt32((int)frame.colorInfo.nominalBitDepth, "BitDepth"),
      TraceLoggingBool(frame.colorInfo.hasEmbeddedIcc, "HasICC"));

  switch (frame.format) {
  case QuickView::PixelFormat::BGRA8888:
    dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    break;

  case QuickView::PixelFormat::BGRX8888:
    dxgiFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
    alphaMode = D2D1_ALPHA_MODE_IGNORE;
    break;

  case QuickView::PixelFormat::RGBA8888:
    dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    break;

  case QuickView::PixelFormat::R32G32B32A32_FLOAT:
    dxgiFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    alphaMode = D2D1_ALPHA_MODE_STRAIGHT;
    break;

  default:
    dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    break;
  }

  D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps(dxgiFormat, alphaMode);
  bool applyCms = false;
  ComPtr<ID2D1ColorContext> srcContext;
  ComPtr<ID2D1Bitmap1> rawBitmap;
  int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
  const bool useHdrOutput = ShouldUseHdrOutputForFrame(frame);

  // [Diagnostic] HDR routing decision + full display state snapshot
  QV_LOG("Render_HdrRouting",
      TraceLoggingBool(useHdrOutput, "UseHdrOutput"),
      TraceLoggingBool(m_isAdvancedColor, "IsAdvancedColor"),
      TraceLoggingBool(m_displayColorState.advancedColorActive, "DisplayAdvColorActive"),
      TraceLoggingBool(m_displayColorState.advancedColorSupported, "DisplayAdvColorSupported"),
      TraceLoggingFloat32(m_displayColorState.maxLuminanceNits, "DisplayMaxNits"),
      TraceLoggingFloat32(m_displayColorState.sdrWhiteLevelNits, "DisplaySdrWhiteNits"),
      TraceLoggingFloat32(m_displayColorState.minLuminanceNits, "DisplayMinNits"),
      TraceLoggingFloat32(m_displayColorState.maxFullFrameLuminanceNits, "DisplayMaxFullFrameNits"),
      TraceLoggingInt32((int)m_displayColorState.colorSpace, "DisplayColorSpace"),
      TraceLoggingFloat32(g_config.HdrPeakNitsOverride, "HdrPeakNitsOverride"));

  if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
      BuildLinearScRgbFloatBuffer(frame, linearScRgbPixels, &uploadPixels, &uploadStride);
      ComPtr<ID2D1ColorContext> scRgbContext;
      CreateScRgbColorContext(m_d2dContext.Get(), &scRgbContext);

      if (useHdrOutput) {
          // Pure HDR Environment (Roll-off)
          const QuickView::ToneMapSettings toneMapSettings = BuildToneMapSettings(frame, m_displayColorState);

          QV_LOG("Render_ToneMapDecision",
              TraceLoggingString("HDR Output Path", "Path"),
              TraceLoggingFloat32(g_config.HdrPeakNitsOverride, "HdrPeakNitsOverride"),
              TraceLoggingFloat32(m_displayColorState.GetEffectivePeakNits(g_config.HdrPeakNitsOverride), "EffectivePeakNits"),
              TraceLoggingFloat32(toneMapSettings.displayPeakScRgb, "DisplayPeakScRgb"),
              TraceLoggingFloat32(toneMapSettings.contentPeakScRgb, "ContentPeakScRgb"),
              TraceLoggingFloat32(toneMapSettings.paperWhiteScRgb, "PaperWhiteScRgb"),
              TraceLoggingFloat32(toneMapSettings.exposure, "Exposure"),
              TraceLoggingInt32(toneMapSettings.toneMappingMode, "ToneMappingMode"));

          if (m_computeEngine && m_computeEngine->IsAvailable() && toneMapSettings.contentPeakScRgb > (toneMapSettings.displayPeakScRgb > 1.0f ? toneMapSettings.displayPeakScRgb : 1.0f)) {
              QV_LOG("Render_ToneMapDecision",
                  TraceLoggingString("ToneMapHdrToHdr ACTIVE", "Shader"),
                  TraceLoggingFloat32(toneMapSettings.displayPeakScRgb, "DisplayPeak"),
                  TraceLoggingFloat32(toneMapSettings.contentPeakScRgb, "ContentPeak"));
              ComPtr<ID3D11Texture2D> pTex;
              if (SUCCEEDED(m_computeEngine->ToneMapHdrToHdr(
                      uploadPixels, static_cast<int>(frame.width),
                      static_cast<int>(frame.height), static_cast<int>(uploadStride),
                      toneMapSettings, &pTex))) {
                  ComPtr<IDXGISurface> dxgiSurface;
                  if (SUCCEEDED(pTex.As(&dxgiSurface))) {
                      D2D1_BITMAP_PROPERTIES1 hdrProps = GetDefaultBitmapProps(
                          DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
                      hdrProps.colorContext = scRgbContext.Get();
                      m_d2dContext->CreateBitmapFromDxgiSurface(
                          dxgiSurface.Get(), &hdrProps, &rawBitmap);
                  }
              }
          }
          if (!rawBitmap) {
              // PASSTHROUGH: No tone mapping applied - content fits within display range
              QV_LOG("Render_ToneMapDecision",
                  TraceLoggingString("HDR PASSTHROUGH - No ToneMap (content <= display)", "Shader"),
                  TraceLoggingFloat32(toneMapSettings.displayPeakScRgb, "DisplayPeak"),
                  TraceLoggingFloat32(toneMapSettings.contentPeakScRgb, "ContentPeak"),
                  TraceLoggingBool(m_computeEngine && m_computeEngine->IsAvailable(), "ComputeAvailable"));
              D2D1_BITMAP_PROPERTIES1 hdrProps = GetDefaultBitmapProps(
                  DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
              hdrProps.colorContext = scRgbContext.Get();
              m_d2dContext->CreateBitmap(
                  D2D1::SizeU(static_cast<UINT32>(frame.width),
                              static_cast<UINT32>(frame.height)),
                  uploadPixels, uploadStride, &hdrProps, &rawBitmap);
          }
          srcContext = scRgbContext; // For FLOAT, always scRGB source in CMS
      } else {
          // SDR Environment (Fallback Tone Mapping)
          if (m_computeEngine && m_computeEngine->IsAvailable()) {
              ComPtr<ID3D11Texture2D> pTex;
              const QuickView::ToneMapSettings toneMapSettings =
                  BuildToneMapSettings(frame, m_displayColorState);
              QV_LOG("Render_ToneMapDecision",
                  TraceLoggingString("SDR Output Path - ToneMapHdrToSdr", "Path"),
                  TraceLoggingFloat32(g_config.HdrPeakNitsOverride, "HdrPeakNitsOverride"),
                  TraceLoggingFloat32(toneMapSettings.displayPeakScRgb, "DisplayPeakScRgb"),
                  TraceLoggingFloat32(toneMapSettings.contentPeakScRgb, "ContentPeakScRgb"),
                  TraceLoggingFloat32(toneMapSettings.exposure, "Exposure"));
              if (SUCCEEDED(m_computeEngine->ToneMapHdrToSdr(
                      uploadPixels, static_cast<int>(frame.width),
                      static_cast<int>(frame.height), static_cast<int>(uploadStride),
                      toneMapSettings, &pTex))) {
                  ComPtr<IDXGISurface> dxgiSurface;
                  if (SUCCEEDED(pTex.As(&dxgiSurface))) {
                      D2D1_BITMAP_PROPERTIES1 sdrProps = GetDefaultBitmapProps(
                          DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                      m_d2dContext->CreateBitmapFromDxgiSurface(
                          dxgiSurface.Get(), &sdrProps, &rawBitmap);
                  }
              }
          }
          if (!rawBitmap) {
              std::vector<uint8_t> sdrPixels(static_cast<size_t>(frame.width) * frame.height * 4);
              const QuickView::ToneMapSettings toneMapSettings = BuildToneMapSettings(frame, m_displayColorState);
              const float sceneScale = toneMapSettings.exposure * toneMapSettings.paperWhiteScRgb;
              for (int y = 0; y < frame.height; ++y) {
                  const float *srcRow = reinterpret_cast<const float *>(uploadPixels + static_cast<size_t>(y) * uploadStride);
                  uint8_t *dstRow = sdrPixels.data() + static_cast<size_t>(y) * frame.width * 4;
                  for (int x = 0; x < frame.width; ++x) {
                      const float r = srcRow[x * 4 + 0] * sceneScale;
                      const float g = srcRow[x * 4 + 1] * sceneScale;
                      const float b = srcRow[x * 4 + 2] * sceneScale;
                      const float a = std::clamp(srcRow[x * 4 + 3], 0.0f, 1.0f);
                      float premulR, premulG, premulB;
                      if (toneMapSettings.toneMappingMode == 1) { // Colorimetric
                          premulR = std::min(1.0f, r) * a; premulG = std::min(1.0f, g) * a; premulB = std::min(1.0f, b) * a;
                      } else { // Perceptual
                          premulR = ToneMapAces(r) * a; premulG = ToneMapAces(g) * a; premulB = ToneMapAces(b) * a;
                      }
                      dstRow[x * 4 + 0] = EncodeLinearToSdr8(premulB);
                      dstRow[x * 4 + 1] = EncodeLinearToSdr8(premulG);
                      dstRow[x * 4 + 2] = EncodeLinearToSdr8(premulR);
                      dstRow[x * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
                  }
              }
              D2D1_BITMAP_PROPERTIES1 sdrProps = GetDefaultBitmapProps(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
              m_d2dContext->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height)),
                  sdrPixels.data(), static_cast<UINT32>(frame.width * 4), &sdrProps, &rawBitmap);
          }
          // After tone mapping to SDR, the source for the next CMS step is sRGB
          m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
      }
  }

  // [ CMS Re-architected ] 
  // Step 1: Pre-calculate CMS requirements if not already handled by FLOAT path
  if (!rawBitmap) {
    // 1. If mode is Unmanaged (0), always false.
    // 2. If mode is Manual (>1), always true (override).
    // 3. If mode is Auto (1), respect the Global Toggle (g_config.ColorManagement).
    applyCms = (effectiveCmsMode == 0) ? false : (effectiveCmsMode == 1 ? g_config.ColorManagement : true);
    ResolveSourceColorContext(frame, effectiveCmsMode, &srcContext);
  }

  if (!srcContext)
    m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);

  if (!rawBitmap) {
    D2D1_BITMAP_PROPERTIES1 propsWithContext = props;
    propsWithContext.colorContext = srcContext.Get();
    // [Optimization] GPU Compute for non-native format conversion (RGBA/BGRX)
    if (m_computeEngine && m_computeEngine->IsAvailable() &&
        frame.format != QuickView::PixelFormat::BGRA8888 &&
        frame.pixels) {
      ComPtr<ID3D11Texture2D> pTex;
      if (SUCCEEDED(m_computeEngine->UploadAndConvert(
              frame.pixels, (int)frame.width, (int)frame.height, frame.format,
              &pTex))) {
        ComPtr<IDXGISurface> dxgiSurface;
        if (SUCCEEDED(pTex.As(&dxgiSurface))) {
          D2D1_BITMAP_PROPERTIES1 computeProps = propsWithContext;
          computeProps.pixelFormat.format = DXGI_FORMAT_R8G8B8A8_UNORM;
          computeProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
          m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &computeProps, &rawBitmap);
        }
      }
    }
    // Fallback: Direct pixel upload
    if (!rawBitmap) {
      if (FAILED(m_d2dContext->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height)),
                  frame.pixels, static_cast<UINT32>(frame.stride), &propsWithContext, &rawBitmap))) {
          return E_FAIL;
      }
    }
  }

  // Step 3: Apply CMS and Soft Proofing
  const bool enableSoftProofing = g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty();
  if ((applyCms && effectiveCmsMode != 0) || enableSoftProofing) {
    // Find destination context (Monitor or scRGB)
    ComPtr<ID2D1ColorContext> dstContext;

    ResolveDestinationColorContext(frame, &dstContext);

    if (srcContext && dstContext) {
      ComPtr<ID2D1Effect> colorManagementEffect;
      if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &colorManagementEffect))) {

        ComPtr<ID2D1Image> currentInput = rawBitmap;

        // Soft Proofing Node Setup
        ComPtr<ID2D1ColorContext> proofContext;
        ComPtr<ID2D1Effect> softProofEffect;
        bool softProofSucceeded = false;

        if (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty()) {
          if (SUCCEEDED(m_d2dContext->CreateColorContextFromFilename(
                  g_runtime.SoftProofProfilePath.c_str(), &proofContext))) {
            if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &softProofEffect))) {
              softProofEffect->SetInput(0, currentInput.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srcContext.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, proofContext.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE, D2D1_COLORMANAGEMENT_ALPHA_MODE_STRAIGHT);
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);
              
              const D2D1_COLORMANAGEMENT_RENDERING_INTENT intent = (g_config.CmsRenderingIntent == 0) ? 
                  D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL : 
                  D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC;

              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT, intent);
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT, intent);
              softProofEffect->GetOutput(&currentInput);
              softProofSucceeded = true;
            }
          }
        }

        colorManagementEffect->SetInput(0, currentInput.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, softProofSucceeded ? proofContext.Get() : srcContext.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, dstContext.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE, D2D1_COLORMANAGEMENT_ALPHA_MODE_STRAIGHT);
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);

        const D2D1_COLORMANAGEMENT_RENDERING_INTENT intent = (g_config.CmsRenderingIntent == 0) ? 
            D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL : 
            D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC;
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT, intent);
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT, intent);

        ComPtr<ID2D1Image> cmsOutput;
        colorManagementEffect->GetOutput(&cmsOutput);

        if (effectiveCmsMode == 5) {
          ComPtr<ID2D1Effect> grayscaleEffect;
          if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1Grayscale, &grayscaleEffect))) {
            grayscaleEffect->SetInput(0, cmsOutput.Get());
            grayscaleEffect->GetOutput(&cmsOutput);
          }
        }

        // Render to persistent target managed bitmap
        ComPtr<ID2D1Bitmap1> managedBitmap;
        D2D1_BITMAP_PROPERTIES1 targetProps = props;
        if (useHdrOutput) {
          targetProps.pixelFormat.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
          targetProps.pixelFormat.alphaMode = alphaMode;
        }
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.colorContext = dstContext.Get(); // Mandatory for correct DrawImage interpretation
        D2D1_SIZE_U targetSize = D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height));

        if (SUCCEEDED(m_d2dContext->CreateBitmap(targetSize, nullptr, 0, &targetProps, &managedBitmap))) {
          ComPtr<ID2D1Image> oldTarget;
          m_d2dContext->GetTarget(&oldTarget);
          float oldDpiX, oldDpiY;
          m_d2dContext->GetDpi(&oldDpiX, &oldDpiY);
          m_d2dContext->SetDpi(96.0f, 96.0f);
          auto oldUnitMode = m_d2dContext->GetUnitMode();
          m_d2dContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

          m_d2dContext->SetTarget(managedBitmap.Get());
          m_d2dContext->BeginDraw();
          m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));

          // Render raw frame into CMS-aware target
          m_d2dContext->DrawImage(cmsOutput.Get());

          HRESULT endDrawHr = m_d2dContext->EndDraw();
          m_d2dContext->SetTarget(oldTarget.Get());
          m_d2dContext->SetDpi(oldDpiX, oldDpiY);
          m_d2dContext->SetUnitMode(oldUnitMode);

          if (SUCCEEDED(endDrawHr)) {
            *outBitmap = managedBitmap.Detach();
            return S_OK;
          }
        }
      }
    }
  }

  // Fallback if CMS failed or bypassed (rawBitmap still has srcContext correctly attached)
  *outBitmap = rawBitmap.Detach();
  return S_OK;
}

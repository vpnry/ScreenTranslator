#include "tesseract.h"
#include "debug.h"
#include "languagecodes.h"
#include "task.h"

#include <leptonica/allheaders.h>

#include <QBuffer>
#include <QDir>
#include <QLibrary>

#if defined(Q_OS_LINUX)
#include <fstream>
static qint64 getFreeMemory()
{
  std::string token;
  std::ifstream file("/proc/meminfo");
  qint64 freeMem = 0;
  while (file >> token) {
    if (token == "MemFree:" || token == "Buffers:" || token == "Cached:") {
      unsigned long mem = 0;
      freeMem += (file >> mem) ? mem : 0;
    }
  }
  return freeMem * 1024;
}
#elif defined(Q_OS_WIN)
#include <windows.h>
#undef min
#undef max
static qint64 getFreeMemory()
{
  MEMORYSTATUSEX statex;
  statex.dwLength = sizeof(statex);
  if (GlobalMemoryStatusEx(&statex)) {
    return statex.ullAvailPhys;
  }
  return -1;
}
#endif

static Pix *convertImage(const QImage &image)
{
  QBuffer buffer;
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "BMP");
  const auto &data = buffer.data();
  return pixReadMemBmp(reinterpret_cast<const l_uint8 *>(data.constData()),
                       data.size());
}

static QImage convertImage(Pix &image)
{
  l_uint8 *buffer = nullptr;
  size_t len = 0;
  pixWriteMemBmp(&buffer, &len, &image);

  QImage result;
  result.loadFromData(static_cast<uchar *>(buffer), int(len));
  return result;
}

static double getScale(Pix *source)
{
  SOFT_ASSERT(source, return -1.0);

  const auto xRes = pixGetXRes(source);
  const auto yRes = pixGetYRes(source);
  if (xRes * yRes == 0)
    return -1.0;

  const auto preferredScale = std::max(500.0 / std::min(xRes, yRes), 1.0);
  if (preferredScale <= 1.0)
    return -1.0;

  const auto maxScaleX = std::numeric_limits<int>::max() / double(source->w);
  const auto scaleX = std::min(preferredScale, maxScaleX);
  const auto maxScaleY = std::numeric_limits<int>::max() / double(source->h);
  const auto scaleY = std::min(preferredScale, maxScaleY);
  auto scale = std::min(scaleX, scaleY);

  const auto availableMemory = getFreeMemory() * 0.95;
  if (availableMemory < 1)
    return -1.0;

  const auto actualSize = source->w * source->h * source->d / 8;
  const auto maxScaleMemory = availableMemory / actualSize;
  scale = std::min(scale, maxScaleMemory);

  return scale;
}

// Smart pointer for Pix
class PixGuard
{
public:
  explicit PixGuard(Pix *pix = nullptr)
    : pix_(pix)
  {
  }
  ~PixGuard()
  {
    if (pix_)
      pixDestroy(&pix_);
  }
  void operator=(Pix *pix)
  {
    if (!pix)
      return;
    if (pix_)
      pixDestroy(&pix_);
    pix_ = pix;
  }
  operator Pix *() { return pix_; }
  Pix *operator->() { return pix_; }
  Pix *&get() { return pix_; }
  Pix *take()
  {
    auto ret = pix_;
    pix_ = nullptr;
    return ret;
  }
  void trace(const QString &name) const
  {
    LTRACE() << qPrintable(name) << pix_;
#if 0
    if (!pix_)
      return;
    auto fileName = name + ".png";
    fileName.replace(' ', "_");
    convertImage(*pix_).save(fileName);
#endif
  }

private:
  Pix *pix_;

  Q_DISABLE_COPY(PixGuard);
};

static Pix *prepareImage(const QImage &image)
{
  auto pix = PixGuard(convertImage(image));
  SOFT_ASSERT(pix, return nullptr);
  pix.trace("Pix 1 Converted");

  {
    pix = pixConvertRGBToGray(pix, 0.0, 0.0, 0.0);
    pix.trace("Pix 2 Gray");
  }

  if (const auto scale = getScale(pix); scale > 1.0) {
    pix = pixScaleGrayLI(pix, scale, scale);
    pix.trace("Pix 3 Scaled");
  }

  l_int32 otsuSx = 5000;
  l_int32 otsuSy = 5000;
  l_int32 otsuSmoothx = 0;
  l_int32 otsuSmoothy = 0;
  l_float32 otsuScorefract = 0.1f;

  {
    PixGuard otsu;
    pixOtsuAdaptiveThreshold(pix, otsuSx, otsuSy, otsuSmoothx, otsuSmoothy,
                             otsuScorefract, nullptr, &otsu.get());
    pix.trace("Pix 4 Test Color Otsu");

    // Get the average intensity of the border pixels,
    // with average of 0.0 being completely white and 1.0 being completely black
    // Top
    auto avg = pixAverageOnLine(otsu, 0, 0, otsu->w - 1, 0, 1);
    // Bottom
    avg += pixAverageOnLine(otsu, 0, otsu->h - 1, otsu->w - 1, otsu->h - 1, 1);
    // Left
    avg += pixAverageOnLine(otsu, 0, 0, 0, otsu->h - 1, 1);
    // Right
    avg += pixAverageOnLine(otsu, otsu->w - 1, 0, otsu->w - 1, otsu->h - 1, 1);
    avg /= 4.0f;

    // If background is dark
    l_float32 threshold = 0.5f;
    if (avg > threshold) {
      pix = pixInvert(nullptr, pix);
      pix.trace("Pix 5 Inverted");
    }
  }

  {
    l_int32 usm_halfwidth = 5;
    l_float32 usm_fract = 2.5f;
    pix = pixUnsharpMaskingGray(pix, usm_halfwidth, usm_fract);
    pix.trace("Pix 6 Unshapred");
  }

  {
    pixOtsuAdaptiveThreshold(pix, otsuSx, otsuSy, otsuSmoothx, otsuSmoothy, 0.0,
                             nullptr, &pix.get());
    pix.trace("Pix 7 Binarized");
  }

  pix.trace("Pix 8 Result");

  return pix.take();
}

static void cleanupImage(Pix **image)
{
  pixDestroy(image);
}

// do not include capi.h from tesseract because it defined BOOL that breaks msvc
struct TessBaseAPI;

class Tesseract::Wrapper
{
  using CreateApi = TessBaseAPI *(*)();
  using DeleteApi = void (*)(TessBaseAPI *);
  using InitApi = int (*)(TessBaseAPI *, const char *, const char *, int);
  using SetImage = void (*)(TessBaseAPI *, struct Pix *);
  using GetUtf8 = char *(*)(TessBaseAPI *);
  using ClearApi = void (*)(TessBaseAPI *);
  using DeleteUtf8 = void (*)(const char *);
  using SetPageMode = void (*)(TessBaseAPI *, int);

public:
  explicit Wrapper(const QString &libraryName)
    : lib(libraryName)
  {
    if (!lib.load()) {
      LERROR() << "Failed to load tesseract library" << libraryName;
      return;
    }

    LTRACE() << "Loaded tesseract library" << lib.fileName();
    auto ok = true;
    ok &= bool(createApi_ = (CreateApi)lib.resolve("TessBaseAPICreate"));
    ok &= bool(deleteApi_ = (DeleteApi)lib.resolve("TessBaseAPIDelete"));
    ok &= bool(initApi_ = (InitApi)lib.resolve("TessBaseAPIInit2"));
    ok &= bool(setImage_ = (SetImage)lib.resolve("TessBaseAPISetImage2"));
    ok &= bool(getUtf8_ = (GetUtf8)lib.resolve("TessBaseAPIGetUTF8Text"));
    ok &= bool(clearApi_ = (ClearApi)lib.resolve("TessBaseAPIClear"));
    ok &= bool(deleteUtf8_ = (DeleteUtf8)lib.resolve("TessDeleteText"));
    ok &= bool(setPageMode_ =
                   (SetPageMode)lib.resolve("TessBaseAPISetPageSegMode"));
    if (!ok) {
      LERROR() << "Failed to resolve tesseract functions from" << libraryName;
      return;
    }
    handle_ = createApi_();
  }

  ~Wrapper()
  {
    if (handle_ && deleteApi_) {
      deleteApi_(handle_);
    }
    lib.unload();
  }

  int Init(const char *datapath, const char *language)
  {
    SOFT_ASSERT(handle_, return -1);
    SOFT_ASSERT(initApi_, return -1);

    const auto mode = 3;  // TessOcrEngineMode::OEM_DEFAULT
    return initApi_(handle_, datapath, language, mode);
  }

  QString GetText(Pix *pix)
  {
    SOFT_ASSERT(handle_, return {});

    SOFT_ASSERT(setPageMode_, return {});
    setPageMode_(handle_, 3);  //    PSM_AUTO

    SOFT_ASSERT(setImage_, return {});
    setImage_(handle_, pix);
    LTRACE() << "Set Pix to engine";

    char *outText = nullptr;

    SOFT_ASSERT(getUtf8_, return {});
    outText = getUtf8_(handle_);
    LTRACE() << "Received recognized text";

    SOFT_ASSERT(clearApi_, return {});
    clearApi_(handle_);
    LTRACE() << "Cleared engine";

    const auto result = QString(outText).trimmed();

    SOFT_ASSERT(deleteUtf8_, return {});
    deleteUtf8_(outText);
    LTRACE() << "Cleared recognized text buffer";

    return result;
  }

private:
  QLibrary lib;
  CreateApi createApi_{nullptr};
  DeleteApi deleteApi_{nullptr};
  InitApi initApi_{nullptr};
  SetImage setImage_{nullptr};
  GetUtf8 getUtf8_{nullptr};
  ClearApi clearApi_{nullptr};
  DeleteUtf8 deleteUtf8_{nullptr};
  SetPageMode setPageMode_{nullptr};
  TessBaseAPI *handle_{nullptr};
};

Tesseract::Tesseract(const LanguageId &language, const QString &tessdataPath,
                     const QString &tesseractLibrary)
  : tesseractLibrary_(tesseractLibrary)
{
  SOFT_ASSERT(!tessdataPath.isEmpty(), return );
  SOFT_ASSERT(!language.isEmpty(), return );

  init(language, tessdataPath);
}

Tesseract::~Tesseract() = default;

void Tesseract::init(const LanguageId &language, const QString &tessdataPath)
{
  SOFT_ASSERT(!engine_, return );

  engine_ = std::make_unique<Wrapper>(tesseractLibrary_);
  LTRACE() << "Created Tesseract api" << engine_.get();

  const auto tesseractName = LanguageCodes::tesseract(language);
  auto result =
      engine_->Init(qPrintable(tessdataPath), qPrintable(tesseractName));
  LTRACE() << "Inited Tesseract api" << result;
  if (result == 0)
    return;

  error_ = QObject::tr("init failed");
  engine_.reset();
  LTRACE() << "Cleared Tesseract api";
}

const QString &Tesseract::error() const
{
  return error_;
}

QStringList Tesseract::availableLanguageNames(const QString &path)
{
  if (path.isEmpty())
    return {};

  QDir dir(path);
  if (!dir.exists())
    return {};

  LanguageIds names;

  const auto files = dir.entryList({"*.traineddata"}, QDir::Files);
  for (const auto &file : files) {
    const auto lang = file.left(file.indexOf("."));
    const auto name = LanguageCodes::name(LanguageCodes::idForTesseract(lang));
    names.append(name);
  }

  if (names.isEmpty())
    return {};

  return names;
}

QString Tesseract::recognize(const QPixmap &source)
{
  SOFT_ASSERT(engine_, return {});
  SOFT_ASSERT(!source.isNull(), return {});

  error_.clear();

  Pix *image = prepareImage(source.toImage());
  SOFT_ASSERT(image, return {});
  LTRACE() << "Preprocessed Pix for OCR" << image;

  auto result = engine_->GetText(image);

  cleanupImage(&image);
  LTRACE() << "Cleared preprocessed Pix";

  if (result.isEmpty())
    error_ = QObject::tr("Failed to recognize text or no text selected");
  return result;
}

bool Tesseract::isValid() const
{
  return engine_.get();
}

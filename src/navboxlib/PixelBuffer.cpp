#include "PixelBuffer.h"
#include "fileclass.h"
#include "log.h"
#include <PNGdec.h>
#include <cstring>
#include <algorithm>
#include <cmath>

#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
#define BUF_HAS_PSRAM
static bool bufUsePsram = psramFound();
#endif

PixelBuffer::~PixelBuffer() {
    clear(true);
}

void PixelBuffer::setMemPool(MemPool* pool) { clear(true); mempool_ = pool; }

bool PixelBuffer::allocate(coord_t width, coord_t height, coord_t offsetX, coord_t offsetY) {
    uint32_t sizereq = width * height * sizeof(pixel_t);
    if (sizereq < datalen_ && data_) {
        clear(false); //don't free, reuse memory
        MAP_LOG("pxl:alloc reusing %d (have %d)", sizereq, datalen_);
    } else {
        clear(true); //free
        if (mempool_) data_ = (uint8_t*) mempool_->alloc(sizereq);
#ifdef BUF_HAS_PSRAM
        else if (bufUsePsram) data_ = (uint8_t*)ps_malloc(sizereq);
#endif
        else data_ = (uint8_t*)malloc(sizereq);
        if (!data_) { return false; }
        datalen_ = sizereq;
    }
    width_ = width;
    height_ = height;
    offsetX_ = offsetX;
    offsetY_ = offsetY;
    return true;
}

void PixelBuffer::clear(bool freemem) {
    if ((freemem || mempool_) && data_) {
        if (!mempool_) free(data_); //free works on psram as well
        data_ = nullptr;
        datalen_ = 0;
    }
    width_ = height_ = 0;
    offsetX_ = offsetY_ = 0;
    isInverted_ = false;
}

void PixelBuffer::drawPixelAbs(coord_t x, coord_t y, pixel_t value) {
    if (data_ && (x < width_ && y < height_)) {
        pixel_t* p = getPixelPtrAbs(x, y);
        *p = value;
    }
}

pixel_t* PixelBuffer::getPixelPtrAbs(coord_t x, coord_t y) {
    if (!data_ || x >= width_ || y >= height_) return nullptr;
    auto data = (pixel_t*) data_;
    return &data[y * width_ + x];
}

static PNG _png;

static void * png_open(const char *fn, int32_t *s) {
    fs::File *f = new fs::File(SD.open(fn));
    if (!f || !*f) { if (f) delete f; return nullptr; }
    *s = f->size(); return (void *)f;
}
static void png_close(void *h) { fs::File *f = (fs::File *)h; if (f) { f->close(); delete f; } }
static int32_t png_read(PNGFILE *h, uint8_t *b, int32_t l) { return static_cast<fs::File *>(h->fHandle)->read(b, l); }
static int32_t png_seek(PNGFILE *h, int32_t p) { return static_cast<fs::File *>(h->fHandle)->seek(p); }

constexpr uint16_t MAX_IMG_WIDTH = 256;

// Context for sparse PNG decoding
struct SparsePngContext {
    PixelBuffer* buffer;
    Bounds b;
    uint16_t lineBuffer[MAX_IMG_WIDTH];
};

static int png_draw_sparse(PNGDRAW *p) {
    auto ctx = (SparsePngContext *)p->pUser;

    // Skip rows outside crop region
    if (ctx->b && (p->y < ctx->b.top || p->y >= ctx->b.bttm)) {
        return 1; // Continue decoding but skip this row
    }

    _png.getLineAsRGB565(p, ctx->lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0);

    // Copy only the cropped portion
    coord_t srcStart = ctx->b ? ctx->b.left : 0;
    coord_t srcEnd = ctx->b ? ctx->b.right : _png.getWidth();
    coord_t copyWidth = srcEnd - srcStart;
    coord_t dstY = ctx->b ? (p->y - ctx->b.top) : p->y;

    // Copy the cropped line into the buffer
    if (dstY < ctx->buffer->height_) {
        std::memcpy(
            ctx->buffer->getPixelPtrAbs(0, dstY),
            &ctx->lineBuffer[srcStart],
            copyWidth * sizeof(uint16_t)
        );
    }

    return 1; // Continue decoding
}

int freeHeap(); // in maprenderer.cpp

bool PixelBuffer::loadImg(const char* path, const Bounds &b) {
    if (!path || !path[0]) return false;

    SparsePngContext ctx = {this, b};

    if (_png.open(path, png_open, png_close, png_read, png_seek, png_draw_sparse) != PNG_SUCCESS) return false;

    coord_t fw = _png.getWidth(), fh = _png.getHeight();
    if (fw > MAX_IMG_WIDTH) {
        MAP_LOG("pxl::load img w (%d) too large (%d max)", fw, MAX_IMG_WIDTH);
        return false;
    }
    coord_t aw = fw, ah = fh;

    if (ctx.b) {
        ctx.b.left = std::min(ctx.b.left, (coord_t)(fw - 1));
        ctx.b.top = std::min(ctx.b.top, (coord_t)(fh - 1));
        ctx.b.right = ctx.b.right ? std::min(ctx.b.right, fw) : fw;
        ctx.b.bttm = ctx.b.bttm ? std::min(ctx.b.bttm, fh) : fh;
        aw = ctx.b.right - ctx.b.left;
        ah = ctx.b.bttm - ctx.b.top;
        // MAP_LOG("pxl::load %s [%dx%d] cropped (%d,%d,%d,%d)]", path, fw, fh, ctx.b.left, ctx.b.top, ctx.b.right, ctx.b.bttm);
    }
    if (aw == 0 || ah == 0) {
        MAP_LOG("pxl:load 0-size image %dx%d", aw, ah);
        _png.close();
        clear(false);
        return false;
    }

    if (!allocate(aw, ah, ctx.b.left, ctx.b.top)) {
        MAP_LOG("pxl::load [%dx%d] failed, OOM (free: %d)", aw, ah, freeHeap());
        _png.close();
        return false;
    }
    MAP_LOG("pxl::load alloc OK %dx%d = %d%%full", aw, ah, mempool_? mempool_->bufpos_ * 100 / mempool_->bufsize_ : -1);

    uncroppedW_ = fw, uncroppedH_ = fh;
    int rc = _png.decode(&ctx, 0);
    _png.close();

    if (rc != PNG_SUCCESS) {
        clear(false);
        MAP_LOG("pxl::load %s decode failed", path);
        return false;
    }

    // MAP_LOG("pxl::load %s finished (free: %d) [%dx%d] offset: (%d,%d)", path, freeHeap(), width_, height_, offsetX_, offsetY_);
    return true;
}

void PixelBuffer::_rgb565ToRgb8(pixel_t color, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = (color >> 11) & 0x1F;      // 5 bits
    uint8_t g6 = (color >> 5) & 0x3F;       // 6 bits
    uint8_t b5 = color & 0x1F;              // 5 bits
    // Scale to 0-255 (replicate MSBs into LSBs for better accuracy)
    r = (r5 << 3) | (r5 >> 2);              // r5 * 255/31 ≈ r5 << 3 + r5 >> 2
    g = (g6 << 2) | (g6 >> 4);              // g6 * 255/63 ≈ g6 << 2 + g6 >> 4
    b = (b5 << 3) | (b5 >> 2);              // b5 * 255/31 ≈ b5 << 3 + b5 >> 2
}

pixel_t PixelBuffer::_rgb8ToRgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t r5 = (r >> 3) & 0x1F;
    uint16_t g6 = (g >> 2) & 0x3F;
    uint16_t b5 = (b >> 3) & 0x1F;
    return (r5 << 11) | (g6 << 5) | b5;
}

uint16_t PixelBuffer::_rgb565ToRgb444(uint16_t c) {
    uint16_t r = (c >> 12) & 0x0F;
    uint16_t g = (c >> 7)  & 0x0F;
    uint16_t b = (c >> 1)  & 0x0F;
    return (r << 8) | (g << 4) | b;
}

PixelBuffer::HSL PixelBuffer::_rgbToHsl(uint8_t r, uint8_t g, uint8_t b) {
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    float maxv = std::max({rf, gf, bf});
    float minv = std::min({rf, gf, bf});
    float delta = maxv - minv;
    PixelBuffer::HSL hsl{};
    hsl.l = (maxv + minv) * 0.5f;
    if (delta < 0.00001f) {
        hsl.h = 0.0f;
        hsl.s = 0.0f;
        return hsl;
    }
    hsl.s = delta / (1.0f - std::fabs(2.0f * hsl.l - 1.0f));
    if (maxv == rf) {
        hsl.h = 60.0f * std::fmod(((gf - bf) / delta), 6.0f);
    } else if (maxv == gf) {
        hsl.h = 60.0f * (((bf - rf) / delta) + 2.0f);
    } else {
        hsl.h = 60.0f * (((rf - gf) / delta) + 4.0f);
    }
    if (hsl.h < 0.0f)
        hsl.h += 360.0f;
    return hsl;
}

void PixelBuffer::_hslToRgb(const HSL& hsl, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = (1.0f - std::fabs(2.0f * hsl.l - 1.0f)) * hsl.s;
    float hp = hsl.h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r1 = 0.0f;
    float g1 = 0.0f;
    float b1 = 0.0f;
    if (hp >= 0.0f && hp < 1.0f) { r1 = c; g1 = x; }
    else if (hp < 2.0f) { r1 = x; g1 = c; }
    else if (hp < 3.0f) { g1 = c; b1 = x; }
    else if (hp < 4.0f) { g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; b1 = c; }
    else { r1 = c; b1 = x; }
    float m = hsl.l - c * 0.5f;
    r = static_cast<uint8_t>(std::round((r1 + m) * 255.0f));
    g = static_cast<uint8_t>(std::round((g1 + m) * 255.0f));
    b = static_cast<uint8_t>(std::round((b1 + m) * 255.0f));
}

uint16_t* _darkMode444Lut = nullptr;
constexpr uint16_t LUT_MAX_SIZE = (1 << 12); //2^12 for 12-bit RGB444 color
constexpr uint16_t LUT_MISSING = UINT16_MAX;

pixel_t PixelBuffer::_smartInvert(pixel_t color, float satBoost) {
    if (!_darkMode444Lut) {
        #ifdef BUF_HAS_PSRAM
        _darkMode444Lut = bufUsePsram? (uint16_t*)ps_malloc(sizeof(uint16_t) * (LUT_MAX_SIZE + 1)) : nullptr;
        #else
        _darkMode444Lut = new uint16_t[LUT_MAX_SIZE + 1]();
        #endif
        if (_darkMode444Lut)
            std::memset(_darkMode444Lut, LUT_MISSING, sizeof(uint16_t) * (LUT_MAX_SIZE + 1));
    }
    auto c444 = _rgb565ToRgb444(color);
    if (_darkMode444Lut && _darkMode444Lut[c444] != LUT_MISSING) return _darkMode444Lut[c444]; //try LUT first

    uint8_t r, g, b;
    _rgb565ToRgb8(color, r, g, b);
    auto hsl = _rgbToHsl(r, g, b);
    hsl.l = 1.0f - hsl.l;  // Invert brightness, keep hue + saturation
    hsl.s *= satBoost;  // Boost saturation
    if (hsl.s > 1.0f) hsl.s = 1.0f;
    _hslToRgb(hsl, r, g, b);

    pixel_t res = _rgb8ToRgb565(r, g, b);
    if (_darkMode444Lut) { //add to the cache
        _darkMode444Lut[c444] = (res == LUT_MISSING)? LUT_MISSING - 1 : res;
    }
    return res;
}

void PixelBuffer::doInvert(bool smartInvert, float satBoost) {
    if (!valid()) return;
    pixel_t* pData = (pixel_t*)data_;
    for (size_t i = 0; i < width_ * height_; i++) {
        auto& p = pData[i];
        if (smartInvert) p = _smartInvert(p, satBoost);
        else {
            uint8_t r, g, b;
            _rgb565ToRgb8(p, r, g, b);
            p = _rgb8ToRgb565(255 - r, 255 - g, 255 - b);
        }
    }
    isInverted_ = true;
    if (_darkMode444Lut) {
        uint32_t lutCount = 0;
        for (uint32_t i = 0; i <= UINT16_MAX; i++)
            if (_darkMode444Lut[i] != LUT_MISSING) lutCount++;
        float coverage = (lutCount * 100.0f) / (UINT16_MAX + 1);
        MAP_LOG("pixel: invert [%dx%d] (LUT coverage: %.2f%%)", width_, height_, coverage);
    }
}

void MemPool::init(uint32_t size) {
    deinit();
    if ((buf_ = (uint8_t*)malloc(size))) {
        bufsize_ = size;
        bufpos_ = 0;
    }
}
void MemPool::deinit() {
    if (buf_) free(buf_);
    buf_ = nullptr;
    bufsize_ = bufpos_ = 0;
}
uint8_t* MemPool::alloc(uint32_t size) {
    if (!buf_ || (bufpos_ + size > bufsize_)) {
        MAP_LOG("mempool::alloc OOM, want %d have %d", size, bufsize_ - bufpos_);
        return nullptr;
    }
    uint8_t* ret = buf_ + bufpos_;
    bufpos_ += size;
    return ret;
}
void MemPool::reset() {
    MAP_LOG("mempool::reset (was %d)", bufpos_);
    bufpos_ = 0;
}

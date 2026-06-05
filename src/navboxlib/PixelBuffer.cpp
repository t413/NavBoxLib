#include "PixelBuffer.h"
#include "fileclass.h"
#include "log.h"
#include <PNGdec.h>
#include <cstring>
#include <algorithm>

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

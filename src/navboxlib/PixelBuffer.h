#pragma once
#include <stdint.h>

typedef uint16_t coord_t;
typedef uint16_t pixel_t;
#define GET_RED(color) (((color) & 0xF800) >> 8)
#define GET_GREEN(color) (((color) & 0x07E0) >> 3)
#define GET_BLUE(color) (((color) & 0x001F) << 3)
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) + (((g) & 0xFC) << 3) + (((b) & 0xF8) >> 3))

struct Bounds;
struct MemPool;

/// PixelBuffer, generic buffer to hold image data with optional sparse mode.
class PixelBuffer {
public:
    PixelBuffer() : data_(nullptr), width_(0), height_(0), offsetX_(0), offsetY_(0) { }
    ~PixelBuffer();

    bool allocate(coord_t width, coord_t height, coord_t offsetX = 0, coord_t offsetY = 0);
    void setMemPool(MemPool*);
    void clear(bool freemem);
    void drawPixelAbs(coord_t x, coord_t y, pixel_t value);
    pixel_t* getPixelPtrAbs(coord_t x, coord_t y);

    pixel_t* getData() const { return (pixel_t*) data_; }
    uint32_t size() const { return width_ * height_; }
    bool valid() const { return width_ + height_ > 0 && data_ != nullptr; }

    /// Load PNG image with optional cropping.
    /// left/right crop is saved as offsetN_, right/bttm values become the new width/heights, 0 crops none
    bool loadImg(const char* path, const Bounds &);

    int getOffsetX() const { return offsetX_; }
    int getOffsetY() const { return offsetY_; }
    bool isSparse() const { return offsetX_ != 0 || offsetY_ != 0; }

    struct HSL { float h, s, l; };  // h: [0, 360], s: [0, 1], v: [0, 1]
    static void _rgb565ToRgb8(pixel_t color, uint8_t& r, uint8_t& g, uint8_t& b);
    static pixel_t _rgb8ToRgb565(uint8_t r, uint8_t g, uint8_t b);
    static pixel_t _rgb565ToRgb444(pixel_t c);
    static HSL _rgbToHsl(uint8_t r, uint8_t g, uint8_t b);
    static void _hslToRgb(const HSL& hsv, uint8_t& r, uint8_t& g, uint8_t& b);
    static pixel_t _smartInvert(pixel_t rgb, float satBoost = 0.8f);

    void doInvert(bool smartInvert = true, float satBoost = 0.8f);
    bool isFiltered() const { return isFiltered_; }

    coord_t uncroppedW_ = 0, uncroppedH_ = 0;
    coord_t width_ = 0, height_ = 0;
    MemPool* mempool_ = nullptr;

protected:
    uint8_t* data_ = nullptr; // RGB565 data for LVGL compatibility
    uint32_t datalen_ = 0;
    int offsetX_, offsetY_;  // Offset within original image for sparse buffers
    bool isFiltered_ = false;
};

struct Bounds {
    coord_t left = 0, top = 0, right = 0, bttm = 0;
    operator bool() const { return left || top || right || bttm; }
};

struct MemPool {
    uint8_t* buf_ = nullptr;
    uint32_t bufsize_ = 0;
    uint32_t bufpos_ = 0;
    virtual void init(uint32_t size);
    virtual void deinit();
    virtual uint8_t* alloc(uint32_t size);
    virtual void reset();
};

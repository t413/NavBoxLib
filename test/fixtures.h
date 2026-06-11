#pragma once
#include <string>
#include <vector>
#include <lvgl.h>
#include <filesystem>
#include <navboxlib/GeoPoint.h>

namespace fixtures {

static const TrackPoint TEST_CENTER(37.87125, -122.31767);
static const std::string TILE_FMT_END = "/%d/%d/%d.png";
static const std::string TILE_FMT = "/tmp" + TILE_FMT_END;
static constexpr int TEST_Z = 10;
static constexpr int TEST_X = 512;
static constexpr int TEST_Y = 512;
extern const std::filesystem::path OSM_TILES_STASH;

std::string getStashTilesPath();
std::string tilePath(int z, int x, int y);


std::string fmtstr(const char* fmt, ... );
std::string basename(const std::string& path);
std::string testname();
std::string cwd();
void printfile(const char* fn);

bool draw_lvgl_png(lv_display_t* drv, const char* path);

// A minimal 4x4 rainbow png. starts with r, g, b, w, then more rainbow for the rest.
extern const std::vector<uint8_t> png4x4;
extern const std::vector<uint8_t> png256hi;

struct TmpFileHelper {
    std::string fn_;
    TmpFileHelper(const std::vector<uint8_t> &img, std::string fn="");
    ~TmpFileHelper() { rm(); }
    void rm();
};

struct GifMaker {
    GifMaker(const std::string& path, uint16_t w, uint16_t h);
    ~GifMaker();
    bool addFrame(lv_display_t* drv, int delayMs = 5);
    void finish();

private:
    std::string path_;
    uint16_t w_, h_;
    void* state_ = nullptr;
};

struct LvglTestEnv {
    std::vector<lv_color_t> buf_;
    lv_display_t* disp_ = nullptr;
    lv_obj_t* base_ = nullptr;
    uint16_t width_ = 0, height_ = 0;

    LvglTestEnv(uint16_t width, uint16_t height, bool clearfirst = true);
    ~LvglTestEnv();
    void reset(uint16_t width=0, uint16_t height=0);

    void draw();
    std::filesystem::path outdir() const;
    bool save(std::string suffix="_canvas");
    void clearfiles();
};

} //fixtures

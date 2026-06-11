#include "MapRenderer.h"
#include "log.h"
#include "TrackLog.h"
#include "MapLayer.h"
#include "fileclass.h"
#include <lvgl.h>

const PixelBuffer* MapRenderer::TileCacheEntry::load(int ox, int oy, int oz, const char* fmt, const Bounds& bounds) {
    char path[128];
    snprintf(path, sizeof(path), fmt, oz, ox, oy);
    if (buffer.loadImg(path, bounds)) {
        z = oz, x = ox, y = oy, onscreen = 0;
#if LV_VERSION_CHECK(9, 0, 0)
        lv_draw_buf_init(
            &dsc_, buffer.width_, buffer.height_, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO,
            (void*)buffer.getData(), buffer.width_ * buffer.height_ * sizeof(uint16_t)
        );
#else
        memset(&dsc_, 0, sizeof(lv_img_dsc_t));
        dsc_.header.w = buffer.width_;
        dsc_.header.h = buffer.height_;
        dsc_.data_size = buffer.width_ * buffer.height_ * sizeof(uint16_t);
        dsc_.header.cf = LV_IMG_CF_TRUE_COLOR;
        dsc_.data = (const uint8_t*)buffer.getData();
#endif
        return &buffer;
    } else MAP_LOG("tile::load failed loading %s (free: %u)", path, freeHeap());
    return nullptr;
}

void MapRenderer::TileCacheEntry::update(int px, int py, bool visible, zoom_t scale, uint32_t redrawIdx) {
    if (visible) {
        lv_coord_t adjustedX = px + (lv_coord_t)(buffer.getOffsetX() * scale);
        lv_coord_t adjustedY = py + (lv_coord_t)(buffer.getOffsetY() * scale);
        lv_img_set_pivot(img_obj, 0, 0);
        lv_img_set_zoom(img_obj, (uint16_t)(scale * buffer.uncroppedW_)); //TODO allow fractional zoom
        lv_obj_set_pos(img_obj, adjustedX, adjustedY);
        lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(img_obj, lv_obj_get_index(img_obj) - 1); //forground
    } else {
        lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    }
    onscreen = visible;
    lastUsed = onscreen? redrawIdx : 0;
}

void MapRenderer::TileCacheEntry::clear() {
    z = -1, x = -1, y = -1;
    update(0,0,false);
    buffer.clear(false); //don't free memory
}

MapRenderer::~MapRenderer() {
    for (auto& t : cache_)
        t.buffer.clear(true);
    for (auto& layer : layers_) {
        if (layer == markerLayer_) markerLayer_ = nullptr;
        delete layer;
        layer = nullptr;
    }
    if (base_) lv_obj_del(base_);
    if (tilesBase_) lv_obj_del(tilesBase_);
    base_ = tilesBase_ = nullptr;
}

bool MapRenderer::begin(lv_obj_t* parent, uint16_t w, uint16_t h, const char* fmt, uint16_t tileSize) {
    width_ = w;
    height_ = h;
    tileSize_ = tileSize;
    pathPattern_ = fmt;

#ifdef ARDUINO
    cropmode_ = !psramFound();
#endif

    if (!parent) {
        return false;
    }
    base_ = lv_obj_create(parent);
    tilesBase_ = lv_obj_create(base_);
    for (auto obj : {base_, tilesBase_}) {
        lv_obj_set_size(obj, width_, height_);
        lv_obj_set_pos(obj, x_, y_);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    }
    lv_obj_set_style_bg_color(base_, lv_color_hex(colBg_), 0);

    // Create fixed image objects for the 2x2 grid
    cache_.resize(TILECACHE_SIZE);
    for (auto& t : cache_) {
        if (t.img_obj) continue;
        t.img_obj = lv_img_create(tilesBase_);
        t.update(0, 0, false); //hide
    }
    return true;
}

void MapRenderer::setXY(uint16_t x, uint16_t y) { lv_obj_set_pos(base_, (x_ = x), (y_ = y)); }

void MapRenderer::invalidate() {
    if (cropmode_) { //invalidate cache first
        MAP_LOG("cropmode enabled, clearing tile cache on recenter");
        for (auto& t : cache_)
            t.clear();
        mempool_.reset();
    }
    _updateTiles(millis(), cropmode_);
}

uint8_t MapRenderer::iterate(uint32_t now, bool loadAll) {
    uint8_t loaded = 0;
    while (!loadQueue_.empty()) {
        bool ret = _loadOneQueuedTile(now);  // synchronous
        loaded++;
        if (!loadAll) break;
    }
    if (loaded)
        MAP_LOG("map queue blocking-loaded %d tiles", loaded);
    return loaded;
}

bool MapRenderer::project(double lat, double lon, lv_coord_t& px, lv_coord_t& py) const {
    const auto sts = scaledTileSize();
    double tx, ty, cx, cy;
    _latLonToTileF(lat, lon, zoom_, tx, ty);
    _latLonToTileF(mapCenter_.lat(), mapCenter_.lon(), zoom_, cx, cy);
    px = (lv_coord_t)round((tx - cx) * sts + width_  / 2);
    py = (lv_coord_t)round((ty - cy) * sts + height_ / 2);
    return isVisible(px, py);
}

bool MapRenderer::isVisible(lv_coord_t px, lv_coord_t py) const {
    return px >= 0 && px < width_ && py >= 0 && py < height_;
}

void MapRenderer::setCenter(const GeoPoint& p, zoom_t zoom) {
    mapCenter_ = p;
    if (zoom > 0 && zoom < 20) zoom_ = zoom;
    invalidate();
}

void MapRenderer::setZoom(zoom_t zoom) {
    zoom_ = std::fmin(std::fmax(0.0, zoom), 20.0);
    MAP_LOG("setZoom %0.1f", zoom_);
    invalidate();
}

int MapRenderer::findBestZoomWithTiles(const GeoPoint &cntr, int start) {
    //zoom out until finding tiles for this zoom level
    char path[128];
    for (int z = start; z > 0; z--) {
        double tx, ty;
        _latLonToTileF(cntr.lat(), cntr.lon(), z, tx, ty);
        snprintf(path, sizeof(path), pathPattern_, z, (int)tx, (int)ty);
        bool has = SD.exists(path);
        MAP_LOG("find zoom %s -> %d", path, has);
        if (has) { return z; } //found
    }
    return 0;
}

zoom_t MapRenderer::scaledTileSize()      const { return scaledTileSize(floor(zoom_)); }
zoom_t MapRenderer::scaledTileSize(int z) const { return (tileSize_ * powf(2.0f, zoom_ - z)); }

void MapRenderer::panPx(int16_t dx, int16_t dy) {
    const auto sts = scaledTileSize(); //already accounts for magnification
    const double totalPx = (double)sts * pow(2.0, zoom_); // total world width in screen-pixels
    double ty = _latToTileY(mapCenter_.lat(), zoom_) + (double)dy / sts;
    setCenter({_tileYToLat(ty, zoom_), mapCenter_.lon() + dx / totalPx * 360.0});
}


void MapRenderer::addLayer(MapLayer* layer) {
    layers_.push_back(layer);
}

void MapRenderer::removeLayer(MapLayer* layer) {
    layers_.erase(std::remove_if(layers_.begin(), layers_.end(),
        [layer](MapLayer* ptr) { return ptr == layer; }), layers_.end());
    if (markerLayer_ == layer) markerLayer_ = nullptr;
}

MarkerLayer* MapRenderer::getMarkerLayer() {
    if (!markerLayer_)
        addLayer( (markerLayer_ = new MarkerLayer(this)) );
    return markerLayer_;
}

MapRenderer::XY MapRenderer::getMarkerPx(uint16_t id) const {
    if (!markerLayer_) return XY{-1,-1};
    const auto& mo = markerLayer_->findObj(id);
    if (!mo.obj_ || lv_obj_has_flag(mo.obj_, LV_OBJ_FLAG_HIDDEN))
        return XY{-1, -1};
    return XY{
        (int16_t)(lv_obj_get_x(mo.obj_) + mo.m_.size / 2),
        (int16_t)(lv_obj_get_y(mo.obj_) + mo.m_.size / 2)
    };
}

int16_t MapRenderer::getPxDistToCenter(const MapRenderer::XY &pos) const {
    int16_t dx = pos.x - (int16_t)(width_ / 2);
    int16_t dy = pos.y - (int16_t)(height_ / 2);
    return (int16_t)sqrt(dx * dx + dy * dy);
}
void MapRenderer::setSmartInvert(bool smartInvert) {
    smartInvert_ = smartInvert;
    MAP_LOG("setSmartInvert %d", smartInvert);
    for (auto& t : cache_) {
        if (smartInvert)
            t.buffer.doInvert(smartInvert);
        else t.clear(); //reload fresh
    }
    if (!smartInvert) invalidate(); //trigger reload
}


// Private update methods

void MapRenderer::_updateLayers() {
    for (auto& layer : layers_) {
        layer->update();
    }
}

void MapRenderer::_updateTiles(uint32_t now, bool blockingload) {
    if (!base_ || !tilesBase_) return;
    if (cropmode_ && !mempool_.buf_) { //runs the first time, or after being unloaded
        mempool_.init(width_ * height_ * sizeof(pixel_t) + 2048); // a little extra
        for (auto& t : cache_) t.buffer.setMemPool(&mempool_);
        MAP_LOG("tiles cropmode mempool allocated %d bytes (%dx%d)", mempool_.bufsize_, width_, height_);
    }

    for (auto& t : cache_)
        t.onscreen = false;

    auto ctx = _calcTileGrid(mapCenter_.lat(), mapCenter_.lon(), zoom_);

    auto updated = _updateAndQueueTiles(ctx, now, true); //queue

    for (auto& t : cache_)
        if (!t.onscreen)
            t.update(0, 0, false); //hide offscreen tiles

    if (blockingload) {
        iterate(now, true);
    } else if (!loadQueue_.empty()) { MAP_LOG("map queue has %d items", (int)loadQueue_.size()); }

    _updateLayers();
    redrawIdx_++;
}

MapRenderer::TileGridCtx MapRenderer::_calcTileGrid(double lat, double lon, zoom_t zoom) const {
    double tx, ty;
    int izoom = (int)floor(zoom);
    _latLonToTileF(lat, lon, izoom, tx, ty);
    TileGridCtx ret = {
        .sts = scaledTileSize(izoom),
        .nTiles = 1 << izoom,
    };
    ret.tx_s = (int)floor(tx - (double)width_  / 2 / ret.sts);
    ret.ty_s = (int)floor(ty - (double)height_ / 2 / ret.sts);
    ret.px_s = (int)round((ret.tx_s - tx) * ret.sts + width_  / 2.0);
    ret.py_s = (int)round((ret.ty_s - ty) * ret.sts + height_ / 2.0);

    ret.cols = (int)ceil((double)width_  / ret.sts) + 1;
    ret.rows = (int)ceil((double)height_ / ret.sts) + 1;
    return ret;
}

MapRenderer::XY MapRenderer::_calcTileScreenPos(int tx, int ty, int tz) const {
    double cx_display, cy_display;
    _latLonToTileF(mapCenter_.lat(), mapCenter_.lon(), tz, cx_display, cy_display);
    const auto sts = scaledTileSize(tz);
    return {
        .x = (int16_t)round((tx - cx_display) * sts + width_  / 2.0),
        .y = (int16_t)round((ty - cy_display) * sts + height_ / 2.0),
    };
}

uint8_t MapRenderer::_updateAndQueueTiles(const TileGridCtx& ctx, uint32_t now, bool allowQueue) {
    // Collect missing tiles into loadQueue_ of TileLoadRequest
    uint8_t updated = 0;

    for (int row = 0; row < ctx.rows; row++) {
        for (int col = 0; col < ctx.cols; col++) {

            const int gx = ((ctx.tx_s + col) % ctx.nTiles + ctx.nTiles) % ctx.nTiles;
            const int gy = ctx.ty_s + row;
            if (gy < 0 || gy >= ctx.nTiles) continue;

            XY dest = {
                .x = (int16_t)(ctx.px_s + col * (int)ctx.sts),
                .y = (int16_t)(ctx.py_s + row * (int)ctx.sts),
            };
            // Skip tiles entirely outside the canvas
            if (dest.x + (int)ctx.sts <= 0 || dest.x >= width_)  continue;
            if (dest.y + (int)ctx.sts <= 0 || dest.y >= height_) continue;

            int idx = _findTile(gx, gy, zoom_, true);
            zoom_t scale = 1;
            if (idx != -1) {
                auto& tile = cache_[idx];
                if (tile.z != zoom_) { //magnication needed!
                    scale = powf(2.0f, zoom_ - tile.z);
                    dest = _calcTileScreenPos(tile.x, tile.y, tile.z);
                }
                tile.update(dest.x, dest.y, true, scale, redrawIdx_);
                updated++;
            }
            if (allowQueue && (idx == -1 || floor(scale) != 1)) {
                _queueTileRequest(gx, gy, zoom_, now, dest);
            }
        }
    }
    return updated;
}

bool MapRenderer::_queueTileRequest(int x, int y, int z, uint32_t now, MapRenderer::XY dest) {
    if (loadQueue_.size() >= TILECACHE_SIZE) return false;
    for (const auto& req : loadQueue_) {
        if (req.x == x && req.y == y && req.z == z)
            return false;
    }
    if (z != zoom_) dest = {INT16_MIN, INT16_MIN};
    TileLoadRequest req = {x, y, z, now, dest};
    loadQueue_.push_back(req);
    return true;
}

bool MapRenderer::_loadOneQueuedTile(uint32_t now) {
    if (!loadQueue_.empty()) {
        const auto m = loadQueue_.back();
        loadQueue_.pop_back();

        int newIdx = _findSlot();
        auto& tile = cache_[newIdx];
        if (newIdx != -1) { // found empty slot
            auto& tile = cache_[newIdx];
            XY dest = m.dest;
            zoom_t scale = 1; //default
            if (dest.x == INT16_MIN || dest.y == INT16_MIN || now != m.queuedAt || m.z != zoom_) {
                dest = _calcTileScreenPos(m.x, m.y, m.z); //update the tile position first!
                scale = powf(2.0f, zoom_ - m.z);
            }
            if (dest.x == INT16_MIN || dest.y == INT16_MIN) {
                MAP_LOG("tile deque bad pos %d,%d,%d -> %d,%d", m.x, m.y, m.z, dest.x, dest.y);
                return false;
            }
            Bounds crop;
            if (cropmode_) {
                crop.left  = (coord_t)std::max(0, (int)(-dest.x / scale));
                crop.top   = (coord_t)std::max(0, (int)(-dest.y / scale));
                crop.right = (coord_t)std::min((int)tileSize_, (int)((width_  - dest.x) / scale));
                crop.bttm  = (coord_t)std::min((int)tileSize_, (int)((height_ - dest.y) / scale));
            }

            if (tile.load(m.x, m.y, m.z, pathPattern_, crop)) {
                if (smartInvert_ && !tile.buffer.isFiltered())
                    tile.buffer.doInvert(true);
                lv_img_set_src(tile.img_obj, &tile.dsc_);
                MAP_LOG("tile loaded %d,%d,%d -> %d,%d scale %.2f", m.x, m.y, m.z, dest.x, dest.y, scale);
                if (dest.x != INT16_MIN && dest.y != INT16_MIN) {
                    tile.update(dest.x, dest.y, true, scale, redrawIdx_); //straight to screen
                    if (m.z == zoom_)
                        _mvForground(tile);
                } else tile.update(0, 0, false); //offscreen cache load
                return true;
            } else MAP_LOG("ERROR loading tile %d,%d,%d", m.x, m.y, m.z);
        } else MAP_LOG("ERROR: no empty tile slots!");
    }
    return false;
}

void MapRenderer::_mvForground(const TileCacheEntry& tile) {
    if (!tile.img_obj) return;
    int32_t max_idx = 0;
    lv_obj_t* max_obj = nullptr;
    for (const auto& entry : cache_) {
        if (entry.img_obj && !entry.onscreen && lv_obj_get_index(entry.img_obj) > max_idx) {
            max_idx = lv_obj_get_index(entry.img_obj);
            max_obj = entry.img_obj;
        }
    }
    // lv_obj_move_to_index(tile.img_obj, lv_obj_get_index(tile.img_obj) - 1);
    if (max_obj) lv_obj_swap(tile.img_obj, max_obj);
}

// Static geo math

void MapRenderer::_latLonToTileF(double lat, double lon, int z, double& tx, double& ty) {
    float n = (1 << z);
    tx = n * (lon + 180.0) / 360.0;
    double lrad = lat * DEG_2_RAD;
    ty = n * (1.0 - log(tan(lrad) + 1.0/cos(lrad)) / M_PI) / 2.0;
}

double MapRenderer::_latToTileY(double lat, int z) {
    float n = (1 << z);
    double lrad = lat * DEG_2_RAD;
    return n * (1.0 - log(tan(lrad) + 1.0/cos(lrad)) / M_PI) / 2.0;
}

double MapRenderer::_tileYToLat(double ty, int z) {
    float n = (1 << z);
    double sinh_val = sinh(M_PI * (1.0 - 2.0 * ty / n));
    return atan(sinh_val) / DEG_2_RAD;
}

// Cache helpers

int MapRenderer::_findTile(int x, int y, int z, bool allowback) {
    for (int i = 0; i < TILECACHE_SIZE; i++) {
        if (cache_[i].is(x, y, z)) return i;
    }
    if (allowback) {
        // Look for a tile from one zoom level out that covers this area
        int bz = z, bx = x, by = y;
        int levels = 0;
        while (bz > 0 && levels < 3) {
            bz--; bx /= 2; by /= 2; levels++;
            int idx = _findTile(bx, by, bz, false);
            if (idx != -1) return idx;
        }
    }
    return -1;
}

int MapRenderer::_findSlot() {
    int ret = -1;
    uint32_t minLastUsed = UINT32_MAX;
    for (int j = 0; j < TILECACHE_SIZE; j++) {
        if (!cache_[j].onscreen && cache_[j].lastUsed < minLastUsed) {
            minLastUsed = cache_[j].lastUsed;
            ret = j;
        }
    }
    return ret;
}
int freeHeap() {
    #ifdef ARDUINO
    return ESP.getFreeHeap();
    #else
    return -1;
    #endif
}

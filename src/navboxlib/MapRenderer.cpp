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
        lv_draw_buf_init(
            &dsc_, buffer.width_, buffer.height_, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO,
            (void*)buffer.getData(), buffer.width_ * buffer.height_ * sizeof(uint16_t)
        );
        return &buffer;
    } else MAP_LOG("tile::load failed loading %s (free: %u)", path, freeHeap());
    return nullptr;
}

void MapRenderer::TileCacheEntry::update(int px, int py, bool visible, zoom_t magnification, uint32_t redrawIdx) {
    if (visible) {
        lv_coord_t adjustedX = px + (lv_coord_t)(buffer.getOffsetX() * magnification);
        lv_coord_t adjustedY = py + (lv_coord_t)(buffer.getOffsetY() * magnification);
        lv_img_set_pivot(img_obj, 0, 0);
        lv_img_set_zoom(img_obj, (uint16_t)(magnification * buffer.uncroppedW_)); //TODO allow fractional zoom
        lv_obj_set_pos(img_obj, adjustedX, adjustedY);
        lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
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
    if (obj_) lv_obj_del(obj_);  // Parent container
    obj_ = nullptr;
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
    obj_ = lv_obj_create(parent);
    lv_obj_set_size(obj_, width_, height_);
    lv_obj_set_pos(obj_, x_, y_);
    lv_obj_set_style_pad_all(obj_, 0, 0);
    lv_obj_set_style_border_width(obj_, 0, 0);
    lv_obj_set_style_radius(obj_, 0, 0);
    lv_obj_set_scrollbar_mode(obj_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(obj_, lv_color_hex(colBg_), 0);

    // Create fixed image objects for the 2x2 grid
    cache_.resize(TILECACHE_SIZE);
    for (auto& t : cache_) {
        if (t.img_obj) continue;
        t.img_obj = lv_img_create(obj_);
        t.update(0, 0, false); //hide
    }
    return true;
}

void MapRenderer::setXY(uint16_t x, uint16_t y) { lv_obj_set_pos(obj_, (x_ = x), (y_ = y)); }

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
    MAP_LOG("map queue blocking-loaded %d tiles", loaded);
    return loaded;
}

bool MapRenderer::project(double lat, double lon, lv_coord_t& px, lv_coord_t& py) const {
    const uint16_t sts = scaledTileSize();
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

void MapRenderer::setZoom(zoom_t zoom, zoom_t magnification) {
    if (magnification == MAGNF_AUTO) {
        zoom_ = findBestZoomWithTiles(mapCenter_, zoom);
        magnification = zoom - zoom_ + 1;
        zoom = zoom_;
    }
    if (zoom != ZOOM_UNCHANGED)
        zoom_          = (zoom > 20) ? 20 : zoom;
    magnification_ = (magnification < 1) ? 1 : magnification;
    MAP_LOG("setZoom z%d mag%d (scaledTile=%d)", zoom_, magnification_, scaledTileSize());
    invalidate();
}

zoom_t MapRenderer::findBestZoomWithTiles(const GeoPoint &cntr, zoom_t start) {
    //zoom out until finding tiles for this zoom level
    char path[128];
    for (zoom_t z = start; z > 0; z--) {
        double tx, ty;
        _latLonToTileF(cntr.lat(), cntr.lon(), z, tx, ty);
        snprintf(path, sizeof(path), pathPattern_, z, (int)tx, (int)ty);
        bool has = SD.exists(path);
        MAP_LOG("find zoom %s -> %d", path, has);
        if (has) { return z; } //found
    }
    return start;
}

void MapRenderer::panPx(int16_t dx, int16_t dy) {
    const uint16_t sts   = scaledTileSize(); //already accounts for magnification
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
}


// Private update methods

void MapRenderer::_updateLayers() {
    for (auto& layer : layers_) {
        layer->update();
    }
}

void MapRenderer::_updateTiles(uint32_t now, bool blockingload) {
    if (!obj_) return;
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
    } else MAP_LOG("map queue has %d items", (int)loadQueue_.size());

    _updateLayers();
    redrawIdx_++;
}

MapRenderer::TileGridCtx MapRenderer::_calcTileGrid(double lat, double lon, zoom_t zoom) const {
    double tx, ty;
    _latLonToTileF(lat, lon, zoom, tx, ty);
    TileGridCtx ret = {
        .sts = scaledTileSize(),
        .nTiles = (int)pow(2.0, zoom_),
    };
    ret.tx_s = (int)floor(tx - (double)width_  / 2 / ret.sts);
    ret.ty_s = (int)floor(ty - (double)height_ / 2 / ret.sts);
    ret.px_s = (int)round((ret.tx_s - tx) * ret.sts + width_  / 2.0);
    ret.py_s = (int)round((ret.ty_s - ty) * ret.sts + height_ / 2.0);

    ret.cols = (int)ceil((double)width_  / ret.sts) + 1;
    ret.rows = (int)ceil((double)height_ / ret.sts) + 1;
    return ret;
}

uint8_t MapRenderer::_updateAndQueueTiles(const TileGridCtx& ctx, uint32_t now, bool allowQueue) {
    // Collect missing tiles into loadQueue_ of TileLoadRequest
    uint8_t updated = 0;

    for (int row = 0; row < ctx.rows; row++) {
        for (int col = 0; col < ctx.cols; col++) {

            const int gx = ((ctx.tx_s + col) % ctx.nTiles + ctx.nTiles) % ctx.nTiles;
            const int gy = ctx.ty_s + row;
            if (gy < 0 || gy >= ctx.nTiles) continue;

            const int tpx = ctx.px_s + col * (int)ctx.sts;
            const int tpy = ctx.py_s + row * (int)ctx.sts;

            // Skip tiles entirely outside the canvas
            if (tpx + (int)ctx.sts <= 0 || tpx >= width_)  continue;
            if (tpy + (int)ctx.sts <= 0 || tpy >= height_) continue;

            int idx = _findTile(gx, gy, zoom_);
            if (idx != -1) {
                cache_[idx].update(tpx, tpy, true, magnification_, redrawIdx_);
                updated++;
            } else if (allowQueue) {
                _queueTileRequest(gx, gy, zoom_, now, tpx, tpy);
            }
        }
    }
    return updated;
}

bool MapRenderer::_queueTileRequest(int x, int y, int z, uint32_t now, int16_t px, int16_t py) {
    if (loadQueue_.size() >= TILECACHE_SIZE) return false;
    //TODO check for duplicates
    TileLoadRequest req = {x, y, z, now, px, py};
    loadQueue_.push_back(req);
    return true;
}

bool MapRenderer::_loadOneQueuedTile(uint32_t now) {
    if (!loadQueue_.empty()) {
        const auto m = loadQueue_.front();
        loadQueue_.pop_front();

        int newIdx = _findSlot();
        auto& tile = cache_[newIdx];
        if (newIdx != -1) { // found empty slot
            auto& tile = cache_[newIdx];
            Bounds crop;
            if (cropmode_) {
                crop.left  = (coord_t)std::max(0, (int)(-m.tx / magnification_));
                crop.top   = (coord_t)std::max(0, (int)(-m.ty / magnification_));
                crop.right = (coord_t)std::min((int)tileSize_, (int)((width_  - m.tx) / magnification_));
                crop.bttm  = (coord_t)std::min((int)tileSize_, (int)((height_ - m.ty) / magnification_));
            }

            if (tile.load(m.x, m.y, zoom_, pathPattern_, crop)) {
                if (smartInvert_ && !tile.buffer.isInverted())
                    tile.buffer.doInvert(true);
                lv_img_set_src(tile.img_obj, &tile.dsc_);
                if (m.tx != INT16_MIN && m.ty != INT16_MIN)
                    tile.update(m.tx, m.ty, true, magnification_, redrawIdx_); //straight to screen
                else tile.update(0, 0, false); //offscreen cache load
                return true;
            } else MAP_LOG("ERROR loading tile %d,%d,%d", m.x, m.y, m.z);
        } else MAP_LOG("ERROR: no empty tile slots!");
    }
    return false;
}


// Static geo math

void MapRenderer::_latLonToTileF(double lat, double lon, int z, double& tx, double& ty) {
    double n = pow(2.0, z);
    tx = n * (lon + 180.0) / 360.0;
    double lrad = lat * DEG_2_RAD;
    ty = n * (1.0 - log(tan(lrad) + 1.0/cos(lrad)) / M_PI) / 2.0;
}

double MapRenderer::_latToTileY(double lat, int z) {
    double n = pow(2.0, z);
    double lrad = lat * DEG_2_RAD;
    return n * (1.0 - log(tan(lrad) + 1.0/cos(lrad)) / M_PI) / 2.0;
}

double MapRenderer::_tileYToLat(double ty, int z) {
    double n = pow(2.0, z);
    double sinh_val = sinh(M_PI * (1.0 - 2.0 * ty / n));
    return atan(sinh_val) / DEG_2_RAD;
}

// Cache helpers

int MapRenderer::_findTile(int x, int y, int z) {
    for (int i = 0; i < TILECACHE_SIZE; i++) {
        if (cache_[i].is(x, y, z)) return i;
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

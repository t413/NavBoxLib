#pragma once

#include <stdint.h>
#include <vector>
#include <deque>
#include <lvgl.h>
#include <algorithm>
#include "GeoPoint.h"
#include "PixelBuffer.h"

class MapRenderer;
constexpr double DEFAULT_LAT = 37.8044;
constexpr double DEFAULT_LON = -122.2712;
typedef float zoom_t;
constexpr zoom_t ZOOM_UNCHANGED = -1;
#ifndef TILECACHE_SIZE
#define TILECACHE_SIZE 4
#endif

class MapLayer;
struct Marker;
class MarkerLayer;


class MapRenderer {
public:
    struct TileCacheEntry {
        int z = -1, x = -1, y = -1;
        PixelBuffer buffer;
        lv_draw_buf_t dsc_{};
        lv_obj_t* img_obj = nullptr;
        bool onscreen = false; //set while iterating over tiles
        uint32_t lastUsed = 0;
        bool is(int ox, int oy, int oz) const { return ox == x && oy == y && oz == z; }
        const PixelBuffer* load(int ox, int oy, int oz, const char* fmt, const Bounds &);
        void update(int px, int py, bool visible, zoom_t scale = 1, uint32_t redrawIdx = 0);
        void clear();
    };
    struct XY { int16_t x, y; };
    struct TileLoadRequest { int x, y, z; uint32_t queuedAt; XY dest; };

    MapRenderer() = default;
    ~MapRenderer();

    bool begin(lv_obj_t* parent, uint16_t w, uint16_t h, const char* fmt, uint16_t tileSize = 256);
    void setXY(uint16_t x, uint16_t y);
    void invalidate();
    uint8_t iterate(uint32_t now, bool loadAll = false);

    bool project(double lat, double lon, lv_coord_t& px, lv_coord_t& py) const; /// get display px position of a lat/lon point
    bool isVisible(lv_coord_t px, lv_coord_t py) const;
    zoom_t findBestZoomWithTiles(const GeoPoint &, zoom_t);

    void setCenter(const GeoPoint &, zoom_t zoom = ZOOM_UNCHANGED); ///Sets the map's center view
    void setZoom(zoom_t zoom);
    void panPx(int16_t dx, int16_t dy);
    XY getMarkerPx(uint16_t id) const;
    int16_t getPxDistToCenter(const XY &) const;
    void setSmartInvert(bool smartInvert);
    bool getInverted() const { return smartInvert_; }

    zoom_t zoom() const { return zoom_; }
    zoom_t zoomtotal() const { return zoom_; }
    zoom_t scaledTileSize(zoom_t z) const;
    double lat() const { return mapCenter_.lat(); }
    double lon() const { return mapCenter_.lon(); }
    const GeoPoint& getCenter() const { return mapCenter_; }
    uint16_t getHeight() const { return height_; }
    uint16_t getWidth()  const { return width_; }

    void addLayer(MapLayer* layer);
    void removeLayer(MapLayer* layer);
    MarkerLayer* getMarkerLayer();
    const std::vector<MapLayer*>& getLayers() const { return layers_; }

protected:
    lv_obj_t* obj_ = nullptr;
    int16_t x_ = 0, y_ = 0;
    uint16_t width_ = 0, height_ = 0;
    uint16_t       tileSize_;
    const char*    pathPattern_ = nullptr;
    zoom_t         zoom_ = 12;
    std::vector<TileCacheEntry> cache_;
    std::deque<TileLoadRequest> loadQueue_;
    uint32_t redrawIdx_ = 0;
    std::vector<MapLayer*> layers_;
    MarkerLayer* markerLayer_ = nullptr;
    GeoPoint mapCenter_ = {DEFAULT_LAT, DEFAULT_LON};
    bool smartInvert_ = false;

public: //settings
    uint32_t colBg_ = 0x280b24; // #280b24
    uint32_t colAccent_ = 0x24b9d7;
    uint32_t colHome_ = 0x2ECC71; // #2ECC71
    bool cropmode_ = false;
    MemPool mempool_;
    uint8_t dotsize_ = 14, homesize_ = 16;

public: //utility methods, helpful to unit test
    static void _latLonToTileF(double lat, double lon, int z, double& tx, double& ty);
    static double _latToTileY(double lat, int z);
    static double _tileYToLat(double ty, int z);
    int _findTile(int x, int y, int z, bool allowback);
    int _findSlot();
    XY _calcTileScreenPos(int tx, int ty, int tz) const;
    void _updateTiles(uint32_t now, bool blockingload = false);
    void _updateLayers();
    lv_obj_t* getLvglBase() const { return obj_; }

    struct TileGridCtx {
        zoom_t sts; int cols, rows, tx_s, ty_s, px_s, py_s, nTiles;
    };

    TileGridCtx _calcTileGrid(double lat, double lon, zoom_t zoom) const;
    uint8_t _updateAndQueueTiles(const TileGridCtx&, uint32_t now, bool allowQueue=true);
    bool _queueTileRequest(int x, int y, int z, uint32_t now, MapRenderer::XY dest);
    bool _loadOneQueuedTile(uint32_t now);  // called from iterate(), loads 1 tile with timeout

};

int freeHeap();

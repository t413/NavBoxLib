# NavBoxLib- Maps for embedded devices

Like Leaflet or google maps web view, but for microcontrollers!

[![License](https://img.shields.io/badge/License-GPL--3.0-green?style=flat-square)](https://github.com/t413/navboxlib/blob/master/LICENSE)
![Platformio](https://img.shields.io/badge/Platform-IO-blue?style=flat-square)

<p align="center">
  <a href="https://t413.com/p/projects/NavBoxLib/dropped-2026-05-28T23-23-08-931Z-heart.gif"><img src="https://t413.com/p/projects/NavBoxLib/dropped-2026-05-28T23-23-08-931Z-heart.gif" alt="maps in action"></a>
</p>

- Ultra effecient, works on non-PSRAM micros with minimal RAM
- Overlays for custom map elements
- Markers support for marking/updating points on the map
- GPX file reading & writing for path loading/viewing/saving
- Track simplification engine
- Supports zooming past available tiles with magnification zoom
- Platform and framework independent, runs on ESP32, desktops, etc
- Unit testable and unit tested, this repo is also runnable as a platformio project that runs unit tests

## Library Installation

For platformio projects it's easy!

```ini
lib_deps =
    https://github.com/t413/NavBoxLib.git
```

## Efficiency

Traditional embedded tile renderers: 4 × 256×256 tiles, RGB565 is 2-bytes = **524 KB RAM**

NavBoxLib: Without PSRAM it only stores *visible* portions of tiles. **~80–180 KB**

## Examples

This is the primary backbone of my [NavBox](https://github.com/t413/NavBoxESP) offline GPS firmware for the ESP32. Check out that project for a full example!

[Unit tests](https://github.com/t413/navboxlib/tree/main/test) each demo realistic uses of each compodent and parts working together, check them out!

```cpp
#include <navboxlib/MapRenderer.h>
MapRenderer map;

void main() {
    // <lvgl setup>
    map.begin(baseLvglObj, 300, 200, "/maps/tiles/%d/%d/%d.png");
    map.setCenter(TrackPoint(37.871, -122.317), 16);
    // <lvgl loop>
}
```

## Map Tiles

Map tiles are standard unmodified OpenStreetMap 256x256 tiles, [documentation here](https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames). You can bach download tiles:

- Using my python tool [osm2edgetx](https://github.com/t413/osm2edgetx) from the command line

  * Download the [lastest version](https://github.com/t413/osm2edgetx/archive/refs/heads/master.zip) and run something like:

      ```bash
      cd /Volumes/SD_CARD_PATH/maps/osm/
      python osm2edgetx.py --osm . --fetch "37.87,-122.32" --radius 5 --zoom 17
      ```
- Using my [AlphaMap Web Tile Downloader](https://t413.com/go/alphamaptool?ref=gh) designed for my [AlphaMapLua](https://github.com/t413/AlphaMapLua) project. It downloads the same tiles, though into `WIDGETS/AlphaMapLua/tiles`.


## License & Contributing

This project is GPLv3. The hope is to encourage other *open source* embedded projects.

If adoption/AI-rewriting becomes an issue the license _may_ be changed to LGPL. Any contributions are with this possible relicense in mind, and the decision ultimately stays with the original developer.

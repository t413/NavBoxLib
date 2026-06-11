#include <gtest/gtest.h>
#include "fixtures.h"
#include <navboxlib/log.h>
#include <navboxlib/MapRenderer.h>
#include <navboxlib/MapLayer.h>
#include <navboxlib/TrackLog.h>
#include <msf_gif.h>

using namespace std;
using namespace fixtures;

TEST(MapRenderer, AnimateSpiralZoom) {
    fixtures::LvglTestEnv env(600, 200);
    int w = env.width_;
    int h = env.height_;
    const auto center = TEST_CENTER.fromDistHeading(130, 40);

    MapRenderer map;
    string tfmt = string(OSM_TILES_STASH) + "/%d/%d/%d.png";
    auto dir = env.outdir();
    filesystem::create_directories(dir);
    auto gifpath = dir / ("test_" + testname() + ".gif");
    map.begin(env.base_, env.width_, env.height_, tfmt.c_str());

    TrackLog track("/tmp");
    TrackLayer tl(&map, 0xFF0000, &track);
    map.addLayer(&tl);

    auto& mkrs = *(map.getMarkerLayer());
    auto dotidx  = mkrs.add(Marker(center, 16));
    auto homeidx = mkrs.add(Marker(TEST_CENTER, 18, 0x00ff00, 'H' ));

    track.newRecording(1936610000);
    map.setCenter(center.fromDistHeading(280, 270)); //500m west
    map.setSmartInvert(false);

    GifMaker gif(gifpath.string(), w, h);

    int total_frames = 120;
    for (int i = 0; i < total_frames; i++) {
        float progress = (float)i / total_frames;
        float easedProgress = progress * (2.0f - progress);
        float zoom = 14.0f + (easedProgress * 3.0f); // 14 to 18

        map.panPx(1, 0); // Move center slowly in X (longitude)
        map.setZoom(zoom);
        if (i == 60) map.setSmartInvert(!map.getInverted());

        // 2. Draw Heart Pattern (Parametric Heart Equation)
        float t = progress * 2.0f * M_PI;
        // Heart shape: x = 16sin^3(t), y = 13cos(t) - 5cos(2t) - 2cos(3t) - cos(4t)
        float hx = 16.0f * pow(sin(t), 3);
        float hy = 13.0f * cos(t) - 5.0f * cos(2*t) - 2.0f * cos(3*t) - cos(4*t);
        float angle = atan2(hx, hy) * (180.0 / M_PI);
        float dist = sqrt(hx*hx + hy*hy) * 5.0f; // scale up

        auto p = center.fromDistHeading(dist, angle);
        track.addPoint(p);
        mkrs.updatePoint(dotidx, p);
        map.iterate(1000, true);
        env.draw();
        gif.addFrame(env.disp_);
        MAP_LOG("frame %d/%d, z%0.1f", i, total_frames, zoom);
    }
    gif.finish();
    track.stopRecording();
    map.removeLayer(&tl);
    remove(track.getRecPath());
    MAP_LOG("finished to %s", gifpath.c_str());
}

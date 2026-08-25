package com.winlator.star.renderer;

import com.winlator.star.widget.XServerView;

public interface HostRenderer {
    XServerView getXServerView();
    void setRenderingEnabled(boolean enabled);
    void requestRender();
    void forceCleanup();
    void setCursorVisible(boolean visible);
    boolean isCursorVisible();
    void setUnviewableWMClasses(String wmClasses);
    void setFilterMode(int mode);
    void setMagnifierZoom(float zoom);
    float getMagnifierZoom();
    void toggleFullscreen();
    boolean isFullscreen();
    // Fullscreen aspect-ratio mode (issue #71): Container.FULLSCREEN_OFF/FIT/STRETCH/...
    // isFullscreen() stays == (mode != OFF) so existing upscaler/magnifier gates behave as before.
    void setFullscreenMode(int mode);
    int getFullscreenMode();
    // Screen alignment (issue #413): Container.ALIGN_CENTER/TOP/BOTTOM — vertical placement of the
    // letterbox rect. CENTER is the historical behavior; only moves the bar, never the scale.
    void setScreenAlignment(int alignment);
    int getScreenAlignment();
    // OSC fit transform (issue #413): the on-screen game viewport expressed as fractions [0..1] of the
    // host surface (left/top/right/bottom), so the touch-controls overlay can locate the pooled letterbox
    // band without depending on the renderer's px surface size (surface px != InputControlsView px). Fills
    // `out` in place. STRETCH / an unsized surface report the full region (0,0,1,1); TOP/BOTTOM report the
    // vertically-shifted rect; FILL reports an over-scan rect (offsets < 0 / > 1) so callers treat it as
    // "no band". This is a read-only query — it never changes renderer state.
    void getGameViewportNormalized(android.graphics.RectF out);
    void setScreenOffsetYRelativeToCursor(boolean b);
    boolean isScreenOffsetYRelativeToCursor();
    void setFpsWindowId(int id);
    void setFrameRating(Object fr);
    int getFpsLimit();
    void setFpsLimit(int limit);
    int getSurfaceWidth();
    int getSurfaceHeight();
}

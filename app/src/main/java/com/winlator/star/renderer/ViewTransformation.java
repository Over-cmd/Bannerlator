package com.winlator.star.renderer;

import com.winlator.star.container.Container;

public class ViewTransformation {
    public int viewOffsetX;
    public int viewOffsetY;
    public int viewWidth;
    public int viewHeight;
    public float aspect;
    public float sceneScaleX;
    public float sceneScaleY;
    public float sceneOffsetX;
    public float sceneOffsetY;

    // Legacy entry point: letterbox (preserve aspect, center with bars). Same as OFF/FIT.
    public void update(int outerWidth, int outerHeight, int innerWidth, int innerHeight) {
        update(outerWidth, outerHeight, innerWidth, innerHeight, Container.FULLSCREEN_FIT, Container.ALIGN_CENTER);
    }

    // 5-arg overload: alignment defaults to CENTER (== legacy behavior) for callers that don't specify it.
    public void update(int outerWidth, int outerHeight, int innerWidth, int innerHeight, int fullscreenMode) {
        update(outerWidth, outerHeight, innerWidth, innerHeight, fullscreenMode, Container.ALIGN_CENTER);
    }

    // Fullscreen aspect-ratio mode (#71) + vertical alignment (#413). A single `aspect` scale factor
    // drives the whole mapping; fullscreenMode only chooses that scale, screenAlignment only moves the
    // resulting rect vertically. CENTER is byte-identical to the historical output.
    public void update(int outerWidth, int outerHeight, int innerWidth, int innerHeight, int fullscreenMode, int screenAlignment) {
        float sx = (float)outerWidth / innerWidth;
        float sy = (float)outerHeight / innerHeight;

        switch (fullscreenMode) {
            case Container.FULLSCREEN_FILL:
                aspect = Math.max(sx, sy);
                break;
            case Container.FULLSCREEN_INTEGER:
                aspect = Math.max(1.0f, (float)Math.floor(Math.min(sx, sy)));
                break;
            case Container.FULLSCREEN_OFF:
            case Container.FULLSCREEN_FIT:
            default:
                aspect = Math.min(sx, sy);
                break;
        }

        viewWidth = (int)Math.ceil(innerWidth * aspect);
        viewHeight = (int)Math.ceil(innerHeight * aspect);
        viewOffsetX = (int)((outerWidth - innerWidth * aspect) * 0.5f);

        sceneScaleX = (innerWidth * aspect) / outerWidth;
        sceneScaleY = (innerHeight * aspect) / outerHeight;
        sceneOffsetX = (innerWidth - innerWidth * sceneScaleX) * 0.5f;

        // Vertical placement of the letterbox rect. CENTER reproduces the exact historical formulas.
        // TOP pins the game to y=0; BOTTOM sits it flush to the bottom using the already-ceiled
        // viewHeight (avoids a 1px overflow). sceneOffsetY is kept consistent with viewOffsetY in guest
        // units (sceneOffsetY = viewOffsetY * innerHeight/outerHeight) so touch mapping + scene render
        // stay aligned. FILL uses a negative gap (crop overflow) and STRETCH never reaches here, so for
        // those TOP/BOTTOM are inert — there is no bar to move.
        float sceneGapY = innerHeight - innerHeight * sceneScaleY;
        switch (screenAlignment) {
            case Container.ALIGN_TOP:
                viewOffsetY = 0;
                sceneOffsetY = 0f;
                break;
            case Container.ALIGN_BOTTOM:
                viewOffsetY = outerHeight - viewHeight;
                sceneOffsetY = sceneGapY;
                break;
            case Container.ALIGN_CENTER:
            default:
                viewOffsetY = (int)((outerHeight - innerHeight * aspect) * 0.5f);
                sceneOffsetY = sceneGapY * 0.5f;
                break;
        }
    }
}

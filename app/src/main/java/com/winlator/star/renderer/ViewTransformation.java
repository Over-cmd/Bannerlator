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

    // Fullscreen aspect-ratio mode (#71) + handheld 50/50 split (#413). A single `aspect` scale factor
    // drives the whole mapping; fullscreenMode only chooses that scale, screenAlignment only moves the
    // resulting rect vertically. CENTER is byte-identical to the historical output.
    //
    // #413 handheld split: on a near-square foldable panel, TOP/BOTTOM under a plain letterbox mode
    // (OFF/FIT) fit the game into HALF the panel height (aspect preserved, pillar/letterboxed WITHIN that
    // half like a normal phone panel) and pin it to that half, so the OTHER half is free for a full-size
    // on-screen controller. Only OFF/FIT qualify — FILL/INTEGER/STRETCH have no plain bar to split, so for
    // those TOP/BOTTOM keep their historical pin (inert). CENTER never splits (gameOuterHeight==outerHeight
    // -> sy, aspect and every derived value identical to before).
    public void update(int outerWidth, int outerHeight, int innerWidth, int innerHeight, int fullscreenMode, int screenAlignment) {
        boolean handheldSplit = (screenAlignment == Container.ALIGN_TOP || screenAlignment == Container.ALIGN_BOTTOM)
                && (fullscreenMode == Container.FULLSCREEN_OFF || fullscreenMode == Container.FULLSCREEN_FIT);
        int gameOuterHeight = handheldSplit ? outerHeight / 2 : outerHeight;

        float sx = (float)outerWidth / innerWidth;
        // Fit against the half panel height when splitting; == outerHeight (so identical) otherwise.
        float sy = (float)gameOuterHeight / innerHeight;

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

        // Vertical placement of the letterbox rect. CENTER reproduces the EXACT historical formulas (do
        // not touch). TOP/BOTTOM: when splitting, center the half-size game inside its half; otherwise keep
        // the historical top/bottom pin. sceneOffsetY is kept consistent with viewOffsetY in guest units
        // (sceneOffsetY = viewOffsetY * innerHeight/outerHeight) so touch mapping + scene render stay
        // aligned — this general form equals sceneGapY*0.5f at CENTER, but CENTER keeps its own line.
        float sceneGapY = innerHeight - innerHeight * sceneScaleY;
        switch (screenAlignment) {
            case Container.ALIGN_TOP:
                viewOffsetY = handheldSplit ? (gameOuterHeight - viewHeight) / 2 : 0;
                sceneOffsetY = viewOffsetY * (float)innerHeight / outerHeight;
                break;
            case Container.ALIGN_BOTTOM:
                viewOffsetY = handheldSplit ? outerHeight / 2 + (gameOuterHeight - viewHeight) / 2
                                            : outerHeight - viewHeight;
                sceneOffsetY = viewOffsetY * (float)innerHeight / outerHeight;
                break;
            case Container.ALIGN_CENTER:
            default:
                viewOffsetY = (int)((outerHeight - innerHeight * aspect) * 0.5f);
                sceneOffsetY = sceneGapY * 0.5f;
                break;
        }
    }
}

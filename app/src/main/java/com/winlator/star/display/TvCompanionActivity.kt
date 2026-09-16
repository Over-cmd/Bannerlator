package com.winlator.star.display

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Display
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winlator.star.core.AppUtils
import com.winlator.star.ui.XServerDrawerState
import com.winlator.star.ui.theme.AppThemeState
import com.winlator.star.ui.theme.WinlatorTheme

/**
 * The handheld's companion screen while a game plays on the TV ("Launch this game on the TV" — see
 * [ExternalDisplay]). It says what is happening, asks the user not to touch the handheld, and gives
 * them the two things they may still need: put input back on the TV, and end the game.
 *
 * Why it exists: Android keeps exactly ONE focused display, and the physical controller follows it.
 * With the game on the TV the handheld was left showing the games list, so a stray touch there moved
 * focus to the handheld and the pad stopped reaching the game — device-proven with `input -d <id> tap`,
 * which moves `mTopFocusedDisplayId` to the display it taps. A games list cannot tell the user any of
 * that, so this screen stands in its place and says it.
 *
 * Why it cannot steal focus itself — two independent reasons, both deliberate:
 * - it is started BEFORE the session, from `launchOnExternalDisplay`, so the session on the TV is the
 *   last activity started and is the one that ends up focused;
 * - its window carries FLAG_NOT_FOCUSABLE, so it never becomes a display's focused window and touching
 *   it cannot hand the handheld the top focus either. Touch events still reach the buttons (only
 *   FLAG_NOT_TOUCHABLE would stop those), which is why nothing here is controller-operated: the pad
 *   belongs to the TV.
 *
 * Lifetime: started by the launch path, dismissed by the SESSION's own lifecycle
 * (XServerDisplayActivity: a declined TV launch, the TV going away, exit, destroy) rather than by
 * anyone guessing from out here. [show] arms a watchdog as the one backstop for a session that never
 * arrives at all, so a failed launch can never leave a screen stuck on the handheld.
 */
class TvCompanionActivity : ComponentActivity() {

    private var gameName = ""
    private var displayName = ""
    /** The display the session is on — where "Send input back to the TV" aims it. -1 = unknown. */
    @Volatile private var sessionDisplayId = -1
    /** A copy of the intent that launched the session, replayed to bring it back to the front. */
    private var sessionIntent: Intent? = null

    private val watchdogHandler = Handler(Looper.getMainLooper())
    private val watchdog = Runnable {
        if (!sessionConfirmed) {
            Log.i(TAG, "no session claimed the external display — closing the companion")
            finishSafely()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppThemeState.init(this)   // in case MainActivity hasn't run yet in this process
        // Never take key focus (see the class comment): the controller follows the focused display.
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
        // A game is playing: don't let the handheld doze off and take the session down with it.
        AppUtils.keepScreenOn(this)
        live = this
        readExtras(intent)
        watchdogHandler.postDelayed(watchdog, SESSION_CONFIRM_TIMEOUT_MS)

        setContent {
            WinlatorTheme {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    TvCompanionScreen(
                        gameName = gameName,
                        displayName = displayName,
                        onSendInputBack = { sendInputBackToTv() },
                        onEndGame = { endGameSession() },
                    )
                }
            }
        }
    }

    /** singleTask: a second launch (a relaunch of the same game) lands here, not in a new instance. */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        readExtras(intent)
        watchdogHandler.removeCallbacks(watchdog)
        watchdogHandler.postDelayed(watchdog, SESSION_CONFIRM_TIMEOUT_MS)
    }

    override fun onResume() {
        super.onResume()
        // The session dismisses this screen itself; this is only the backstop for the case where that
        // never arrives (the session's process died) — a companion pointing at a screen that is gone
        // must not be left sitting on the handheld.
        if (sessionConfirmed && sessionDisplayId >= 0
            && ExternalDisplay.byId(this, sessionDisplayId) == null) {
            Log.i(TAG, "display " + sessionDisplayId + " is gone — closing the companion")
            finishSafely()
        }
    }

    override fun onDestroy() {
        watchdogHandler.removeCallbacks(watchdog)
        if (live === this) live = null
        super.onDestroy()
    }

    private fun readExtras(from: Intent) {
        try {
            gameName = from.getStringExtra(EXTRA_GAME_NAME).orEmpty()
            displayName = from.getStringExtra(EXTRA_DISPLAY_NAME).orEmpty()
            sessionDisplayId = from.getIntExtra(EXTRA_DISPLAY_ID, -1)
            @Suppress("DEPRECATION")
            sessionIntent = from.getParcelableExtra<Intent>(EXTRA_SESSION_INTENT)
        } catch (t: Throwable) {
            Log.w(TAG, "could not read the companion's extras", t)
        }
        if (displayName.isEmpty()) {
            displayName = ExternalDisplay.title(ExternalDisplay.byId(this, sessionDisplayId))
        }
    }

    /**
     * Put the game back in front on its own display, which is what makes the system focus that display
     * again — and with it the controller. The session activity is `singleTask`, so replaying the intent
     * that launched it brings the RUNNING instance forward (its default `onNewIntent` does nothing); it
     * never starts a second session or restarts the guest.
     *
     * Works from this non-focusable window: starting an activity needs the APP to be in the foreground,
     * not this window to hold focus, and both this screen and the session are visible.
     */
    private fun sendInputBackToTv() {
        val target = sessionIntent
        val displayId = sessionDisplayId
        if (target == null || displayId < 0) {
            Log.w(TAG, "nothing to send back: session intent " + (target != null) + ", display " + displayId)
            return
        }
        try {
            startActivity(Intent(target), ExternalDisplay.launchOptions(displayId))
        } catch (t: Throwable) {
            // Nothing to fall back to — the game is still playing over there, the user simply keeps
            // the pad on the handheld's display until they touch the TV's task again.
            Log.w(TAG, "could not bring the session back to display " + displayId, t)
        }
    }

    /**
     * End the game the same way the in-game drawer's Exit button does — the session's own `exit()`, via
     * the callback it publishes while it is running. Never a second teardown path: when the callback is
     * gone the session is already on its way out and there is nothing to stop.
     */
    private fun endGameSession() {
        try {
            XServerDrawerState.onExit?.run()
        } catch (t: Throwable) {
            Log.w(TAG, "could not end the session", t)
        }
        // The session dismisses this screen as it tears down; close now anyway so the handheld is not
        // left looking at a companion for a game that is shutting down.
        finishSafely()
    }

    private fun finishSafely() {
        try {
            if (!isFinishing && !isDestroyed) finish()
        } catch (t: Throwable) {
            Log.w(TAG, "could not close the companion", t)
        }
    }

    companion object {
        private const val TAG = "TvCompanion"

        private const val EXTRA_GAME_NAME = "game_name"
        private const val EXTRA_DISPLAY_NAME = "display_name"
        private const val EXTRA_DISPLAY_ID = "display_id"
        private const val EXTRA_SESSION_INTENT = "session_intent"

        /** How long the companion waits for a session to say it really is on the TV before giving up. */
        private const val SESSION_CONFIRM_TIMEOUT_MS = 20_000L

        /** The instance on screen, or null. Written on the main thread; every reader posts there too. */
        @Volatile private var live: TvCompanionActivity? = null
        /** Set once a session has reported that its window really is on the external display. */
        @Volatile private var sessionConfirmed = false

        private val mainHandler = Handler(Looper.getMainLooper())

        /**
         * Put the companion up on the screen the user is looking at, for a session about to start on
         * [display]. Called from the launch path BEFORE the session is started, so the session is the
         * last activity started and the TV is the display that ends up focused.
         *
         * [sessionIntent] is the exact intent the session is launched with; it is replayed by
         * "Send input back to the TV". Never throws: a companion that cannot come up must not stop a
         * game from launching.
         */
        @JvmStatic
        fun show(activity: Activity, gameName: String?, display: Display, sessionIntent: Intent) {
            try {
                sessionConfirmed = false
                val intent = Intent(activity, TvCompanionActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    .putExtra(EXTRA_GAME_NAME, gameName.orEmpty())
                    .putExtra(EXTRA_DISPLAY_NAME, ExternalDisplay.title(display))
                    .putExtra(EXTRA_DISPLAY_ID, display.displayId)
                    .putExtra(EXTRA_SESSION_INTENT, Intent(sessionIntent))
                // Explicitly the display the LAUNCHER is on, not DEFAULT_DISPLAY: on a DeX-style setup
                // the app itself runs on a second screen, and the companion belongs on whichever screen
                // the user just tapped from. ExternalDisplay.find() never returns that display, so this
                // is always the other one.
                activity.startActivity(intent,
                    ExternalDisplay.launchOptions(ExternalDisplay.currentDisplayId(activity)))
            } catch (t: Throwable) {
                Log.w(TAG, "could not show the companion screen", t)
            }
        }

        /** A session has confirmed its window is on [displayId] — the companion has something to point at. */
        @JvmStatic
        fun onSessionOnTv(displayId: Int) {
            sessionConfirmed = true
            mainHandler.post { live?.sessionDisplayId = displayId }
        }

        /**
         * Take the companion down: the session was refused the TV, lost it, ended, or is gone. Safe to
         * call at any time, from any thread, with or without a companion on screen.
         */
        @JvmStatic
        fun dismiss(reason: String) {
            sessionConfirmed = false
            mainHandler.post {
                val activity = live ?: return@post
                Log.i(TAG, "closing the companion: " + reason)
                activity.finishSafely()
            }
        }
    }
}

/**
 * One calm screen: what is playing, where, why not to touch this one, and the two actions. Deliberately
 * not a dashboard — the user is looking at the TV, and this is what they see out of the corner of an eye
 * when they pick the handheld up.
 *
 * The two actions are far apart on purpose: the whole premise is that someone touches this screen by
 * accident, so the big central target is the harmless one and ending the game is a low-emphasis button
 * at the bottom.
 */
@Composable
private fun TvCompanionScreen(
    gameName: String,
    displayName: String,
    onSendInputBack: () -> Unit,
    onEndGame: () -> Unit,
) {
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(
            modifier = Modifier
                .widthIn(max = 520.dp)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 28.dp, vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "EXTERNAL DISPLAY CONNECTED",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                letterSpacing = 2.sp,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(14.dp))
            Text(
                if (gameName.isNotEmpty()) gameName else "Your game",
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(6.dp))
            Text(
                "Playing on " + (if (displayName.isNotEmpty()) displayName else "the external display"),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )

            Spacer(Modifier.height(28.dp))

            Card(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(14.dp),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
            ) {
                Column(modifier = Modifier.padding(18.dp)) {
                    Text(
                        "Please don't touch this screen",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "Android gives the controller to the screen you last touched. A tap here takes it " +
                            "away from the TV, and the game stops answering the pad. Play with the controller " +
                            "and leave the handheld alone.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Spacer(Modifier.height(28.dp))

            Button(
                onClick = onSendInputBack,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(64.dp),
                shape = RoundedCornerShape(14.dp),
            ) {
                Text("Send input back to the TV", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(10.dp))
            Text(
                "Already touched it? This puts the game back in front on the TV, and the controller " +
                    "follows it there.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )

            Spacer(Modifier.height(36.dp))

            TextButton(onClick = onEndGame) {
                Text(
                    "End the game",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

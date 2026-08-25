package com.winlator.star.store

import android.net.Uri
import android.net.http.SslError
import android.os.Bundle
import android.util.Log
import android.webkit.ConsoleMessage
import android.webkit.SslErrorHandler
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import com.winlator.star.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

class GogLoginActivity : ComponentActivity() {

    companion object {
        private const val TAG = "BH_GOG"
        // This config mirrors the proven-working star-compose store-integration
        // GogLoginActivity (the version that rendered the GOG login fine). Keep it
        // exactly: layout=client2 + the GOG Galaxy UA + a plain full-screen WebView
        // (setContentView). The white screen was a regression from the marcescence
        // Compose rewrite that hosted the WebView in a Compose AndroidView and then
        // mutated these params (drop layout / change UA / third-party cookies) — all
        // dead ends. Do NOT reintroduce those changes.
        const val AUTH_URL =
            "https://auth.gog.com/auth" +
            "?client_id=46899977096215655" +
            "&redirect_uri=https%3A%2F%2Fembed.gog.com%2Fon_login_success%3Forigin%3Dclient" +
            "&response_type=token&layout=client2"

        private const val KEY_STATE = "bh_gog_oauth_state"

        /** Appends the CSRF [state] to the base AUTH_URL. */
        @JvmStatic
        fun buildAuthUrl(state: String): String = "$AUTH_URL&state=${Uri.encode(state)}"

        /** Random URL-safe CSRF state (24 bytes → ~32 chars, well over the 16-byte floor). */
        @JvmStatic
        fun generateState(): String {
            val bytes = ByteArray(24)
            java.security.SecureRandom().nextBytes(bytes)
            return android.util.Base64.encodeToString(
                bytes,
                android.util.Base64.URL_SAFE or android.util.Base64.NO_WRAP or android.util.Base64.NO_PADDING,
            )
        }

        @JvmStatic
        fun parseJsonStringField(json: String?, key: String?): String? {
            if (json == null || key == null) return null
            val search = "\"$key\":\""
            val idx = json.indexOf(search)
            if (idx < 0) return null
            val start = idx + search.length
            val end = json.indexOf('"', start)
            if (end < 0) return null
            return json.substring(start, end)
        }
    }

    private var webViewRef: WebView? = null

    // CSRF state we sent on AUTH_URL; must survive WebView/Activity recreation.
    private var oauthState: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        oauthState = savedInstanceState?.getString(KEY_STATE) ?: generateState()
        val webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.userAgentString = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) GOG Galaxy/2.0"
            webViewClient = GogWebViewClient()
            webChromeClient = object : WebChromeClient() {
                override fun onConsoleMessage(cm: ConsoleMessage): Boolean {
                    Log.d(TAG, "console[${cm.messageLevel()}] ${cm.message()} @${cm.sourceId()}:${cm.lineNumber()}")
                    return true
                }
                override fun onProgressChanged(view: WebView?, newProgress: Int) {
                    Log.d(TAG, "progress=$newProgress")
                }
            }
        }
        webViewRef = webView
        setContentView(webView)
        webView.loadUrl(buildAuthUrl(oauthState!!))
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        oauthState?.let { outState.putString(KEY_STATE, it) }
    }

    private inner class GogWebViewClient : WebViewClient() {

        override fun onPageStarted(view: WebView?, url: String?, favicon: android.graphics.Bitmap?) {
            Log.d(TAG, "pageStarted: ${StoreLog.redactUrl(url)}")
        }

        override fun onPageFinished(view: WebView?, url: String?) {
            Log.d(TAG, "pageFinished: ${StoreLog.redactUrl(url)}")
        }

        override fun onReceivedError(view: WebView?, request: WebResourceRequest?, error: WebResourceError?) {
            Log.e(TAG, "recvError: ${error?.errorCode} ${error?.description} url=${StoreLog.redactUrl(request?.url?.toString())} mainFrame=${request?.isForMainFrame}")
        }

        override fun onReceivedHttpError(view: WebView?, request: WebResourceRequest?, errorResponse: WebResourceResponse?) {
            Log.e(TAG, "recvHttpError: ${errorResponse?.statusCode} ${errorResponse?.reasonPhrase} url=${StoreLog.redactUrl(request?.url?.toString())} mainFrame=${request?.isForMainFrame}")
        }

        override fun onReceivedSslError(view: WebView?, handler: SslErrorHandler?, error: SslError?) {
            Log.e(TAG, "recvSslError: $error")
            // Do NOT proceed() — a real cert error should surface, not be silently bypassed.
            handler?.cancel()
        }

        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            val uri = request.url
            if (uri.toString().startsWith("https://embed.gog.com/on_login_success")) {
                handleImplicitRedirect(uri)
                return true
            }
            return false
        }

        @Suppress("DEPRECATION")
        override fun shouldOverrideUrlLoading(view: WebView, url: String): Boolean {
            if (url.startsWith("https://embed.gog.com/on_login_success")) {
                handleImplicitRedirect(Uri.parse(url))
                return true
            }
            return false
        }

        private fun handleImplicitRedirect(uri: Uri) {
            val fragment = uri.fragment ?: return
            val frag = Uri.parse("x://x?$fragment")

            // CSRF: the auth server echoes back the `state` we sent. Reject a redirect
            // whose state is missing or does not match (defeats forged login responses).
            // NOTE: depends on auth.gog.com echoing `state` in the implicit-flow fragment.
            // If device-test shows valid logins being rejected, relax to mismatch-only
            // (i.e. allow a null returnedState).
            val expected = oauthState
            val returnedState = frag.getQueryParameter("state")
            if (expected != null && returnedState != expected) {
                Log.e(TAG, "OAuth state mismatch (got=${returnedState != null}) — rejecting redirect")
                rejectLogin("Login verification failed, please try again")
                return
            }

            val accessToken = frag.getQueryParameter("access_token") ?: return
            val refreshToken = frag.getQueryParameter("refresh_token")
            val userId = frag.getQueryParameter("user_id")

            webViewRef?.loadData(
                "<html><body style='background:#000;color:#ccc;font-family:sans-serif;" +
                "font-size:20px;text-align:center;padding-top:40%'>" +
                "Logging in to GOG...</body></html>",
                "text/html", "UTF-8",
            )

            lifecycleScope.launch(Dispatchers.IO) {
                loginRunnable(accessToken, refreshToken, userId)
            }
        }
    }

    /** Shows a themed error and reloads the login page with a fresh CSRF state. */
    private fun rejectLogin(message: String) {
        runOnUiThread {
            if (!isFinishing && !isDestroyed) {
                android.app.AlertDialog.Builder(this, R.style.StoreAlertDialogDark)
                    .setMessage(message)
                    .setPositiveButton("OK", null)
                    .show()
            }
            val fresh = generateState()
            oauthState = fresh
            webViewRef?.loadUrl(buildAuthUrl(fresh))
        }
    }

    private suspend fun loginRunnable(accessToken: String, refreshToken: String?, userId: String?) {
        try {
            var username = "Unknown"
            try {
                val url = URL("https://embed.gog.com/userData.json")
                val conn = url.openConnection() as HttpURLConnection
                conn.connectTimeout = 15000
                conn.readTimeout = 15000
                conn.setRequestProperty("Authorization", "Bearer $accessToken")
                val sb = StringBuilder()
                BufferedReader(InputStreamReader(conn.inputStream, "UTF-8")).use { br ->
                    var line: String?
                    while (br.readLine().also { line = it } != null) sb.append(line)
                }
                conn.disconnect()
                val parsed = parseJsonStringField(sb.toString(), "username")
                if (parsed != null) username = parsed
            } catch (_: Exception) {}

            val ed = getSharedPreferences("bh_gog_prefs", 0).edit()
            ed.putString("access_token", accessToken)
            if (refreshToken != null) ed.putString("refresh_token", refreshToken)
            if (userId != null) ed.putString("user_id", userId)
            ed.putString("username", username)
            val nowSec = System.currentTimeMillis() / 1000L
            ed.putInt("bh_gog_login_time", nowSec.toInt())
            ed.putInt("bh_gog_expires_in", 3600)
            ed.apply()

            Log.d(TAG, "GOG login saved OK")   // don't log username (PII)
            withContext(Dispatchers.Main) { finish() }
        } catch (e: Exception) {
            Log.e(TAG, "Login post-processing failed", e)
            withContext(Dispatchers.Main) {
                // Not a Compose screen (plain WebView via setContentView), so the shared
                // UninstallResultBar can't apply — use the themed dark dialog instead of a
                // black-box Toast.
                if (!isFinishing && !isDestroyed) {
                    android.app.AlertDialog.Builder(this@GogLoginActivity, R.style.StoreAlertDialogDark)
                        .setMessage("Login error, please try again")
                        .setPositiveButton("OK", null)
                        .show()
                }
                val fresh = generateState()
                oauthState = fresh
                webViewRef?.loadUrl(buildAuthUrl(fresh))
            }
        }
    }
}

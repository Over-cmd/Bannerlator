package com.winlator.star.ui.screens.contents

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.winlator.star.container.ContainerManager
import com.winlator.star.linux.LinuxRuntime
import com.winlator.star.linux.LinuxRuntimeInstaller
import com.winlator.star.linux.LinuxShortcuts
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.runtime.rememberCoroutineScope

/**
 * The Linux runtime: one opt-in download that turns on gamescope sessions and Valve's native arm64
 * Steam client. It is a whole second userland, so it is deliberately not mixed in with the `.wcp`
 * components — nothing fetches it until the user asks.
 */
@Composable
fun LinuxRuntimeTab() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var installed by remember { mutableStateOf(LinuxRuntimeInstaller.installedVersion(context)) }
    var release by remember { mutableStateOf<LinuxRuntimeInstaller.Release?>(null) }
    var checking by remember { mutableStateOf(true) }
    var busy by remember { mutableStateOf(false) }
    var stage by remember { mutableStateOf("") }
    var percent by remember { mutableStateOf(-1) }
    var message by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        release = withContext(Dispatchers.IO) { LinuxRuntimeInstaller.fetchRelease() }
        checking = false
    }

    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Linux runtime", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Runs Linux programs instead of Windows ones: a glibc rootfs with gamescope as "
                        + "the session compositor, and Valve's Steam client built for ARM. Games run "
                        + "through Steam's own Proton, not through this app's Wine. Optional — "
                        + "nothing else changes if you leave it off.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Text(
                    when {
                        installed != null -> "Installed: ${installed}"
                        checking -> "Checking for a build…"
                        release == null -> "No build available (could not reach the catalog)"
                        else -> "Not installed"
                    },
                    style = MaterialTheme.typography.labelLarge
                )
                release?.let { r ->
                    val size = if (r.size > 0) " · ${r.size / (1024 * 1024)} MB download" else ""
                    Text("Available: ${r.version}$size",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }

                if (busy) {
                    Spacer(Modifier.height(4.dp))
                    Text(stage, style = MaterialTheme.typography.labelSmall)
                    if (percent in 0..100) {
                        LinearProgressIndicator(progress = { percent / 100f },
                            modifier = Modifier.fillMaxWidth())
                    } else {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    }
                }

                message?.let {
                    Text(it, style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.error)
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    val target = release
                    Button(
                        enabled = !busy && target != null
                            && (installed == null || installed != target.version),
                        onClick = {
                            val r = target ?: return@Button
                            busy = true; message = null; stage = "Starting…"; percent = -1
                            scope.launch {
                                val ok = withContext(Dispatchers.IO) {
                                    LinuxRuntimeInstaller.install(context, r) { s, p ->
                                        stage = s; percent = p
                                    }
                                }
                                if (ok) {
                                    // The runtime is only useful with something to launch, so the
                                    // Steam entry appears in the library the moment it lands.
                                    withContext(Dispatchers.IO) { addSteamEntry(context) }
                                    installed = LinuxRuntimeInstaller.installedVersion(context)
                                    message = null
                                } else {
                                    message = "Install failed — the existing runtime was left alone."
                                }
                                busy = false
                            }
                        }
                    ) { Text(if (installed == null) "Install" else "Update") }

                    OutlinedButton(
                        enabled = !busy && installed != null,
                        onClick = {
                            busy = true
                            scope.launch {
                                withContext(Dispatchers.IO) {
                                    LinuxRuntimeInstaller.uninstall(context)
                                    removeSteamEntry(context)
                                }
                                installed = null
                                busy = false
                            }
                        }
                    ) { Text("Remove") }
                }

                if (installed != null) {
                    Text(
                        "\"${LinuxShortcuts.STEAM_NAME}\" is in your Games tab. Sign in there with "
                            + "your Steam account; installs and updates are Steam's own.",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

/** Puts the Steam entry in the first container, so the Games tab has something to launch. */
private fun addSteamEntry(context: android.content.Context) {
    runCatching {
        val manager = ContainerManager(context)
        val container = manager.containers.firstOrNull() ?: return
        if (!LinuxShortcuts.hasSteamShortcut(container)) LinuxShortcuts.createSteamShortcut(container)
    }
}

private fun removeSteamEntry(context: android.content.Context) {
    runCatching {
        val manager = ContainerManager(context)
        manager.containers.forEach { LinuxShortcuts.removeSteamShortcut(it) }
    }
}

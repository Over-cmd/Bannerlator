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
import com.winlator.star.linux.LinuxProtons
import com.winlator.star.linux.LinuxRuntime
import com.winlator.star.linux.LinuxRuntimeInstaller
import com.winlator.star.linux.LinuxRuntimeUpdate
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
        LinuxRuntimeUpdate.refreshInstalled(context)
        release = withContext(Dispatchers.IO) { LinuxRuntimeInstaller.fetchRelease() }
        LinuxRuntimeUpdate.setAvailable(release)
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
                    // Same wording as the Games-tab card, from the same state.
                    Text(LinuxRuntimeUpdate.line(LinuxRuntimeUpdate.state.value),
                        style = MaterialTheme.typography.labelSmall)
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
                            LinuxRuntimeUpdate.begin()
                            scope.launch {
                                val ok = withContext(Dispatchers.IO) {
                                    LinuxRuntimeInstaller.install(context, r) { s, p ->
                                        stage = s; percent = p
                                        LinuxRuntimeUpdate.progress(s, p)
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
                                LinuxRuntimeUpdate.finish(context)
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

        // Only meaningful once the runtime is there: these land inside it, and the session is what
        // unpacks and registers them.
        if (installed != null) ProtonBuildsCard()
    }
}

/**
 * The Proton builds a game in the Steam client can be run with. Downloading one here does not
 * finish the job: the session unpacks and registers it at its next start, because a build's own
 * manifest asks Steam for a container Android cannot provide and has to be rewritten first. The
 * wording says so rather than claiming an install that has not happened yet.
 */
@Composable
private fun ProtonBuildsCard() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var builds by remember { mutableStateOf<List<LinuxProtons.Build>>(emptyList()) }
    var states by remember { mutableStateOf<Map<String, LinuxProtons.State>>(emptyMap()) }
    var loading by remember { mutableStateOf(true) }
    var workingOn by remember { mutableStateOf<String?>(null) }
    var stage by remember { mutableStateOf("") }
    var percent by remember { mutableStateOf(-1) }
    var error by remember { mutableStateOf<String?>(null) }
    var used by remember { mutableStateOf(0L) }

    // Reads the runtime's filesystem, which may be half-built, absent, or owned by a session that
    // is running right now. A tab in Contents must not be able to take the app down, whatever it
    // finds there.
    fun refresh() {
        runCatching {
            states = builds.associate { it.name to LinuxProtons.stateOf(context, it) }
            used = LinuxProtons.installedBytes(context)
        }.onFailure { android.util.Log.w("LinuxRuntimeTab", "proton state", it) }
    }

    LaunchedEffect(Unit) {
        builds = withContext(Dispatchers.IO) { LinuxProtons.fetchCatalog() }
        withContext(Dispatchers.IO) { refresh() }
        loading = false
    }

    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Proton builds", style = MaterialTheme.typography.titleMedium)
            Text(
                "What the Steam client runs a Windows game with. Add one here and it appears in "
                    + "that game's Compatibility list, so you can pick a different one per game. "
                    + "A build is unpacked the next time you open the Steam client.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            if (used > 0) {
                Text("Using ${used / (1024 * 1024)} MB",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (loading) {
                Text("Checking for builds…", style = MaterialTheme.typography.labelSmall)
            } else if (builds.isEmpty()) {
                Text("No builds available (could not reach the catalog)",
                    style = MaterialTheme.typography.labelSmall)
            }
            error?.let {
                Text(it, style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.error)
            }

            builds.forEach { build ->
                val state = states[build.name] ?: LinuxProtons.State.ABSENT
                val busy = workingOn == build.name
                Spacer(Modifier.height(4.dp))
                Text(build.display, style = MaterialTheme.typography.labelLarge)
                if (build.notes.isNotEmpty()) {
                    Text(build.notes, style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Text(
                    when (state) {
                        LinuxProtons.State.INSTALLED -> "Added"
                        LinuxProtons.State.PENDING ->
                            if (build.isDepot) "Requested — the client fetches it next session"
                            else "Ready — unpacked next time you open the Steam client"
                        else ->
                            if (build.isDepot) "Downloaded by the Steam client"
                            else "${build.size / (1024 * 1024)} MB download"
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (busy) {
                    Text(if (percent in 0..100) "$stage $percent%" else stage,
                        style = MaterialTheme.typography.labelSmall)
                    if (percent in 0..100) {
                        LinearProgressIndicator(progress = { percent / 100f },
                            modifier = Modifier.fillMaxWidth())
                    } else {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        enabled = workingOn == null && state == LinuxProtons.State.ABSENT,
                        onClick = {
                            workingOn = build.name; error = null
                            stage = "Starting…"; percent = -1
                            scope.launch {
                                val failure = withContext(Dispatchers.IO) {
                                    LinuxProtons.install(context, build) { s, p ->
                                        stage = s; percent = p
                                    }
                                }
                                error = failure
                                withContext(Dispatchers.IO) { refresh() }
                                workingOn = null
                            }
                        }
                    ) { Text(if (build.isDepot) "Request" else "Download") }

                    // Valve's depots belong to the client, which installs and removes them itself.
                    if (!build.isDepot) {
                        OutlinedButton(
                            enabled = workingOn == null && state != LinuxProtons.State.ABSENT,
                            onClick = {
                                workingOn = build.name; error = null
                                scope.launch {
                                    withContext(Dispatchers.IO) {
                                        LinuxProtons.remove(context, build)
                                        refresh()
                                    }
                                    workingOn = null
                                }
                            }
                        ) { Text("Remove") }
                    }
                }
            }
        }
    }
}

/**
 * Puts the Steam entry in a container so the Games tab has something to launch: one the user has
 * marked as a gamescope container if there is one, else the first. The entry carries the runtime
 * itself, so it launches into the Linux runtime from either.
 */
private fun addSteamEntry(context: android.content.Context) {
    runCatching {
        val manager = ContainerManager(context)
        // Check EVERY container, not just the one we are about to write to. The choice below can
        // land on a different container than last time - one got marked as a gamescope runtime, or
        // the order changed - and an update then added a SECOND entry beside the one already there.
        if (manager.containers.any { LinuxShortcuts.hasSteamShortcut(it) }) return
        val container = manager.containers.firstOrNull { it.isGamescopeRuntime }
            ?: manager.containers.firstOrNull() ?: return
        LinuxShortcuts.createSteamShortcut(container, context)
    }
}

private fun removeSteamEntry(context: android.content.Context) {
    runCatching {
        val manager = ContainerManager(context)
        manager.containers.forEach { LinuxShortcuts.removeSteamShortcut(it) }
    }
}

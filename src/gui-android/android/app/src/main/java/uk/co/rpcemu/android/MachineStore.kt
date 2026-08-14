package uk.co.rpcemu.android

import android.content.Context
import java.io.File

/**
 * Where RPCEmu keeps its files on Android, and what has to be there before a
 * machine can start.
 *
 * getExternalFilesDir is used rather than the app's internal directory, and that
 * is a deliberate trade rather than laziness. It is app-private in the sense that
 * it goes away when the app is uninstalled and needs no permission, but it is
 * reachable at Android/data/uk.co.rpcemu.android/files, so a ROM can be put there
 * with a file manager, MTP over USB, or adb push. Requiring the system file picker
 * for the very first ROM would mean nothing could be tested without a working
 * picker; that work can come later without moving the files.
 */
object MachineStore {

    fun dataDir(context: Context): File =
        context.getExternalFilesDir(null) ?: context.filesDir

    fun romsDir(context: Context): File = File(dataDir(context), "roms")

    /**
     * Create the directory layout and lay down the files the core cannot start
     * without.
     *
     * The CMOS template is the one that matters: with no default/cmos.ram the core
     * calls fatal() while seeding a new machine's settings, which on Android means
     * an abort and a tombstone rather than a message. It is shipped as an asset and
     * copied out once.
     */
    fun prepare(context: Context) {
        val data = dataDir(context)
        File(data, "roms").mkdirs()
        File(data, "machines/Default").mkdirs()
        File(data, "default").mkdirs()

        copyAssetIfMissing(context, "default/cmos.ram", File(data, "default/cmos.ram"))

        /*
         * The podule ROMs, and they are not optional. HostFS is one of them
         * (hostfs,ffa and hostfsfiler,ffa), so without these the guest cannot see
         * its hostfs directory at all - RISC OS then never finds !Boot and stops at
         * a Supervisor prompt, which is exactly what happened before they were
         * shipped. 60K in total, so they go in the APK.
         */
        val podules = File(data, "poduleroms")
        podules.mkdirs()
        context.assets.list("poduleroms")?.forEach { name ->
            copyAssetIfMissing(context, "poduleroms/$name", File(podules, name))
        }

        /*
         * A one-line Obey file so a fresh machine reaches the desktop instead of a
         * Supervisor prompt. The shipped HostFS carries only an installer that has
         * to be run from inside RISC OS; RISC OS runs $.!Boot at startup, and the
         * ,feb suffix is how HostFS spells the Obey filetype. Same trick as
         * tests/boot_smoke.py.
         */
        val hostfs = File(data, "machines/Default/hostfs")
        hostfs.mkdirs()
        val boot = File(hostfs, "!Boot,feb")
        if (!boot.exists()) {
            boot.writeText("Desktop\n")
        }
    }

    /**
     * ROMs the user has provided.
     *
     * A ROM is either a single file in roms/ or a directory of parts, which is what
     * the desktop's rom_dir setting names, so both are offered. Anything obviously
     * not a ROM - the machines directory, dotfiles - is left out.
     */
    fun listRoms(context: Context): List<String> {
        val roms = romsDir(context)
        val entries = roms.listFiles() ?: return emptyList()
        return entries
            .filter { !it.name.startsWith(".") }
            .filter { it.isDirectory || it.length() > 512 * 1024 }
            .map { it.name }
            .sorted()
    }

    private fun copyAssetIfMissing(context: Context, asset: String, dest: File) {
        if (dest.exists()) {
            return
        }
        dest.parentFile?.mkdirs()
        try {
            context.assets.open(asset).use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
        } catch (e: Exception) {
            // Not fatal here: the list screen reports what is missing rather than
            // dying, which is more use than a crash on first launch.
            android.util.Log.e("rpcemu", "cannot copy asset $asset: ${e.message}")
        }
    }
}

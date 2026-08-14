package uk.co.rpcemu.android

import android.os.Bundle
import android.system.Os
import org.libsdl.app.SDLActivity

/**
 * The activity a machine runs in.
 *
 * SDLActivity does the heavy lifting: it creates the surface, pumps touch and key
 * events, owns the audio thread and hands the native side its lifecycle. All this
 * adds is telling the native code where its files are, before that native code
 * starts.
 *
 * The environment is set in onCreate, not later, because SDLActivity loads the
 * native libraries during super.onCreate() and main() reads these on entry. Setting
 * them afterwards would be a race that happened to work on a fast device.
 */
class RPCEmuActivity : SDLActivity() {

    /**
     * The libraries SDLActivity loads, in order.
     *
     * "main" is not a name we chose: SDLActivity loads "SDL2" and then "main", so
     * the front end's shared object has to be libmain.so. Renaming it fails at
     * runtime with nothing more useful than a missing library.
     */
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "main")

    override fun onCreate(savedInstanceState: Bundle?) {
        val dataDir = MachineStore.dataDir(this)
        val machine = intent.getStringExtra(EXTRA_MACHINE) ?: "Default"
        val romDir = intent.getStringExtra(EXTRA_ROM_DIR) ?: ""

        // The core reads these on entry; see machine_start() in rpcemu_android.c.
        Os.setenv("RPCEMU_DATADIR", dataDir.absolutePath, true)
        Os.setenv("RPCEMU_MACHINE", machine, true)
        if (romDir.isNotEmpty()) {
            Os.setenv("RPCEMU_ROM_DIR", romDir, true)
        }

        super.onCreate(savedInstanceState)
    }

    companion object {
        const val EXTRA_MACHINE = "machine"
        const val EXTRA_ROM_DIR = "rom_dir"
    }
}

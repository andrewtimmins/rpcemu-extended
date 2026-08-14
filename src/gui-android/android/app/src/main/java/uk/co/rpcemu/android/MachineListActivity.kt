package uk.co.rpcemu.android

import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * The launcher: choose a ROM, run a machine.
 *
 * Built in code rather than XML, and deliberately plain. One machine runs at a
 * time, so this is a list and nothing else - no Manager, no tabs, no settings yet.
 * It is the smallest thing that lets a machine be started and tells the user what
 * to do when there is nothing to start.
 */
class MachineListActivity : AppCompatActivity() {

    private lateinit var list: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        MachineStore.prepare(this)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
        }
        root.addView(TextView(this).apply {
            text = "RPCEmu"
            textSize = 32f
        })
        root.addView(TextView(this).apply {
            text = "Choose a ROM to start"
            textSize = 16f
            setPadding(0, 8, 0, 32)
        })

        list = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(ScrollView(this).apply { addView(list) })

        setContentView(root)
    }

    /** Rebuilt on every return, so a ROM copied in while the app was open appears. */
    override fun onResume() {
        super.onResume()
        populate()
    }

    private fun populate() {
        list.removeAllViews()

        val roms = MachineStore.listRoms(this)
        if (roms.isEmpty()) {
            list.addView(TextView(this).apply {
                text = getString(R.string.no_roms)
                textSize = 16f
                gravity = Gravity.START
            })
            list.addView(TextView(this).apply {
                text = "\n" + MachineStore.romsDir(this@MachineListActivity).absolutePath
                textSize = 12f
            })
            return
        }

        for (rom in roms) {
            list.addView(Button(this).apply {
                text = rom
                textSize = 20f
                setOnClickListener { start(rom) }
            })
        }
    }

    private fun start(rom: String) {
        val intent = Intent(this, RPCEmuActivity::class.java).apply {
            putExtra(RPCEmuActivity.EXTRA_MACHINE, "Default")
            putExtra(RPCEmuActivity.EXTRA_ROM_DIR, rom)
        }
        startActivity(intent)
    }
}

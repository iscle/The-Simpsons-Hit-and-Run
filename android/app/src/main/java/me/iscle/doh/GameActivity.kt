package me.iscle.doh

import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import org.libsdl.app.SDLActivity

class GameActivity : SDLActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        if (SettingsManager.ignoreCutout && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            } else {
                window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        super.onCreate(savedInstanceState)
    }

    override fun getArguments(): Array<String> {
        val args = mutableListOf<String>()

        if (SettingsManager.showFps) {
            args.add("FPS")
        }

        return args.toTypedArray()
    }

    override fun getLibraries(): Array<String> {
        return arrayOf("SDL2", "SRR2")
    }
}
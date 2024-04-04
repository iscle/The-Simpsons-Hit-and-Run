package me.iscle.doh

import org.libsdl.app.SDLActivity

class GameActivity : SDLActivity() {

    override fun getLibraries(): Array<String> {
        return arrayOf("SDL2", "SRR2")
    }
}
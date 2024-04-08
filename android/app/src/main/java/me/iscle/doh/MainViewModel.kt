package me.iscle.doh

import androidx.lifecycle.ViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

class MainViewModel : ViewModel() {
    private val _ignoreCutout = MutableStateFlow(SettingsManager.ignoreCutout)
    val ignoreCutout = _ignoreCutout.asStateFlow()

    private val _showFps = MutableStateFlow(SettingsManager.showFps)
    val showFps = _showFps.asStateFlow()

    fun setIgnoreCutout(ignoreCutout: Boolean) {
        SettingsManager.ignoreCutout = ignoreCutout
        _ignoreCutout.value = ignoreCutout
    }

    fun setShowFps(showFps: Boolean) {
        SettingsManager.showFps = showFps
        _showFps.value = showFps
    }
}
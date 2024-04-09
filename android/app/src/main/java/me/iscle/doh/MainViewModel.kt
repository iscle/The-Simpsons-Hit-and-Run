package me.iscle.doh

import androidx.lifecycle.ViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

class MainViewModel : ViewModel() {
    private val _ignoreCutout = MutableStateFlow(SettingsManager.ignoreCutout)
    val ignoreCutout = _ignoreCutout.asStateFlow()

    private val _args = MutableStateFlow(SettingsManager.args)
    val args = _args.asStateFlow()

    fun setIgnoreCutout(ignoreCutout: Boolean) {
        SettingsManager.ignoreCutout = ignoreCutout
        _ignoreCutout.value = ignoreCutout
    }

    fun setArgs(args: String) {
        SettingsManager.args = args
        _args.value = args
    }
}
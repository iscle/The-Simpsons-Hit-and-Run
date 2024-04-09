package me.iscle.doh

import android.content.Context
import android.content.SharedPreferences

object SettingsManager {
    private lateinit var preferences: SharedPreferences

    fun init(context: Context) {
        preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
    }

    var ignoreCutout: Boolean
        get() = preferences.getBoolean("ignoreCutout", false)
        set(value) = preferences.edit().putBoolean("ignoreCutout", value).apply()

    var args: String
        get() = preferences.getString("args", null) ?: ""
        set(value) = preferences.edit().putString("args", value).apply()
}
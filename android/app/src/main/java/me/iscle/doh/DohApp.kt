package me.iscle.doh

import android.app.Application

class DohApp : Application() {

    override fun onCreate() {
        super.onCreate()
        SettingsManager.init(applicationContext)
    }
}
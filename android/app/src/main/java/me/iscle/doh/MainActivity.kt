package me.iscle.doh

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import me.iscle.doh.ui.theme.DohTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            DohTheme {
                Scaffold(
                    modifier = Modifier.fillMaxSize()
                ) { innerPadding ->
                    Column(
                        modifier = Modifier
                            .padding(innerPadding)
                            .fillMaxSize(),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        val context = LocalContext.current
                        val viewModel: MainViewModel = viewModel()
                        val ignoreCutout = viewModel.ignoreCutout.collectAsState().value
                        val args = viewModel.args.collectAsState().value

                        Row(
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Ignore cutout")
                            Switch(
                                checked = ignoreCutout,
                                onCheckedChange = viewModel::setIgnoreCutout
                            )
                        }

                        Row(
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Args")
                            TextField(
                                value = args,
                                onValueChange = viewModel::setArgs
                            )
                        }

                        Button(onClick = {
                            Intent(context, GameActivity::class.java).also {
                                startActivity(it)
                            }
                        }) {
                            Text("Start game")
                        }
                    }
                }
            }
        }
    }
}
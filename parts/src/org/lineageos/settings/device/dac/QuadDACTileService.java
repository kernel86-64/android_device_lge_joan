// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.widget.Toast;

// Matches the LineageOS 22 tile states; the backend is the mainline ALSA HAL.
public final class QuadDACTileService extends TileService {
    private Controls controls;
    private final Handler handler = new Handler();
    private final Runnable update = new Runnable() {
        @Override public void run() { refresh(); handler.postDelayed(this, 2000); }
    };
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) { refresh(); }
    };
    @Override public void onStartListening() {
        controls = new Controls(this);
        controls.restore();
        registerReceiver(receiver, new IntentFilter(Intent.ACTION_HEADSET_PLUG),
                Context.RECEIVER_EXPORTED);
        handler.post(update);
    }
    @Override public void onStopListening() {
        handler.removeCallbacks(update);
        unregisterReceiver(receiver);
    }
    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) return;
        boolean ready = controls.available() && controls.wired();
        boolean enabled = controls.enabled();
        tile.setState(!ready ? Tile.STATE_UNAVAILABLE
                : enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel(getString(!ready ? R.string.quad_dac_unavail
                : enabled ? R.string.quad_dac_on : R.string.quad_dac_off));
        tile.updateTile();
    }
    @Override public void onClick() {
        if (controls == null) controls = new Controls(this);
        if (!controls.available() || !controls.wired()) { refresh(); return; }
        if (!controls.setEnabled(!controls.enabled()))
            Toast.makeText(this, R.string.hardware_error, Toast.LENGTH_SHORT).show();
        refresh();
    }
}

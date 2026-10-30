// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.os.Handler;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.widget.Toast;

public final class FlashlightTileService extends TileService {
    private final Handler handler = new Handler();
    private final Runnable update = new Runnable() {
        @Override public void run() { refresh(); handler.postDelayed(this, 1000); }
    };
    @Override public void onStartListening() { handler.post(update); }
    @Override public void onStopListening() { handler.removeCallbacks(update); }
    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) return;
        int level = Torch.level(this);
        tile.setState(level < 0 ? Tile.STATE_UNAVAILABLE
                : level == 0 ? Tile.STATE_INACTIVE : Tile.STATE_ACTIVE);
        tile.setLabel(getString(R.string.flashlight));
        tile.setSubtitle(level < 0 ? getString(R.string.unavailable)
                : level == 0 ? getString(R.string.off) : getString(R.string.level, level));
        tile.updateTile();
    }
    @Override public void onClick() {
        int level = Torch.level(this);
        if (level < 0) { refresh(); return; }
        int saved = new Controls(this).prefs.getInt("torch_level", 3);
        if (!Torch.set(this, level > 0 ? 0 : saved))
            Toast.makeText(this, R.string.hardware_error, Toast.LENGTH_SHORT).show();
        refresh();
    }
}

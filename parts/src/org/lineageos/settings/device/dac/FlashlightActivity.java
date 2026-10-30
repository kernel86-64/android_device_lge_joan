// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.os.Bundle;
import android.service.quicksettings.TileService;
import android.widget.Toast;

public final class FlashlightActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int current = Torch.level(this);
        CharSequence[] levels = new CharSequence[6];
        levels[0] = getString(R.string.off);
        for (int i = 1; i <= 5; i++) levels[i] = getString(R.string.level, i);
        new AlertDialog.Builder(this).setTitle(R.string.brightness)
                .setSingleChoiceItems(levels, current, (dialog, which) -> {
                    if (!Torch.set(this, which)) {
                        Toast.makeText(this, R.string.hardware_error, Toast.LENGTH_SHORT).show();
                        return;
                    }
                    if (which > 0) new Controls(this).prefs.edit().putInt("torch_level", which).apply();
                    TileService.requestListeningState(this,
                            new ComponentName(this, FlashlightTileService.class));
                }).setPositiveButton(android.R.string.ok, (d, w) -> finish())
                .setOnCancelListener(d -> finish()).show();
    }
}

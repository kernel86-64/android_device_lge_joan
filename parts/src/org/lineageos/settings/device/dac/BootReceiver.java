// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.provider.Settings;
import java.util.ArrayList;
import java.util.Arrays;

public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        new Controls(context).restore();
        context.getSystemService(JobScheduler.class).schedule(new JobInfo.Builder(300,
                new ComponentName(context, RestoreJob.class))
                .setMinimumLatency(15000).setOverrideDeadline(60000)
                .setBackoffCriteria(30000, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build());
        Controls controls = new Controls(context);
        String current = Settings.Secure.getString(context.getContentResolver(), "sysui_qs_tiles");
        if (current == null || current.isEmpty()) return;
        ArrayList<String> tiles = new ArrayList<>(Arrays.asList(current.split(",")));
        boolean changed = false;
        // Add the Quad DAC tile once, without replacing the user's other tiles.
        if (!controls.prefs.getBoolean("tiles_added", false)) {
            String dac = "custom(org.lineageos.settings.device.dac/.QuadDACTileService)";
            if (!tiles.contains(dac)) tiles.add(dac);
            changed = true;
        }
        // The system flashlight tile has a strength slider now: put it back
        // where an earlier version replaced it with this app's tile.
        int at = tiles.indexOf("custom(org.lineageos.settings.device.dac/.FlashlightTileService)");
        if (at >= 0) {
            tiles.remove(at);
            if (!tiles.contains("flashlight")) tiles.add(at, "flashlight");
            changed = true;
        }
        if (changed && Settings.Secure.putString(context.getContentResolver(), "sysui_qs_tiles",
                String.join(",", tiles))) {
            controls.prefs.edit().putBoolean("tiles_added", true).apply();
        }
    }
}

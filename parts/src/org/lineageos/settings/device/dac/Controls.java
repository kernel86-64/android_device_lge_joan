// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.content.Context;
import android.content.SharedPreferences;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;

final class Controls {
    static final String ENABLED = "joan_quad_dac";
    private final AudioManager audio;
    final SharedPreferences prefs;

    Controls(Context context) {
        audio = context.getSystemService(AudioManager.class);
        prefs = context.createDeviceProtectedStorageContext()
                .getSharedPreferences("controls", Context.MODE_PRIVATE);
    }

    boolean wired() {
        for (AudioDeviceInfo d : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (d.getType() == AudioDeviceInfo.TYPE_WIRED_HEADSET
                    || d.getType() == AudioDeviceInfo.TYPE_WIRED_HEADPHONES) return true;
        }
        return false;
    }

    boolean available() { return "1".equals(read("joan_dac_available")); }
    boolean enabled() { return "1".equals(read(ENABLED)); }
    private String read(String key) {
        for (String field : audio.getParameters(key).split(";")) {
            if (field.startsWith(key + "=")) return field.substring(key.length() + 1);
        }
        return "";
    }

    boolean setEnabled(boolean enabled) {
        audio.setParameters(ENABLED + "=" + (enabled ? 1 : 0));
        if (enabled() != enabled) return false;
        prefs.edit().putBoolean(ENABLED, enabled).apply();
        return true;
    }

    boolean set(String key, int value) {
        audio.setParameters(key + "=" + value);
        if (!Integer.toString(value).equals(read(key))) return false;
        prefs.edit().putInt(key, value).apply();
        return true;
    }

    void restore() {
        audio.setParameters(ENABLED + "=" + (prefs.getBoolean(ENABLED, false) ? 1 : 0));
        // Apply mixer settings only after the delayed ADSP sound card exists.
        if (available()) {
            audio.setParameters("joan_dac_filter=" + prefs.getInt("joan_dac_filter", 4)
                    + ";joan_dac_type=" + prefs.getInt("joan_dac_type", 0)
                    + ";joan_dac_volume=" + prefs.getInt("joan_dac_volume", 10));
        }
    }
}

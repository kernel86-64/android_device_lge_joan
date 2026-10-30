// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.app.Activity;
import android.os.Bundle;
import android.preference.ListPreference;
import android.widget.Toast;
import android.preference.PreferenceFragment;
import android.preference.PreferenceScreen;
import android.preference.SwitchPreference;

public final class QuadDACPanelActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state == null) getFragmentManager().beginTransaction()
                .replace(android.R.id.content, new Panel()).commit();
    }
    public static final class Panel extends PreferenceFragment {
        private Controls controls;
        @Override public void onCreate(Bundle state) {
            super.onCreate(state);
            controls = new Controls(getActivity());
            controls.restore();
            PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(getActivity());
            setPreferenceScreen(screen);
            SwitchPreference enabled = new SwitchPreference(getActivity());
            enabled.setTitle(R.string.quad_dac);
            enabled.setChecked(controls.enabled());
            enabled.setPersistent(false);
            enabled.setOnPreferenceChangeListener((p, v) -> controls.setEnabled((Boolean)v));
            screen.addPreference(enabled);
            addList(screen, "joan_dac_filter", R.string.digital_filter,
                    getResources().getStringArray(R.array.filters), 4);
            addList(screen, "joan_dac_type", R.string.headphone_type,
                    getResources().getStringArray(R.array.headphone_types), 0);
            String[] volumes = new String[25];
            for (int i = 0; i <= 24; i++) volumes[i] = (i - 24) + " dB";
            addList(screen, "joan_dac_volume", R.string.analog_volume, volumes, 10);
        }
        private void addList(PreferenceScreen screen, String key, int title,
                String[] entries, int fallback) {
            ListPreference preference = new ListPreference(getActivity());
            preference.setTitle(title);
            preference.setPersistent(false);
            preference.setEntries(entries);
            String[] values = new String[entries.length];
            for (int i = 0; i < values.length; i++) values[i] = Integer.toString(i);
            preference.setEntryValues(values);
            preference.setValue(Integer.toString(controls.prefs.getInt(key, fallback)));
            preference.setSummary("%s");
            preference.setOnPreferenceChangeListener((p, v) -> {
                boolean applied = controls.set(key, Integer.parseInt((String)v));
                if (!applied) Toast.makeText(getActivity(), R.string.hardware_error,
                        Toast.LENGTH_SHORT).show();
                return applied;
            });
            screen.addPreference(preference);
        }
    }
}

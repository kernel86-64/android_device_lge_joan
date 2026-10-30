// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.content.Context;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.os.Handler;
import android.os.Looper;

// The flashlight through the camera service's torch API, so the camera app,
// the system flashlight tile and this tile share one state. The camera HAL
// maps its strength levels onto the LED's torch current (capped by the DT).
final class Torch {
    static final int STEPS = 5;

    private static CameraManager manager;
    private static String cameraId;
    private static int maxStrength;
    // Torch state from the camera service: null until known or unavailable.
    private static volatile Boolean on;

    private static synchronized boolean init(Context context) {
        if (manager != null) return cameraId != null;
        manager = context.getApplicationContext().getSystemService(CameraManager.class);
        try {
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics c = manager.getCameraCharacteristics(id);
                Boolean flash = c.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
                Integer max = c.get(CameraCharacteristics.FLASH_INFO_STRENGTH_MAXIMUM_LEVEL);
                if (Boolean.TRUE.equals(flash) && max != null && max > 1) {
                    cameraId = id;
                    maxStrength = max;
                    break;
                }
            }
        } catch (CameraAccessException e) {
            cameraId = null;
        }
        if (cameraId == null) return false;
        manager.registerTorchCallback(new CameraManager.TorchCallback() {
            @Override public void onTorchModeChanged(String id, boolean enabled) {
                if (id.equals(cameraId)) on = enabled;
            }
            @Override public void onTorchModeUnavailable(String id) {
                if (id.equals(cameraId)) on = null;
            }
        }, new Handler(Looper.getMainLooper()));
        return true;
    }

    // 0 off, 1..STEPS on, -1 unavailable (no flash, or a camera is open).
    static int level(Context context) {
        if (!init(context) || on == null) return -1;
        if (!on) return 0;
        try {
            int strength = manager.getTorchStrengthLevel(cameraId);
            return Math.max(1, Math.min(STEPS, Math.round((float) strength * STEPS / maxStrength)));
        } catch (CameraAccessException | IllegalArgumentException e) {
            return -1;
        }
    }

    static boolean set(Context context, int level) {
        if (level < 0 || level > STEPS || !init(context)) return false;
        try {
            if (level == 0) {
                manager.setTorchMode(cameraId, false);
            } else {
                manager.turnOnTorchWithStrengthLevel(cameraId,
                        Math.max(1, level * maxStrength / STEPS));
            }
            on = level > 0;
            return true;
        } catch (CameraAccessException | IllegalArgumentException e) {
            return false;
        }
    }
}

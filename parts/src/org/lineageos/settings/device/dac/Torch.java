// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

final class Torch {
    // LED brightness controls continuous torch current, never flash strobes.
    private static final File LED = new File("/sys/class/leds/white:flash");
    private static int read(String name) throws IOException {
        return Integer.parseInt(Files.readString(new File(LED, name).toPath()).trim());
    }
    static int level() {
        try {
            int max = read("max_brightness");
            int current = read("brightness");
            return max > 0 ? Math.min(5, (current * 5 + max - 1) / max) : -1;
        } catch (IOException | NumberFormatException e) { return -1; }
    }
    static boolean set(int level) {
        if (level < 0 || level > 5) return false;
        try {
            int max = read("max_brightness");
            // Round down so each step stays within the DT's 200 mA limit.
            if (max < 5) return false;
            Files.writeString(new File(LED, "brightness").toPath(),
                    Integer.toString(max * level / 5), StandardCharsets.US_ASCII);
            return level() == level;
        } catch (IOException | NumberFormatException e) { return false; }
    }
}

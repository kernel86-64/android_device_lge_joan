// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.device.dac;

import android.app.job.JobParameters;
import android.app.job.JobService;

/** ADSP deliberately starts after boot; retry saved mixer settings when ready. */
public final class RestoreJob extends JobService {
    @Override public boolean onStartJob(JobParameters params) {
        new Thread(() -> {
            Controls controls = new Controls(this);
            controls.restore();
            jobFinished(params, !controls.available());
        }, "RestoreQuadDAC").start();
        return true;
    }
    @Override public boolean onStopJob(JobParameters params) { return true; }
}

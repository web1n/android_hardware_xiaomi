package vendor.xiaomi.sensor.camera;

import android.os.ParcelFileDescriptor;

@VintfStability
parcelable Ashmem {
    ParcelFileDescriptor fd;
    long size;
}

package vendor.xiaomi.sensor.camera;

import vendor.xiaomi.sensor.camera.Ashmem;
import vendor.xiaomi.sensor.camera.IMiCamCallback;

@VintfStability
interface IMiCam {
    int openCam(int clientId, int cameraId, IMiCamCallback callback,
            in Ashmem memory, in int[] config);
    int closeCam(int clientId, int cameraId);
    void setisReadingAshmem(boolean reading);
}

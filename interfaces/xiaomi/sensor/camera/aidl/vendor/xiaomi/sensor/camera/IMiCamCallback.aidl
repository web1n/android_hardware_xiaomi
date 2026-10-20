package vendor.xiaomi.sensor.camera;

@VintfStability
interface IMiCamCallback {
    void onFrameEvent(int event, int width, int height, int format);
}

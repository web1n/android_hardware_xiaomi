package vendor.xiaomi.hardware.aon;

@VintfStability
interface IAlwaysOnListener {
    void onCallbackListener(int Type, in int[] data);
}

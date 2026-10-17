package vendor.xiaomi.hardware.aon;

import vendor.xiaomi.hardware.aon.IAlwaysOnListener;

@VintfStability
interface IAlwaysOn {
    int getCapability();
    int registerListener(int Type, float fps, int timeout, IAlwaysOnListener listener);
    int unregisterListener(int Type, IAlwaysOnListener listener);
    void aon_update_parameter(int Type, float fps, int timeout, long data);
}

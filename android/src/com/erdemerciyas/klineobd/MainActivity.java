package com.erdemerciyas.klineobd;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Base64;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.util.Collections;
import java.util.UUID;

/**
 * Panel sayfasını (assets/panel.html) gösterir ve Web Bluetooth yerine BLE'yi kendisi yürütür:
 * kartı servis UUID'siyle bulur, bağlanır, STREAM bildirimlerini sayfaya bleData(base64) olarak verir,
 * sayfadan gelen komutları (AndroidBle.send) CMD karakteristiğine yazar. Kopunca kendisi yeniden bağlanır.
 */
@SuppressWarnings({"deprecation", "MissingPermission"})
public class MainActivity extends Activity {
    private static final UUID SVC = UUID.fromString("4b4c0001-0b5e-4e1a-9a7d-6f62642d6b6c");
    private static final UUID STREAM = UUID.fromString("4b4c0002-0b5e-4e1a-9a7d-6f62642d6b6c");
    private static final UUID CMD = UUID.fromString("4b4c0003-0b5e-4e1a-9a7d-6f62642d6b6c");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final int REQ_PERM = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private WebView web;
    private BluetoothAdapter adapter;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic cmdChr;
    private boolean scanning, alive;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);   // araçta ekran kapanmasın
        getWindow().setStatusBarColor(Color.parseColor("#0b0d10"));
        getWindow().setNavigationBarColor(Color.parseColor("#0b0d10"));

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#0b0d10"));
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        web.addJavascriptInterface(new Bridge(), "AndroidBle");
        web.loadUrl("file:///android_asset/panel.html");
        setContentView(web);

        BluetoothManager bm = (BluetoothManager) getSystemService(BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
    }

    @Override
    protected void onStart() {
        super.onStart();
        alive = true;
        if (hasPerms()) {
            start();
        } else {
            requestPermissions(perms(), REQ_PERM);
        }
    }

    @Override
    protected void onStop() {
        // arka planda bağlantıyı bırak: kart başka telefona/sayfaya açık kalsın, pil harcanmasın
        alive = false;
        stopScan();
        closeGatt();
        super.onStop();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] p, int[] r) {
        if (hasPerms()) {
            start();
        } else {
            state("perm");
        }
    }

    private String[] perms() {
        if (Build.VERSION.SDK_INT >= 31) {
            return new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT};
        }
        return new String[]{Manifest.permission.ACCESS_FINE_LOCATION};
    }

    private boolean hasPerms() {
        for (String p : perms()) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- BLE
    private void start() {
        if (!alive || gatt != null || scanning) {
            return;
        }
        if (adapter == null || !adapter.isEnabled()) {
            state("off");
            ui.postDelayed(this::start, 3000);     // Bluetooth açılınca kendisi devam eder
            return;
        }
        BluetoothLeScanner sc = adapter.getBluetoothLeScanner();
        if (sc == null) {
            ui.postDelayed(this::start, 3000);
            return;
        }
        ScanFilter f = new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SVC)).build();
        ScanSettings st = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        scanning = true;
        state("scan");
        sc.startScan(Collections.singletonList(f), st, scanCb);
    }

    private void stopScan() {
        if (scanning && adapter != null && adapter.getBluetoothLeScanner() != null) {
            adapter.getBluetoothLeScanner().stopScan(scanCb);
        }
        scanning = false;
    }

    private void closeGatt() {
        cmdChr = null;
        if (gatt != null) {
            gatt.close();
            gatt = null;
        }
    }

    private void retry() {
        closeGatt();
        state("lost");
        ui.postDelayed(this::start, 1500);
    }

    private final ScanCallback scanCb = new ScanCb();

    // isimli iç sınıf: d8, alan başlatıcısındaki anonim sınıflarda (EnclosingMethod boş) çöküyor
    private class ScanCb extends ScanCallback {
        @Override
        public void onScanResult(int type, ScanResult r) {
            if (!scanning) {
                return;
            }
            stopScan();
            BluetoothDevice d = r.getDevice();
            state("connecting");
            gatt = d.connectGatt(MainActivity.this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
        }

        @Override
        public void onScanFailed(int err) {
            scanning = false;
            ui.postDelayed(MainActivity.this::start, 3000);
        }
    }

    private final BluetoothGattCallback gattCb = new GattCb();

    private class GattCb extends BluetoothGattCallback {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int st) {
            if (st == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
                g.requestMtu(517);
            } else {
                ui.post(MainActivity.this::retry);
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int mtu, int status) {
            g.discoverServices();
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattService svc = g.getService(SVC);
            BluetoothGattCharacteristic st = svc != null ? svc.getCharacteristic(STREAM) : null;
            if (st == null) {
                g.disconnect();
                return;
            }
            cmdChr = svc.getCharacteristic(CMD);
            g.setCharacteristicNotification(st, true);
            BluetoothGattDescriptor d = st.getDescriptor(CCCD);
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            g.writeDescriptor(d);
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            state("connected");
        }

        // Android 13+
        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] v) {
            data(v);
        }

        // Android 12 ve öncesi
        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            data(c.getValue());
        }
    }

    // ---------------------------------------------------------------- sayfa köprüsü
    private void data(byte[] v) {
        final String b64 = Base64.encodeToString(v, Base64.NO_WRAP);
        ui.post(() -> web.evaluateJavascript("bleData('" + b64 + "')", null));
    }

    private void state(final String s) {
        ui.post(() -> web.evaluateJavascript("window.bleState&&bleState('" + s + "')", null));
    }

    class Bridge {
        @JavascriptInterface
        public boolean send(String cmd) {
            BluetoothGatt g = gatt;
            BluetoothGattCharacteristic c = cmdChr;
            if (g == null || c == null) {
                return false;
            }
            c.setValue(cmd.getBytes());
            c.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            return g.writeCharacteristic(c);
        }

        /** "Bağlan" düğmesi: izin yoksa tekrar ister, yoksa aramayı yeniden başlatır. */
        @JavascriptInterface
        public void connect() {
            ui.post(() -> {
                if (!hasPerms()) {
                    requestPermissions(perms(), REQ_PERM);
                } else {
                    start();
                }
            });
        }
    }
}

package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */
public class Dashboard {

    private final Map<String, DeviceState> devices = new TreeMap<>();

    public synchronized void updateTelemetry(String deviceId, long timestamp, double temperature) {
        DeviceState state = stateFor(deviceId);
        state.temperature = temperature;
        state.hasTemperature = true;
        state.lastMessageTimestamp = timestamp;
    }

    public synchronized void updateAlarm(String deviceId, long timestamp, double temperature) {
        DeviceState state = stateFor(deviceId);
        state.temperature = temperature;
        state.hasTemperature = true;
        state.lastMessageTimestamp = timestamp;
        state.alarmCount++;
    }

    public synchronized void updateStatus(String deviceId, long timestamp) {
        stateFor(deviceId).lastMessageTimestamp = timestamp;
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        System.out.printf("%-16s %-12s %-20s %s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");
        long now = System.currentTimeMillis();
        for (Map.Entry<String, DeviceState> entry : devices.entrySet()) {
            DeviceState state = entry.getValue();
            String temperature = state.hasTemperature
                    ? String.format(Locale.ROOT, "%.2f", state.temperature)
                    : "-";
            long ageSeconds = Math.max(0, now - state.lastMessageTimestamp) / 1000;
            System.out.printf("%-16s %-12s %-20s %d%n",
                    entry.getKey(), temperature, formatAge(ageSeconds), state.alarmCount);
        }
    }

    private DeviceState stateFor(String deviceId) {
        return devices.computeIfAbsent(deviceId, ignored -> new DeviceState());
    }

    private static String formatAge(long seconds) {
        if (seconds < 60) {
            return "acum " + seconds + " s";
        }
        if (seconds < 3600) {
            return "acum " + seconds / 60 + " min";
        }
        return "acum " + seconds / 3600 + " h";
    }

    private static final class DeviceState {
        private double temperature;
        private boolean hasTemperature;
        private long lastMessageTimestamp;
        private long alarmCount;
    }
}

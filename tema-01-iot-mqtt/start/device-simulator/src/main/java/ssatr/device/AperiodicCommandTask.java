package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    private static final long MIN_PERIOD_MS = 50;
    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String command;
            try {
                command = commands.take();              // în ordinea sosirii, fără interval minim
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            // Un job = o comandă. Orice excepție se transformă în ERROR, taskul nu se oprește.
            String reply;
            try {
                reply = execute(command);
            } catch (Exception e) {
                reply = "ERROR " + e.getMessage();
            }
            publishStatus(reply);
        }
    }

    /** Execută o comandă și întoarce textul confirmării ("OK ..."); aruncă excepție dacă e invalidă. */
    private String execute(String raw) {
        if (raw == null || raw.isBlank()) {
            throw new IllegalArgumentException("comanda goala");
        }
        String[] parts = raw.trim().split("\\s+");
        String name = parts[0].toUpperCase(Locale.ROOT);

        switch (name) {
            case "SET_INTERVAL": {
                requireArgs(parts, 1, "SET_INTERVAL <ms>");
                long ms = Long.parseLong(parts[1]);
                if (ms < MIN_PERIOD_MS) {
                    throw new IllegalArgumentException("interval prea mic (minim " + MIN_PERIOD_MS + " ms)");
                }
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                requireArgs(parts, 1, "SET_THRESHOLD <valoare>");
                double value = Double.parseDouble(parts[1]);
                if (Double.isNaN(value) || Double.isInfinite(value)) {
                    throw new IllegalArgumentException("prag invalid");
                }
                periodic.setThreshold(value);
                return "OK SET_THRESHOLD " + String.format(Locale.US, "%.2f", value);
            }
            case "RESET": {
                requireArgs(parts, 0, "RESET");
                sensor.reset();
                return "OK RESET";
            }
            case "STATUS": {
                requireArgs(parts, 0, "STATUS");
                // fără ';' în text, ca să nu strice formatul "deviceId;timestamp;text"
                return String.format(Locale.US, "OK STATUS interval=%d threshold=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            }
            default:
                throw new IllegalArgumentException("comanda necunoscuta " + name);
        }
    }

    private static void requireArgs(String[] parts, int expected, String usage) {
        if (parts.length - 1 != expected) {
            throw new IllegalArgumentException("sintaxa: " + usage);
        }
    }

    private void publishStatus(String text) {
        try {
            String payload = deviceId + ";" + System.currentTimeMillis() + ";" + text;
            mqtt.publish(Config.statusTopic(deviceId), payload);
            System.out.println("[" + deviceId + "] " + text);
        } catch (Exception e) {
            System.err.println("[" + deviceId + "] eroare la publicarea confirmării: " + e.getMessage());
        }
    }
}

package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.DoubleSupplier;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;
    private final DoubleSupplier thresholdSupplier;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents, DoubleSupplier thresholdSupplier) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        this.thresholdSupplier = thresholdSupplier;
    }

    @Override
    public void run() {
        long lastReleaseNs = 0;
        boolean hasReleased = false;   // până la prima alarmă nu există "ultima eliberare"

        while (!Thread.currentThread().isInterrupted()) {
            double temperature;
            try {
                temperature = alarmEvents.take();          // blochează până sosește un eveniment
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            long arrivalNs = System.nanoTime();

            // Impunerea intervalului minim între sosiri (față de ultima eliberare acceptată)
            if (hasReleased) {
                long sinceLastMs = TimeUnit.NANOSECONDS.toMillis(arrivalNs - lastReleaseNs);
                if (sinceLastMs < MIN_INTERARRIVAL_MS) {
                    System.out.printf(Locale.US,
                            "[%s] alarmă RESPINSĂ (%.2f °C): au trecut doar %d ms din %d ms%n",
                            deviceId, temperature, sinceLastMs, MIN_INTERARRIVAL_MS);
                    continue;
                }
            }

            // Eveniment acceptat: aceasta este eliberarea taskului sporadic
            lastReleaseNs = arrivalNs;
            hasReleased = true;

            try {
                long ts = System.currentTimeMillis();
                double threshold = thresholdSupplier.getAsDouble();
                String payload = String.format(Locale.US, "%s;%d;%.2f;%.2f",
                        deviceId, ts, temperature, threshold);
                mqtt.publish(Config.alarmTopic(deviceId), payload);
                System.out.printf(Locale.US, "[%s] alarmă publicată: %.2f °C > %.2f%n",
                        deviceId, temperature, threshold);
            } catch (Exception e) {
                System.err.println("[" + deviceId + "] eroare la publicarea alarmei: " + e.getMessage());
            }
        }
    }
}

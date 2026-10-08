package ssatr.device;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.BlockingQueue;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // Prima eliberare: acum. Restul sunt pe grila r_k = r_0 + k*T (timp monoton).
        long next = System.nanoTime();

        while (!Thread.currentThread().isInterrupted()) {
            // ---- JOB ----
            try {
                double t = sensor.read();
                long ts = System.currentTimeMillis();

                String payload = deviceId + ";" + ts + ";" + String.format(Locale.US, "%.2f", t);
                mqtt.publish(Config.telemetryTopic(deviceId), payload);

                // Taskul periodic doar semnalează; alarma o tratează taskul sporadic.
                // offer() nu blochează, deci jobul nu depinde de coada alarmelor.
                if (t > threshold) {
                    alarmEvents.offer(t);
                }
            } catch (Exception e) {
                // o eroare într-un job (ex. publicare eșuată) nu trebuie să oprească taskul
                System.err.println("[" + deviceId + "] eroare în jobul periodic: " + e.getMessage());
            }

            // ---- Așteptare absolută până la următoarea eliberare de pe grilă ----
            // periodMs se citește la fiecare job, ca SET_INTERVAL să aibă efect imediat.
            next += TimeUnit.MILLISECONDS.toNanos(periodMs);

            long now = System.nanoTime();
            long remaining = next - now;

            if (remaining > 0) {
                try {
                    TimeUnit.NANOSECONDS.sleep(remaining);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            } else {
                // Jobul a depășit perioada (overrun). Nu recuperăm eliberările pierdute
                // în rafală: resincronizăm grila la momentul curent.
                System.err.println("[" + deviceId + "] overrun: " + (-remaining / 1_000_000) + " ms");
                next = now;
            }
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}

package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        if (!"telemetry".equals(kind) && !"alarm".equals(kind) && !"status".equals(kind)) {
            throw new IllegalArgumentException("Tip de mesaj necunoscut: " + kind);
        }
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            final String message;
            try {
                message = inbox.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            storage.append(kind, message);
            try {
                process(message);
            } catch (IllegalArgumentException e) {
                System.err.println("Mesaj " + kind + " invalid: " + message + " (" + e.getMessage() + ")");
            }
        }
    }

    private void process(String message) {
        String[] fields = message.split(";", -1);
        int expectedFields = "alarm".equals(kind) ? 4 : 3;
        if (fields.length != expectedFields || fields[0].isBlank()) {
            throw new IllegalArgumentException("număr incorect de câmpuri sau deviceId lipsă");
        }

        long timestamp;
        try {
            timestamp = Long.parseLong(fields[1]);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("timestamp invalid", e);
        }
        if (timestamp < 0) {
            throw new IllegalArgumentException("timestamp invalid");
        }

        switch (kind) {
            case "telemetry": {
                double temperature = parseFiniteNumber(fields[2], "temperatura");
                dashboard.updateTelemetry(fields[0], timestamp, temperature);
                break;
            }
            case "alarm": {
                double temperature = parseFiniteNumber(fields[2], "temperatura");
                parseFiniteNumber(fields[3], "prag");
                dashboard.updateAlarm(fields[0], timestamp, temperature);
                System.out.println("[" + fields[0] + "] alarmă: " + message);
                break;
            }
            case "status":
                if (fields[2].isBlank()) {
                    throw new IllegalArgumentException("confirmare lipsă");
                }
                dashboard.updateStatus(fields[0], timestamp);
                System.out.println("[" + fields[0] + "] confirmare: " + fields[2]);
                break;
            default:
                throw new IllegalStateException("Tip de mesaj necunoscut: " + kind);
        }
    }

    private static double parseFiniteNumber(String value, String fieldName) {
        try {
            double number = Double.parseDouble(value);
            if (Double.isFinite(number)) {
                return number;
            }
        } catch (NumberFormatException ignored) {
            // Report all malformed numeric values consistently below.
        }
        throw new IllegalArgumentException(fieldName + " invalid");
    }
}

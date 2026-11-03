#include <Arduino.h>
#include "config.h"
#include "connection.h"

// --- Tipuri de date pentru cozi ---
#define COMMAND_MAX_LEN 64

struct AlarmEvent {
    double temperature;
    double threshold;
};

struct CommandMessage {
    char text[COMMAND_MAX_LEN];
};

// --- Cozi FreeRTOS ---
static QueueHandle_t alarm_queue = NULL;
static QueueHandle_t command_queue = NULL;

// --- Variabile partajate (volatile) ---
static volatile uint32_t current_period_ms = PERIOD_MS_INITIAL;
static volatile double current_threshold   = THRESHOLD_INITIAL;

// --- Priorități FreeRTOS ---
// Pe ESP32-C3 (single-core): loopTask = 1, Wi-Fi/Sys = 20-23
// Taskul sporadic (alarme) are prioritatea cea mai mare (criticitate/urgență),
// urmat de cel periodic (telemetrie), iar aperiodic (comenzi de fundal) cel mai jos.
#define PRIORITY_APERIODIC 2
#define PRIORITY_PERIODIC  3
#define PRIORITY_SPORADIC  4

// --- Citire senzor intern ---
float read_temperature() {
    return temperatureRead();
}

// =============================================================================
// TODO 1: Task Periodic (Telemetrie și detectare prag)
// =============================================================================
void periodic_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    char payload[128];

    for (;;) {
        // Obținem perioada curentă
        uint32_t period = current_period_ms;
        
        // Așteptare absolută pe grila de eliberare pentru a preveni deviația (drift)
        xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(period));

        float temp = read_temperature();
        long long now = (long long)epoch_ms();

        // 1. Format payload: esp32-xxxxxx;timestamp;temperatura
        snprintf(payload, sizeof(payload), "%s;%lld;%.2f", DEVICE_ID, now, temp);
        mqtt_publish("telemetry", payload);
        Serial.printf("[PERIODIC] Telemetrie: %s\n", payload);

        // 2. Verificare prag de alarmă
        double thresh = current_threshold;
        if (temp > thresh) {
            AlarmEvent ev;
            ev.temperature = temp;
            ev.threshold = thresh;

            // Timeout 0: trimitere non-blocantă pentru a nu întârzia taskul periodic
            if (xQueueSend(alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[PERIODIC] Coada de alarme este plina! Eveniment ignorat.");
            }
        }
    }
}

// =============================================================================
// TODO 2: Task Sporadic (Procesare alarme cu filtru de inter-sosire)
// =============================================================================
void sporadic_task(void *pvParameters) {
    AlarmEvent ev;
    uint32_t last_alarm_ms = 0;
    char payload[128];

    for (;;) {
        // Așteaptă blocat până când apare un eveniment în coadă
        if (xQueueReceive(alarm_queue, &ev, portMAX_DELAY) == pdTRUE) {
            uint32_t now_ms = millis();

            // Verificare interval minim între două alarme consecutive
            if (last_alarm_ms == 0 || (now_ms - last_alarm_ms) >= MIN_INTERARRIVAL_MS) {
                last_alarm_ms = now_ms;
                long long now = (long long)epoch_ms();

                // Format payload: esp32-xxxxxx;timestamp;temperatura;prag
                snprintf(payload, sizeof(payload), "%s;%lld;%.2f;%.2f",
                         DEVICE_ID, now, ev.temperature, ev.threshold);
                
                mqtt_publish("alarm", payload);
                Serial.printf("[SPORADIC] Alarma emisa: %s\n", payload);
            } else {
                Serial.printf("[SPORADIC] Alarma respinsa: sub intervalul minim (%lu ms ramase)\n",
                              MIN_INTERARRIVAL_MS - (now_ms - last_alarm_ms));
            }
        }
    }
}

// =============================================================================
// TODO 3: Task Aperiodic (Execuție comenzi)
// =============================================================================
void aperiodic_task(void *pvParameters) {
    CommandMessage cmd;
    char status_payload[128];
    char reply[96];

    for (;;) {
        // Așteaptă comenzi din coadă
        if (xQueueReceive(command_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            long long now = (long long)epoch_ms();
            reply[0] = '\0';

            // Parsare comenzi
            if (strncmp(cmd.text, "SET_INTERVAL ", 13) == 0) {
                int new_period = atoi(cmd.text + 13);
                if (new_period > 0) {
                    current_period_ms = (uint32_t)new_period;
                    snprintf(reply, sizeof(reply), "OK SET_INTERVAL %d", new_period);
                } else {
                    snprintf(reply, sizeof(reply), "ERROR Invalid interval: %s", cmd.text + 13);
                }
            } else if (strncmp(cmd.text, "SET_THRESHOLD ", 14) == 0) {
                char *endptr;
                double new_thresh = strtod(cmd.text + 14, &endptr);
                if (endptr != cmd.text + 14) {
                    current_threshold = new_thresh;
                    snprintf(reply, sizeof(reply), "OK SET_THRESHOLD %.2f", new_thresh);
                } else {
                    snprintf(reply, sizeof(reply), "ERROR Invalid threshold: %s", cmd.text + 14);
                }
            } else if (strcmp(cmd.text, "RESET") == 0) {
                current_period_ms = PERIOD_MS_INITIAL;
                current_threshold = THRESHOLD_INITIAL;
                snprintf(reply, sizeof(reply), "OK RESET");
            } else if (strcmp(cmd.text, "STATUS") == 0) {
                snprintf(reply, sizeof(reply), "OK STATUS period=%lu threshold=%.2f",
                         current_period_ms, current_threshold);
            } else {
                snprintf(reply, sizeof(reply), "ERROR Unknown command: %s", cmd.text);
            }

            // Publicare pe topic-ul de status
            // Format: esp32-xxxxxx;timestamp;<raspuns>
            snprintf(status_payload, sizeof(status_payload), "%s;%lld;%s",
                     DEVICE_ID, now, reply);
            mqtt_publish("status", status_payload);
            Serial.printf("[APERIODIC] Status publicat: %s\n", status_payload);
        }
    }
}

// Callback apelat din clientul MQTT când sosește un mesaj pe topic-ul de comenzi
void command_received(const char *text) {
    if (command_queue == NULL || text == NULL) return;

    CommandMessage cmd;
    strncpy(cmd.text, text, COMMAND_MAX_LEN - 1);
    cmd.text[COMMAND_MAX_LEN - 1] = '\0';

    // Se adaugă non-blocant în coadă (suntem pe contextul taskului MQTT)
    if (xQueueSend(command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[MQTT Callback] Coada de comenzi este plina!");
    }
}

// =============================================================================
// TODO 0: Inițializare și pornire taskuri
// =============================================================================
void setup() {
    Serial.begin(115200);
    // Timeout 0 pentru TX pentru a preveni blocarea în Serial.printf când terminalul e deconectat
    Serial.setTxTimeoutMs(0);

    Serial.printf("\n--- Pornire dispozitiv %s ---\n", DEVICE_ID);

    // 1. Creare cozi FreeRTOS (inainte de mqtt_start)
    alarm_queue   = xQueueCreate(10, sizeof(AlarmEvent));
    command_queue = xQueueCreate(10, sizeof(CommandMessage));

    if (alarm_queue == NULL || command_queue == NULL) {
        Serial.println("[EROARE] Alocarea cozilor a esuat!");
        while (true) { delay(1000); }
    }

    // 2. Conectare retea si pornire servicii de baza
    wifi_connect();
    time_sync();
    mqtt_start();

    // 3. Pornire taskuri (dupa pornirea MQTT pentru a nu pierde pachete initiale)
    // Dimensiunea stivei: 4096 octeti pe ESP32
    xTaskCreate(
        periodic_task,
        "periodic_task",
        4096,
        NULL,
        PRIORITY_PERIODIC,
        NULL
    );

    xTaskCreate(
        sporadic_task,
        "sporadic_task",
        4096,
        NULL,
        PRIORITY_SPORADIC,
        NULL
    );

    xTaskCreate(
        aperiodic_task,
        "aperiodic_task",
        4096,
        NULL,
        PRIORITY_APERIODIC,
        NULL
    );

    Serial.println("[SETUP] Sistem initializat complet. Taskurile ruleaza.");
}

void loop() {
    // FreeRTOS gestioneaza totul; bucla Arduino principala ramane inactiva
    vTaskDelay(pdMS_TO_TICKS(1000));
}
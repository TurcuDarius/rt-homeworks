#include "config.h"
#include "connection.h"


enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// ============================================================================
// TODO 0 (c): STAREA SEMAFORULUI, MUTEXUL ȘI COADA BUTONULUI
// ============================================================================
struct TrafficLightState {
    Phase phase;
    uint32_t phase_entered_ms;
    bool ped_request;
};

struct ButtonPress {
    uint32_t timestamp_ms;
    bool from_isr; 
};

static TrafficLightState state = { CAR_GREEN, 0, false };
static SemaphoreHandle_t state_mutex = NULL;
static QueueHandle_t button_queue = NULL;

// ============================================================================
// TODO 1: TASK PERIODIC DE CONTROL (T = CONTROL_PERIOD_MS = 100 ms)
// ============================================================================
void control_task(void* pvParameters) {
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        uint32_t now = millis();
        bool changed = false;
        Phase new_phase = CAR_GREEN;

        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            uint32_t elapsed = now - state.phase_entered_ms;

            switch (state.phase) {
                case CAR_GREEN:
                    if (elapsed >= CAR_GREEN_MIN_MS && state.ped_request) {
                        state.phase = CAR_YELLOW;
                        state.phase_entered_ms = now;
                        changed = true;
                    }
                    break;

                case CAR_YELLOW:
                    if (elapsed >= CAR_YELLOW_MS) {
                        state.phase = ALL_RED;
                        state.phase_entered_ms = now;
                        changed = true;
                    }
                    break;

                case ALL_RED:
                    if (elapsed >= ALL_RED_MS) {
                        state.phase = PED_GREEN;
                        state.phase_entered_ms = now;
                        state.ped_request = false;  // se șterge cererea pietonului
                        changed = true;
                    }
                    break;

                case PED_GREEN:
                    if (elapsed >= PED_GREEN_MS) {
                        state.phase = CAR_GREEN;
                        state.phase_entered_ms = now;
                        changed = true;
                    }
                    break;
            }

            new_phase = state.phase;
            xSemaphoreGive(state_mutex);
        }

        if (changed) {
            Serial.printf("[control] fază nouă: %s\n", PHASE_NAMES[new_phase]);
        }
    }
}

// ============================================================================
// TODO 2: TASK PERIODIC DE IEȘIRE (T = OUTPUT_PERIOD_MS = 500 ms)
// ============================================================================
void output_task(void* pvParameters) {
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OUTPUT_PERIOD_MS);

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        // 1. Copiem starea local sub protecția mutexului
        TrafficLightState local_state;
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            local_state = state;
            xSemaphoreGive(state_mutex);
        } else {
            continue;
        }

        // 2. Stabilim culorile lămpilor în funcție de fază
        Light car_light = RED;
        Light ped_light = RED;

        switch (local_state.phase) {
            case CAR_GREEN:
                car_light = GREEN;
                ped_light = RED;
                break;
            case CAR_YELLOW:
                car_light = YELLOW;
                ped_light = RED;
                break;
            case ALL_RED:
                car_light = RED;
                ped_light = RED;
                break;
            case PED_GREEN:
                car_light = RED;
                ped_light = GREEN;
                break;
        }

        // 3. Calculăm secundele rămase pentru pietoni (rotunjite în sus)
        uint32_t remaining_sec = 0;
        if (local_state.phase == PED_GREEN) {
            uint32_t elapsed = millis() - local_state.phase_entered_ms;
            if (elapsed < PED_GREEN_MS) {
                uint32_t remaining_ms = PED_GREEN_MS - elapsed;
                remaining_sec = (remaining_ms + 999) / 1000;  // rotunjire în sus (ceil)
            }
        }

        // 4. Actualizăm LED-ul de pe placă (aprins pe LOW când AȘTEPTAȚI e activ)
        digitalWrite(LED_PIN, local_state.ped_request ? LOW : HIGH);

        // 5. Publicăm mesajul MQTT "lights"
        char payload[128];
        snprintf(payload, sizeof(payload), "%s;%llu;%s;%s;%s;%d;%lu",
                 DEVICE_ID,
                 (unsigned long long)epoch_ms(),
                 PHASE_NAMES[local_state.phase],
                 LIGHT_NAMES[car_light],
                 LIGHT_NAMES[ped_light],
                 local_state.ped_request ? 1 : 0,
                 (unsigned long)remaining_sec);

        mqtt_publish("lights", payload);
    }
}

// ============================================================================
// TODO 3: TASK SPORADIC ȘI RUTINA DE ÎNTRERUPERE (ISR)
// ============================================================================
void sporadic_button_task(void* pvParameters) {
    ButtonPress press;
    uint32_t last_accepted_ms = 0;
    bool has_accepted_before = false;

    for (;;) {
        if (xQueueReceive(button_queue, &press, portMAX_DELAY) == pdTRUE) {
            // Verificăm intervalul minim între sosiri (filtrează și bounce-ul)
            if (has_accepted_before && (press.timestamp_ms - last_accepted_ms < MIN_INTERARRIVAL_MS)) {
                Serial.printf("[buton] apăsare respinsă (dt = %lu ms < %d ms)\n",
                              (unsigned long)(press.timestamp_ms - last_accepted_ms),
                              MIN_INTERARRIVAL_MS);
                continue;
            }

            last_accepted_ms = press.timestamp_ms;
            has_accepted_before = true;

            if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                state.ped_request = true;
                xSemaphoreGive(state_mutex);
            }

            Serial.printf("[buton] cerere pieton acceptată (%s)\n",
                          press.from_isr ? "BOOT" : "MQTT");
        }
    }
}

void IRAM_ATTR button_isr(void) {
    if (button_queue == NULL) return;

    ButtonPress press;
    press.timestamp_ms = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    press.from_isr = true;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);
    portYIELD_FROM_ISR(woken);
}

void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului fără blocare (timeout 0)
    if (strcmp(text, "BUTTON") == 0 && button_queue != NULL) {
        ButtonPress press;
        press.timestamp_ms = millis();
        press.from_isr = false;
        xQueueSend(button_queue, &press, 0);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
    delay(200);
    Serial.println();
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): crearea mutexului, a cozii și inițializarea fazei de start
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(10, sizeof(ButtonPress));

    state.phase = CAR_GREEN;
    state.phase_entered_ms = millis();
    state.ped_request = false;

    mqtt_start();

    // TODO 0 (b): pornirea taskurilor (Rate Monotonic) și atașarea întreruperii
    // Priorități (2..19):
    //   - Control (T = 100 ms): prioritate mai mare (ex: 5)
    //   - Ieșire  (T = 500 ms): prioritate mai mică (ex: 4)
    //   - Sporadic (buton):     prioritate 6 (răspunde prompt la apăsări)
    xTaskCreate(sporadic_button_task, "button_task",  4096, NULL, 6, NULL);
    xTaskCreate(control_task,         "control_task", 4096, NULL, 5, NULL);
    xTaskCreate(output_task,          "output_task",  4096, NULL, 4, NULL);

    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    delay(1000);
}
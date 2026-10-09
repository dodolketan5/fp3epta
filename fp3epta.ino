#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2); 

// --- PIN DEFINITION ---
const int pinLEDHijau = 27; 
const int pinLEDMerah = 26; 
const int pinButton1  = 13; // Tombol Cetak
const int pinButton2  = 12; // Tombol Panggil
const int pinBuzzer   = 15;   

// --- VARIABEL COUNTER ANTRIAN ---
int tiketPrioritasTerakhir = 0;
int panggilPrioritasTerakhir = 0;

int tiketRegulerTerakhir = 0;
int panggilRegulerTerakhir = 0;

String statusPanggilAktif = "PANGGIL: --";

// --- RTOS SEMAPHORE HANDLES ---
SemaphoreHandle_t xSemCetakReguler   = NULL;
SemaphoreHandle_t xSemCetakPrioritas = NULL;
SemaphoreHandle_t xSemPanggil        = NULL;
SemaphoreHandle_t xMutexLCD          = NULL;

// Prototipe Fungsi
void TaskInput(void *pvParameters);
void TaskProcess(void *pvParameters);
void updateDisplay(String baris1, String baris2);
void dibunyikanBuzzer(int frekuensi, int durasiMs);
void refreshStandbyDisplay();

void setup() {
  Serial.begin(115200);

  pinMode(pinLEDHijau, OUTPUT);
  pinMode(pinLEDMerah, OUTPUT);
  pinMode(pinButton1, INPUT_PULLUP); 
  pinMode(pinButton2, INPUT_PULLUP); 
  pinMode(pinBuzzer, OUTPUT);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  // Inisialisasi Binary Semaphores
  vSemaphoreCreateBinary(xSemCetakReguler);
  vSemaphoreCreateBinary(xSemCetakPrioritas);
  vSemaphoreCreateBinary(xSemPanggil);
  
  xSemaphoreTake(xSemCetakReguler, 0);
  xSemaphoreTake(xSemCetakPrioritas, 0);
  xSemaphoreTake(xSemPanggil, 0);

  xMutexLCD = xSemaphoreCreateMutex();

  updateDisplay("SMART QUEUE RTOS", "Sistem Siap...");
  vTaskDelay(pdMS_TO_TICKS(1500));
  
  refreshStandbyDisplay();

  Serial.println("=== SMART QUEUE RTOS (PRIORITY QUEUE ENABLED) ===");

  xTaskCreatePinnedToCore(TaskInput,   "TaskInput",   2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskProcess, "TaskProcess", 4096, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000)); 
}

// ==========================================
// TASK 1: BACA TOMBOL + DETEKSI TAHAN (CORE 1)
// ==========================================
void TaskInput(void *pvParameters) {
  unsigned long pressTimeBtn1 = 0;
  bool isPressingBtn1 = false;

  unsigned long lastDebounce2 = 0;
  const unsigned long debounceDelay = 250;

  for (;;) {
    // --- BACA TOMBOL 1 (CETAK: REGULER VS PRIORITAS) ---
    if (digitalRead(pinButton1) == LOW) {
      if (!isPressingBtn1) {
        isPressingBtn1 = true;
        pressTimeBtn1 = millis(); // Catat waktu awal tekan
      }
    } else {
      if (isPressingBtn1) {
        unsigned long holdDuration = millis() - pressTimeBtn1;
        isPressingBtn1 = false;

        if (holdDuration >= 1000) {
          // Tahan >= 1 Detik -> PRIORITAS
          xSemaphoreGive(xSemCetakPrioritas);
          Serial.println("[INPUT] Cetak Prioritas Sent (Hold >= 1s)");
        } else if (holdDuration >= 50) {
          // Klik Singkat (< 0.5 Detik) -> REGULER
          xSemaphoreGive(xSemCetakReguler);
          Serial.println("[INPUT] Cetak Reguler Sent (Short Press)");
        }
      }
    }

    // --- BACA TOMBOL 2 (PANGGIL) ---
    if (digitalRead(pinButton2) == LOW) {
      if (millis() - lastDebounce2 > debounceDelay) {
        lastDebounce2 = millis();
        xSemaphoreGive(xSemPanggil);
        Serial.println("[INPUT] Signal Panggil Sent");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ==========================================
// TASK 2: OLAH PRIORITAS & REGULER (CORE 0)
// ==========================================
void TaskProcess(void *pvParameters) {
  char bufBaris1[17];
  char bufBaris2[17];

  for (;;) {
    // 1. EVENT CETAK REGULER
    if (xSemaphoreTake(xSemCetakReguler, 0) == pdTRUE) {
      tiketRegulerTerakhir++;
      sprintf(bufBaris1, "CETAK REGULER");
      sprintf(bufBaris2, "NOMOR: R%02d", tiketRegulerTerakhir);
      updateDisplay(bufBaris1, bufBaris2);

      digitalWrite(pinLEDHijau, HIGH);
      dibunyikanBuzzer(2000, 150);
      vTaskDelay(pdMS_TO_TICKS(800));
      digitalWrite(pinLEDHijau, LOW);

      refreshStandbyDisplay();
    }

    // 2. EVENT CETAK PRIORITAS
    if (xSemaphoreTake(xSemCetakPrioritas, 0) == pdTRUE) {
      tiketPrioritasTerakhir++;
      sprintf(bufBaris1, "CETAK PRIORITAS");
      sprintf(bufBaris2, "NOMOR: P%02d", tiketPrioritasTerakhir);
      updateDisplay(bufBaris1, bufBaris2);

      // Beep 2x untuk penanda Prioritas
      digitalWrite(pinLEDHijau, HIGH);
      dibunyikanBuzzer(5000, 100);
      vTaskDelay(pdMS_TO_TICKS(500));
      dibunyikanBuzzer(5000, 150);
      digitalWrite(pinLEDHijau, LOW);

      refreshStandbyDisplay();
    }

    // 3. EVENT PANGGIL (UTAMAKAN PRIORITAS DULU)
    if (xSemaphoreTake(xSemPanggil, 0) == pdTRUE) {
      int sisaPrioritas = tiketPrioritasTerakhir - panggilPrioritasTerakhir;
      int sisaReguler   = tiketRegulerTerakhir - panggilRegulerTerakhir;

      if (sisaPrioritas > 0) {
        // Melayani Antrian Prioritas Terlebih Dahulu
        panggilPrioritasTerakhir++;
        char strPanggil[10];
        sprintf(strPanggil, "P%02d", panggilPrioritasTerakhir);
        statusPanggilAktif = "PANGGIL: " + String(strPanggil);

        refreshStandbyDisplay();

        digitalWrite(pinLEDMerah, HIGH);
        dibunyikanBuzzer(5000, 300);
        vTaskDelay(pdMS_TO_TICKS(100));
        dibunyikanBuzzer(5000, 400);
        digitalWrite(pinLEDMerah, LOW);

      } else if (sisaReguler > 0) {
        // Jika Prioritas Habis, Layani Antrian Reguler
        panggilRegulerTerakhir++;
        char strPanggil[10];
        sprintf(strPanggil, "R%02d", panggilRegulerTerakhir);
        statusPanggilAktif = "PANGGIL: " + String(strPanggil);

        refreshStandbyDisplay();

        digitalWrite(pinLEDMerah, HIGH);
        dibunyikanBuzzer(1000, 400);
        digitalWrite(pinLEDMerah, LOW);

      } else {
        // Antrian Kosong
        updateDisplay("ANTRIAN KOSONG!", "Tidak ada tiket");

        for (int i = 0; i < 2; i++) {
          digitalWrite(pinLEDMerah, HIGH);
          dibunyikanBuzzer(500, 100);
          vTaskDelay(pdMS_TO_TICKS(100));
          digitalWrite(pinLEDMerah, LOW);
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        refreshStandbyDisplay();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ==========================================
// FUNGSI UTILITY & DISPLAY DRIVER
// ==========================================
void refreshStandbyDisplay() {
  int sisaP = tiketPrioritasTerakhir - panggilPrioritasTerakhir;
  int sisaR = tiketRegulerTerakhir - panggilRegulerTerakhir;

  char bufBaris2[17];
  sprintf(bufBaris2, "SISA P:%02d R:%02d", sisaP, sisaR);

  updateDisplay(statusPanggilAktif, String(bufBaris2));
}

void updateDisplay(String baris1, String baris2) {
  if (xSemaphoreTake(xMutexLCD, portMAX_DELAY) == pdTRUE) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(baris1);
    lcd.setCursor(0, 1);
    lcd.print(baris2);
    xSemaphoreGive(xMutexLCD);
  }
}

void dibunyikanBuzzer(int frekuensi, int durasiMs) {
  #if defined(ESP32)
    tone(pinBuzzer, frekuensi, durasiMs);
    vTaskDelay(pdMS_TO_TICKS(durasiMs));
    noTone(pinBuzzer);
  #endif
}
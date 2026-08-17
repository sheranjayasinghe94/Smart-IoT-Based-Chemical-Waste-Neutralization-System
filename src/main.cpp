#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>

// ==================== AZURE IOT HUB ====================
#include <AzureIoTHub.h>
#include <AzureIoTProtocol_MQTT.h>
#include <iothubtransportmqtt.h>
#include <AzureIoTUtility.h>
#include <certs/certs.h>
#include <time.h>

// Azure IoT Hub connection string
// Replace with your Azure IoT Hub connection string
// Format: HostName=<your-hub>.azure-devices.net;DeviceId=<device-id>;SharedAccessKey=<key>
static const char* connectionString = "YOUR_AZURE_CONNECTION_STRING";

// Azure IoT Hub client handle
static IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle;

// ============== CONFIGURATION VARIABLES ====================
float NEUTRAL_PH_MIN = 6.7;      // Lower bound of neutral pH
float NEUTRAL_PH_MAX = 7.5;      // Upper bound of neutral pH

// ==================== BLYNK CREDENTIALS ====================
// Replace with your Blynk credentials from Blynk app
#define BLYNK_TEMPLATE_ID "YOUR_BLYNK_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "YOUR_TEMPLATE_NAME"
#define BLYNK_AUTH_TOKEN "YOUR_BLYNK_AUTH_TOKEN"

// ==================== WiFi CREDENTIALS ====================
// Replace with your WiFi network credentials
char ssid[] = "YOUR_SSID";
char pass[] = "YOUR_PASSWORD";

#include <BlynkSimpleEsp32.h>

// ==================== BLYNK VIRTUAL PINS ====================
#define VPIN_PH_VALUE          V0   // Current pH value (Gauge)
#define VPIN_VOLTAGE           V1   // Current voltage (Value Display)
#define VPIN_STATUS            V2   // Status text (Label)
#define VPIN_ACTION            V3   // Recommended action (Label)
#define VPIN_PH_CHANGES        V4   // pH change count (Value Display)
#define VPIN_RUNTIME           V5   // Runtime in minutes (Value Display)
#define VPIN_IS_NEUTRAL        V6   // Neutral indicator (LED)
#define VPIN_NEUTRAL_VOLTAGE   V7   // Calibration neutral voltage (Value Display)
#define VPIN_VOLTAGE_SLOPE     V8   // Calibration slope (Value Display)
#define VPIN_PH_CHART          V9   // pH history chart (SuperChart)
#define VPIN_TERMINAL          V10  // Terminal for logs (Terminal)

// ==================== PIN DEFINITIONS ====================
// OLED Display (I2C)
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// pH Sensor
#define PH_SENSOR_PIN 1  // ADC pin

// Buzzer
#define BUZZER_PIN 18

// ==================== GLOBAL OBJECTS ====================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
BlynkTimer timer;

// ==================== pH SENSOR CALIBRATION ====================
#define ADC_MAX 4095.0
#define VREF 3.3

// CALIBRATION - Adjust these based on your sensor
#define PH_NEUTRAL_VOLTAGE 2.5    // Voltage at pH 7.0 - CALIBRATE THIS!
#define PH_VOLTAGE_SLOPE   0.17   // Voltage change per pH unit

// EMA Filter for stable readings
float filteredVoltage = 2.5;
#define ALPHA 0.12   // Filter response speed (0.1 - 0.2 recommended)

// ==================== STATE VARIABLES ====================
float currentPH = 7.0;
float previousPH = 7.0;
bool isNeutral = false;
bool wasNeutral = false;

// Statistics
int phChangeCount = 0;
unsigned long startTime = 0;
unsigned long totalRuntime = 0;

// Azure IoT connection status
bool azureConnected = false;
unsigned long lastAzureSendTime = 0;
unsigned long lastBlynkSendTime = 0;
#define AZURE_SEND_INTERVAL 2000  // Send to Azure every 2 seconds (same as Blynk for live updates)

// Live update counters
int azureUpdateCount = 0;
int blynkUpdateCount = 0;

// ==================== FUNCTION DECLARATIONS ====================
float readPH();
void displayPH(float ph, String status, String action);
void activateBuzzer(int times);
void sendDataToBlynk();
void sendTerminalLog(String message);
void initializeAzureIoT();
void sendDataToAzure();
void azureIoTDoWork();

// ==================== AZURE IOT FUNCTIONS ====================

// Callback for when message is confirmed sent to Azure IoT Hub
static void sendConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback) {
  if (result == IOTHUB_CLIENT_CONFIRMATION_OK) {
    Serial.println("  ✓ Azure: Message confirmed delivered");
  } else {
    Serial.println("  ✗ Azure: Delivery failed");
  }
}

// Initialize Azure IoT Hub connection
void initializeAzureIoT() {
  Serial.println("\n=== Initializing Azure IoT Hub ===");
  
  // Initialize the IoTHub client
  if (platform_init() != 0) {
    Serial.println("Azure IoT: Failed to initialize platform");
    azureConnected = false;
    return;
  }
  
  // Create IoT Hub client handle
  iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol);
  
  if (iotHubClientHandle == NULL) {
    Serial.println("Azure IoT: Failed to create IoT Hub client");
    azureConnected = false;
    return;
  }
  
  // Set option to log trace
  bool traceOn = true;
  IoTHubClient_LL_SetOption(iotHubClientHandle, "logtrace", &traceOn);

  // Connection status callback for detailed connect/disconnect reasons
  auto connectionStatusCallback = [](IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContext) {
    Serial.print("[Azure] Connection status: ");
    Serial.println(result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED ? "Authenticated" : "Disconnected");
    Serial.print("[Azure] Reason: ");
    switch (reason) {
      case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN: Serial.println("Expired SAS token"); break;
      case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED: Serial.println("Device disabled"); break;
      case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR: Serial.println("Communication error"); break;
      case IOTHUB_CLIENT_CONNECTION_NO_NETWORK: Serial.println("No network"); break;
      case IOTHUB_CLIENT_CONNECTION_OK: Serial.println("OK"); break;
      default: Serial.println("Other/Unknown"); break;
    }
  };
  IoTHubClient_LL_SetConnectionStatusCallback(iotHubClientHandle, connectionStatusCallback, NULL);

  // Provide Trusted Root Certificates for TLS (from library certs)
  IoTHubClient_LL_SetOption(iotHubClientHandle, "TrustedCerts", (void*)certificates);
  
  // Set keep alive interval
  int keepAlive = 20;
  IoTHubClient_LL_SetOption(iotHubClientHandle, "keepalive", &keepAlive);
  
  Serial.println("Azure IoT Hub: Connected successfully!");
  Serial.println("Device ID: Configure in connection string");
  azureConnected = true;
  
  sendTerminalLog("Azure IoT Hub Connected!");
}

// Send data to Azure IoT Hub
void sendDataToAzure() {
  if (!azureConnected || iotHubClientHandle == NULL) {
    return;
  }
  
  azureUpdateCount++;
  
  // Create JSON payload
  String payload = "{";
  payload += "\"deviceId\":\"ChemicalWasteMonitor\",";
  payload += "\"pH\":" + String(currentPH, 2) + ",";
  payload += "\"voltage\":" + String(filteredVoltage, 3) + ",";
  payload += "\"isNeutral\":" + String(isNeutral ? "true" : "false") + ",";
  payload += "\"status\":\"";
  
  if (isNeutral) {
    payload += "NEUTRAL";
  } else if (currentPH < NEUTRAL_PH_MIN) {
    payload += "ACIDIC";
  } else {
    payload += "BASIC";
  }
  
  payload += "\",";
  payload += "\"phChangeCount\":" + String(phChangeCount) + ",";
  payload += "\"runtimeMinutes\":" + String(totalRuntime) + ",";
  payload += "\"updateCount\":" + String(azureUpdateCount) + ",";
  payload += "\"timestamp\":" + String(millis());
  payload += "}";
  
  // Create IoT Hub message
  IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray(
    (const unsigned char*)payload.c_str(), 
    payload.length()
  );
  
  if (messageHandle == NULL) {
    Serial.println("Azure IoT: Failed to create message");
    return;
  }
  
  // Send the message
  if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, messageHandle, sendConfirmationCallback, NULL) != IOTHUB_CLIENT_OK) {
    Serial.println("Azure IoT: Failed to send message");
  } else {
    Serial.print("Azure IoT Update #");
    Serial.print(azureUpdateCount);
    Serial.println(" - Message sent");
  }
  
  // Cleanup
  IoTHubMessage_Destroy(messageHandle);
}

// Process Azure IoT Hub work (must be called regularly)
void azureIoTDoWork() {
  if (azureConnected && iotHubClientHandle != NULL) {
    IoTHubClient_LL_DoWork(iotHubClientHandle);
  }
}

// ==================== BLYNK FUNCTIONS ====================
// Send data to Blynk every 2 seconds
void sendDataToBlynk() {
  blynkUpdateCount++;
  
  // Send current pH value
  Blynk.virtualWrite(VPIN_PH_VALUE, currentPH);
  
  // Send voltage
  Blynk.virtualWrite(VPIN_VOLTAGE, filteredVoltage);
  
  // Send status
  String status = "";
  if (isNeutral) {
    status = "NEUTRAL ✓";
  } else if (currentPH < NEUTRAL_PH_MIN) {
    status = "ACIDIC ⚠";
  } else {
    status = "BASIC ⚠";
  }
  Blynk.virtualWrite(VPIN_STATUS, status);
  
  // Send action recommendation
  String action = "";
  if (isNeutral) {
    action = "Safe for disposal";
  } else if (currentPH < NEUTRAL_PH_MIN) {
    action = "Add BASE to neutralize";
  } else {
    action = "Add ACID to neutralize";
  }
  Blynk.virtualWrite(VPIN_ACTION, action);
  
  // Send pH change count
  Blynk.virtualWrite(VPIN_PH_CHANGES, phChangeCount);
  
  // Calculate and send runtime in minutes
  totalRuntime = (millis() - startTime) / 60000; // Convert to minutes
  Blynk.virtualWrite(VPIN_RUNTIME, totalRuntime);
  
  // Send neutral indicator (LED)
  if (isNeutral) {
    Blynk.virtualWrite(VPIN_IS_NEUTRAL, 255); // LED ON (Green)
  } else {
    Blynk.virtualWrite(VPIN_IS_NEUTRAL, 0);   // LED OFF
  }
  
  // Send calibration values
  Blynk.virtualWrite(VPIN_NEUTRAL_VOLTAGE, PH_NEUTRAL_VOLTAGE);
  Blynk.virtualWrite(VPIN_VOLTAGE_SLOPE, PH_VOLTAGE_SLOPE);
}

// Send message to Blynk Terminal
void sendTerminalLog(String message) {
  Blynk.virtualWrite(VPIN_TERMINAL, message + "\n");
}

// Handle connection to Blynk
BLYNK_CONNECTED() {
  Serial.println("Connected to Blynk!");
  sendTerminalLog("=== System Connected ===");
  sendTerminalLog("pH Monitor Online");
  
  // Send initial data
  sendDataToBlynk();
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n===========================================");
  Serial.println("Chemical Waste pH Monitoring System");
  Serial.println("with Blynk IoT + Azure IoT Hub Integration");
  Serial.println("===========================================\n");
  
  // Initialize I2C for OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  
  // Initialize OLED Display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("ERROR: SSD1306 OLED not found!"));
    Serial.println(F("Check connections:"));
    Serial.println(F("  VDD -> 3.3V"));
    Serial.println(F("  GND -> GND"));
    Serial.println(F("  SDA -> GPIO 8"));
    Serial.println(F("  SCL -> GPIO 9"));
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Chemical Waste");
  display.println("pH Monitor");
  display.println("=============");
  display.println("");
  display.println("Connecting...");
  display.display();
  
  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Initialize pH Sensor Pin
  pinMode(PH_SENSOR_PIN, INPUT);
  analogReadResolution(12); // 12-bit ADC resolution
  
  // Connect to Blynk
  Serial.println("Connecting to Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  
  // Wait for connection
  while (Blynk.connected() == false) {
    delay(100);
  }
  
  Serial.println("Connected to Blynk Cloud!");
  
  // Setup timer to send data to BOTH platforms every 2 seconds for live updates
  timer.setInterval(2000L, []() {
    sendDataToBlynk();
    sendDataToAzure();  // Send to Azure at same time as Blynk
  });
  
  // Sync system time via NTP before initializing TLS connections
  Serial.println("Syncing time (NTP)...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 1600000000 && tries < 15) {
    Serial.print(".");
    delay(1000);
    now = time(nullptr);
    tries++;
  }
  if (now < 1600000000) {
    Serial.println("\nWarning: time sync failed; TLS may fail until time is set");
  } else {
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.print("\nTime synced: ");
    Serial.print(asctime(&timeinfo));
  }

  // Initialize Azure IoT Hub (after time sync and timer setup)
  initializeAzureIoT();
  
  // Record start time
  startTime = millis();
  
  // Startup beep
  delay(1000);
  activateBuzzer(2);
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("System Ready!");
  display.println("Blynk: Connected");
  display.print("Azure: ");
  display.println(azureConnected ? "Connected" : "Failed");
  display.println("");
  display.println("Monitoring pH...");
  display.display();
  delay(2000);
  
  Serial.println("System Ready!");
  Serial.println("Starting pH monitoring...\n");
}

// ==================== MAIN LOOP ====================
void loop() {
  // Run Blynk
  Blynk.run();
  timer.run();
  
  // Process Azure IoT Hub work
  azureIoTDoWork();
  
  // Read current pH value
  currentPH = readPH();
  
  // Detect significant pH changes (>0.5 pH units)
  if (abs(currentPH - previousPH) > 0.5) {
    phChangeCount++;
    Serial.print("  >> pH CHANGE DETECTED! Count: ");
    Serial.println(phChangeCount);
    
    // Log to Blynk Terminal
    String logMsg = "pH Change #" + String(phChangeCount) + 
                    " | From: " + String(previousPH, 2) + 
                    " To: " + String(currentPH, 2);
    sendTerminalLog(logMsg);
    
    // Immediately send to Azure on pH change for live updates
    sendDataToAzure();
  }
  previousPH = currentPH;
  
  // Check if waste is neutral
  isNeutral = (currentPH >= NEUTRAL_PH_MIN && currentPH <= NEUTRAL_PH_MAX);
  
  // Determine status and action needed
  String status = "";
  String action = "";
  
  if (isNeutral) {
    status = "NEUTRAL";
    action = "Safe for disposal";
    
    // Alert when becoming neutral
    if (!wasNeutral) {
      Serial.println("\n*** WASTE IS NOW NEUTRAL! ***");
      Serial.println("Safe for disposal.\n");
      
      // Send notification to Blynk
      Blynk.logEvent("waste_neutral", "Chemical waste is now NEUTRAL and safe for disposal!");
      sendTerminalLog("*** NEUTRALIZATION COMPLETE ***");
      sendTerminalLog("pH: " + String(currentPH, 2) + " - Safe for disposal");
      
      // Send alert to Azure IoT Hub
      sendDataToAzure();
      
      activateBuzzer(5);
      wasNeutral = true;
    }
  } 
  else if (currentPH < NEUTRAL_PH_MIN) {
    status = "ACIDIC";
    action = "Add BASE to neutralize";
    wasNeutral = false;
  } 
  else if (currentPH > NEUTRAL_PH_MAX) {
    status = "BASIC";
    action = "Add ACID to neutralize";
    wasNeutral = false;
  }
  
  // Update display
  displayPH(currentPH, status, action);
  
  delay(300);
}

// ==================== pH READING ====================
float readPH() {
  int adc = analogRead(PH_SENSOR_PIN);
  float voltage = (adc / ADC_MAX) * VREF;
  
  // Apply EMA filter for stable readings
  filteredVoltage = (ALPHA * voltage) + ((1 - ALPHA) * filteredVoltage);
  
  // Calculate pH from voltage
  float pH = 7.0 - ((filteredVoltage - PH_NEUTRAL_VOLTAGE) / PH_VOLTAGE_SLOPE);
  pH = constrain(pH, 0.0, 14.0);
  
  // Determine type
  String phType = "";
  if (pH < 6.5) {
    phType = "ACIDIC";
  } else if (pH >= 6.5 && pH <= 7.5) {
    phType = "NEUTRAL";
  } else {
    phType = "BASIC";
  }
  
  // Print detailed reading to serial monitor
  Serial.print("ADC: ");
  Serial.print(adc);
  Serial.print("  Voltage: ");
  Serial.print(filteredVoltage, 3);
  Serial.print(" V  pH: ");
  Serial.print(pH, 2);
  Serial.print("  (");
  Serial.print(phType);
  Serial.println(")");
  
  return pH;
}

// ==================== DISPLAY FUNCTIONS ====================
void displayPH(float ph, String status, String action) {
  display.clearDisplay();
  
  // Title
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Chemical Waste");
  display.println("pH Monitor");
  display.drawLine(0, 18, 128, 18, SSD1306_WHITE);
  
  // pH Value (Large)
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.print("pH: ");
  display.println(ph, 1);
  
  // Status
  display.setTextSize(1);
  display.setCursor(0, 44);
  display.print("Status: ");
  display.println(status);
  
  // Action/Info
  display.setCursor(0, 54);
  display.println(action);
  
  display.display();
}

// ==================== BUZZER ====================
void activateBuzzer(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
    delay(200);
  }
}

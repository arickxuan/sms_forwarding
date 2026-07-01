#include "globals.h"

Config config;
Preferences preferences;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);
DNSServer dnsServer;
bool configValid = false;
bool timeSynced = false;
bool modemReady = false;
bool provisioningMode = false;
unsigned long lastPrintTime = 0;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

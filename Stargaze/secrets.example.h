// secrets.example.h
// Copy this file to "secrets.h" in the same folder and put your own WiFi details in it.
// secrets.h is listed in .gitignore, so your passwords never get uploaded to GitHub.

#pragma once

// Primary WiFi
#define WIFI_SSID  "YourPrimarySSID"
#define WIFI_PASS  "YourPrimaryPassword"

// Secondary WiFi (used when the primary isn't available).
// Leave WIFI_SSID2 as "" if you don't have one.
#define WIFI_SSID2 "YourBackupSSID"
#define WIFI_PASS2 "YourBackupPassword"

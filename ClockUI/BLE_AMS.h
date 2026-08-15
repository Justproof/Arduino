#pragma once
#include <stdint.h>
#include <Arduino.h>

// Apple Media Service Remote Command IDs
enum AmsRemoteCmd : uint8_t {
  AMS_CMD_PLAY = 0,
  AMS_CMD_PAUSE = 1,
  AMS_CMD_TOGGLE_PLAY_PAUSE = 2,
  AMS_CMD_NEXT_TRACK = 3,
  AMS_CMD_PREVIOUS_TRACK = 4,
  AMS_CMD_VOLUME_UP = 5,
  AMS_CMD_VOLUME_DOWN = 6,
  AMS_CMD_SKIP_FORWARD = 9,
  AMS_CMD_SKIP_BACKWARD = 10,
};

void BLE_AMS_OnBonded(uint16_t conn_handle);
void BLE_AMS_OnDisconnect();

// UI accessors. Cached values; updated via BLE notifications.
const String& BLE_AMS_GetTitle();
const String& BLE_AMS_GetArtist();
const String& BLE_AMS_GetAlbum();
bool BLE_AMS_IsPlaying();
bool BLE_AMS_IsConnected();
float BLE_AMS_GetPlaybackRate();  // 1.0 normal speed, 0.0 while paused
float BLE_AMS_GetElapsedSec();    // extrapolated from the last PlaybackInfo
float BLE_AMS_GetDurationSec();   // 0 if the player never reported it
float BLE_AMS_GetVolume();        // 0..1, -1 until the first Volume update

// Send a Remote Command (no-op if not connected).
void BLE_AMS_SendCommand(AmsRemoteCmd cmd);

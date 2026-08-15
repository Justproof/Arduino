#include "BLE_AMS.h"
#include <Arduino.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>

// Apple Media Service UUIDs. NimBLE wants 128-bit UUIDs in little-endian byte order.
// Service:        89D3502B-0F36-433A-8EF4-C502AD55F8DC
// Remote Command: 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2
// Entity Update:  2F7CABCE-808D-411F-9A0C-BB92BA96C102
// Entity Attr:    C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7

static const ble_uuid128_t UUID_AMS_SVC = BLE_UUID128_INIT(
  0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
  0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);

static const ble_uuid128_t UUID_AMS_REMOTE = BLE_UUID128_INIT(
  0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8,
  0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);

static const ble_uuid128_t UUID_AMS_ENTITY_UPDATE = BLE_UUID128_INIT(
  0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A,
  0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);

static const ble_uuid128_t UUID_AMS_ENTITY_ATTR = BLE_UUID128_INIT(
  0xD7, 0xD5, 0xBB, 0x70, 0xA8, 0xA3, 0xAB, 0xA6,
  0xD8, 0x46, 0xAB, 0x23, 0x8C, 0xF3, 0xB2, 0xC6);

// Entity / attribute IDs from the AMS spec.
enum : uint8_t { ENTITY_PLAYER = 0, ENTITY_QUEUE = 1, ENTITY_TRACK = 2 };
enum : uint8_t { PLAYER_NAME = 0, PLAYER_PLAYBACK_INFO = 1, PLAYER_VOLUME = 2 };
enum : uint8_t { TRACK_ARTIST = 0, TRACK_ALBUM = 1, TRACK_TITLE = 2, TRACK_DURATION = 3 };

struct AmsHandles {
  uint16_t conn_handle = 0xFFFF;
  uint16_t svc_start = 0;
  uint16_t svc_end = 0;
  uint16_t hdl_remote = 0;
  uint16_t hdl_entity_update = 0;
  uint16_t hdl_entity_update_cccd = 0;
  uint16_t hdl_entity_attr = 0;
  bool ready = false;
};

static AmsHandles ams;
static struct ble_gap_event_listener notify_listener;
static bool listener_registered = false;

static String s_title;
static String s_artist;
static String s_album;
static bool s_playing = false;
static float s_rate = 0.0f;
static float s_elapsed = 0.0f;
static uint32_t s_elapsed_ms = 0;   // millis() when s_elapsed was reported
static float s_duration = 0.0f;
static float s_volume = -1.0f;

const String& BLE_AMS_GetTitle()  { return s_title; }
const String& BLE_AMS_GetArtist() { return s_artist; }
const String& BLE_AMS_GetAlbum()  { return s_album; }
bool BLE_AMS_IsPlaying()          { return s_playing; }
bool BLE_AMS_IsConnected()        { return ams.ready; }
float BLE_AMS_GetPlaybackRate()   { return s_playing ? s_rate : 0.0f; }
float BLE_AMS_GetDurationSec()    { return s_duration; }
float BLE_AMS_GetVolume()         { return s_volume; }

float BLE_AMS_GetElapsedSec() {
  if (!s_playing) return s_elapsed;
  float rate = (s_rate > 0.0f) ? s_rate : 1.0f;
  return s_elapsed + (millis() - s_elapsed_ms) / 1000.0f * rate;
}

static int gap_event_cb(struct ble_gap_event* event, void* arg);

static int write_interest(uint16_t conn, uint16_t handle,
                          const uint8_t* data, uint16_t len) {
  return ble_gattc_write_flat(conn, handle, data, len, nullptr, nullptr);
}

static void subscribe_and_register_interests() {
  // 1. Subscribe to Entity Update notifications: write 0x0001 to CCCD.
  const uint8_t cccd_enable[2] = { 0x01, 0x00 };
  int rc = ble_gattc_write_flat(ams.conn_handle, ams.hdl_entity_update_cccd,
    cccd_enable, sizeof(cccd_enable), nullptr, nullptr);
  Serial.printf("[AMS] subscribe CCCD@%u rc=0x%04x\n", ams.hdl_entity_update_cccd, rc);

  // 2. Register interest in Track attributes (artist, album, title, duration).
  const uint8_t track_attrs[] = {
    ENTITY_TRACK, TRACK_ARTIST, TRACK_ALBUM, TRACK_TITLE, TRACK_DURATION
  };
  rc = write_interest(ams.conn_handle, ams.hdl_entity_update,
    track_attrs, sizeof(track_attrs));
  Serial.printf("[AMS] register track interests rc=0x%04x\n", rc);

  // 3. Register interest in Player.PlaybackInfo (play state, rate, elapsed)
  //    and Player.Volume (drives the scope trace amplitude).
  const uint8_t player_attrs[] = {
    ENTITY_PLAYER, PLAYER_PLAYBACK_INFO, PLAYER_VOLUME
  };
  rc = write_interest(ams.conn_handle, ams.hdl_entity_update,
    player_attrs, sizeof(player_attrs));
  Serial.printf("[AMS] register player interests rc=0x%04x\n", rc);

  // 4. Register a single GAP event listener for incoming notifications.
  if (!listener_registered) {
    rc = ble_gap_event_listener_register(&notify_listener, gap_event_cb, nullptr);
    Serial.printf("[AMS] listener register rc=0x%04x\n", rc);
    listener_registered = (rc == 0);
  }
  ams.ready = true;
}

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc,
                       void* arg) {
  if (error->status == BLE_HS_EDONE) {
    if (ams.hdl_entity_update_cccd == 0) {
      Serial.println("[AMS] no CCCD found for Entity Update");
      return 0;
    }
    subscribe_and_register_interests();
    return 0;
  }
  if (error->status != 0) {
    Serial.printf("[AMS] dsc discovery error: 0x%04x\n", error->status);
    return 0;
  }
  // CCCD UUID is 0x2902.
  if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
      ((const ble_uuid16_t*)&dsc->uuid)->value == 0x2902 &&
      ams.hdl_entity_update_cccd == 0) {
    ams.hdl_entity_update_cccd = dsc->handle;
  }
  return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                       const struct ble_gatt_chr* chr, void* arg) {
  if (error->status == BLE_HS_EDONE) {
    Serial.printf("[AMS] char discovery done: remote=%u upd=%u attr=%u\n",
      ams.hdl_remote, ams.hdl_entity_update, ams.hdl_entity_attr);
    if (ams.hdl_entity_update == 0) {
      Serial.println("[AMS] entity update not found, aborting");
      return 0;
    }
    // Discover descriptors for Entity Update (to find its CCCD).
    int rc = ble_gattc_disc_all_dscs(conn_handle, ams.hdl_entity_update,
      ams.svc_end, dsc_disc_cb, nullptr);
    if (rc != 0) Serial.printf("[AMS] disc_all_dscs failed: 0x%04x\n", rc);
    return 0;
  }
  if (error->status != 0) {
    Serial.printf("[AMS] char discovery error: 0x%04x\n", error->status);
    return 0;
  }
  if (ble_uuid_cmp(&chr->uuid.u, &UUID_AMS_REMOTE.u) == 0) {
    ams.hdl_remote = chr->val_handle;
  } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_AMS_ENTITY_UPDATE.u) == 0) {
    ams.hdl_entity_update = chr->val_handle;
  } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_AMS_ENTITY_ATTR.u) == 0) {
    ams.hdl_entity_attr = chr->val_handle;
  }
  return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                       const struct ble_gatt_svc* service, void* arg) {
  if (error->status == BLE_HS_EDONE) {
    if (ams.svc_start == 0) {
      Serial.println("[AMS] service not found on peer");
      return 0;
    }
    Serial.printf("[AMS] svc found: handles %u..%u, enumerating chars\n",
      ams.svc_start, ams.svc_end);
    int rc = ble_gattc_disc_all_chrs(conn_handle, ams.svc_start, ams.svc_end,
      chr_disc_cb, nullptr);
    if (rc != 0) Serial.printf("[AMS] disc_all_chrs failed: 0x%04x\n", rc);
    return 0;
  }
  if (error->status != 0) {
    Serial.printf("[AMS] svc discovery error: 0x%04x\n", error->status);
    return 0;
  }
  ams.svc_start = service->start_handle;
  ams.svc_end = service->end_handle;
  return 0;
}

// Parse an Entity Update notification:
//   byte 0: entity ID
//   byte 1: attribute ID
//   byte 2: flags (bit 0 = truncated)
//   bytes 3+: UTF-8 value
static void parse_entity_update(const struct os_mbuf* om) {
  uint16_t len = OS_MBUF_PKTLEN(om);
  if (len < 3) return;

  String all;
  all.reserve(len);
  for (const struct os_mbuf* m = om; m; m = SLIST_NEXT(m, om_next)) {
    all.concat((const char*)m->om_data, m->om_len);
  }

  uint8_t entity = (uint8_t)all[0];
  uint8_t attr   = (uint8_t)all[1];
  String value   = (len > 3) ? all.substring(3) : String();

  if (entity == ENTITY_TRACK) {
    if      (attr == TRACK_TITLE)    s_title    = value;
    else if (attr == TRACK_ARTIST)   s_artist   = value;
    else if (attr == TRACK_ALBUM)    s_album    = value;
    else if (attr == TRACK_DURATION) s_duration = value.toFloat();
    Serial.printf("[AMS] Track.%u = '%s'\n", attr, value.c_str());
  } else if (entity == ENTITY_PLAYER && attr == PLAYER_PLAYBACK_INFO) {
    // "PlaybackState,PlaybackRate,ElapsedTime", e.g. "1,1.000,15.229"
    int c1 = value.indexOf(',');
    int c2 = (c1 >= 0) ? value.indexOf(',', c1 + 1) : -1;
    if (c1 > 0) {
      s_playing = (value.substring(0, c1) == "1");
      if (c2 > c1) {
        s_rate    = value.substring(c1 + 1, c2).toFloat();
        s_elapsed = value.substring(c2 + 1).toFloat();
      }
      s_elapsed_ms = millis();
    }
    Serial.printf("[AMS] PlaybackInfo = '%s' (playing=%d rate=%.2f elapsed=%.1f)\n",
      value.c_str(), s_playing, s_rate, s_elapsed);
  } else if (entity == ENTITY_PLAYER && attr == PLAYER_VOLUME) {
    s_volume = value.toFloat();
    Serial.printf("[AMS] Volume = %.2f\n", s_volume);
  }
}

static int gap_event_cb(struct ble_gap_event* event, void* arg) {
  if (event->type != BLE_GAP_EVENT_NOTIFY_RX) return 0;
  if (event->notify_rx.conn_handle != ams.conn_handle) return 0;
  if (event->notify_rx.attr_handle != ams.hdl_entity_update) return 0;
  parse_entity_update(event->notify_rx.om);
  return 0;
}

void BLE_AMS_OnBonded(uint16_t conn_handle) {
  ams = AmsHandles{};
  ams.conn_handle = conn_handle;
  Serial.printf("[AMS] starting discovery on conn=%u\n", conn_handle);
  int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &UUID_AMS_SVC.u,
    svc_disc_cb, nullptr);
  if (rc != 0) Serial.printf("[AMS] disc_svc_by_uuid failed: 0x%04x\n", rc);
}

void BLE_AMS_OnDisconnect() {
  ams = AmsHandles{};
  s_title = "";
  s_artist = "";
  s_album = "";
  s_playing = false;
  s_rate = 0.0f;
  s_elapsed = 0.0f;
  s_duration = 0.0f;
  s_volume = -1.0f;
  if (listener_registered) {
    ble_gap_event_listener_unregister(&notify_listener);
    listener_registered = false;
  }
}

void BLE_AMS_SendCommand(AmsRemoteCmd cmd) {
  if (!ams.ready || ams.hdl_remote == 0) return;
  uint8_t b = (uint8_t)cmd;
  int rc = ble_gattc_write_flat(ams.conn_handle, ams.hdl_remote,
    &b, 1, nullptr, nullptr);
  Serial.printf("[AMS] send cmd %u rc=0x%04x\n", b, rc);
}

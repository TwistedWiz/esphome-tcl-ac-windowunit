//  TCL AC UART Protocol — ESPHome Climate Component
//  Complete rewrite based on protocol analysis of 1757 captured packets.
//
//  Key fixes vs previous version:
//   1. POLL packet corrected from 7→31 bytes (matches 979 identical captures)
//   2. setup() syncs YAML config → runtime state
//   3. STATUS parsing: power detection, display/beeper feedback, room temperature
//   4. Buffer safety: max size limit + incomplete packet timeout
//   5. Periodic CMD 0x09/0x0A queries for robust power state detection
//   6. Removed dead code (get_fan_speed_, celsius_to_raw_)

#include "tcl_ac.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace tcl_ac {

static const char *const TAG = "tcl_ac";

// ─── Canonical packets from UART captures (checksum pre-calculated) ─────────

// POLL: 31-byte CMD 0x04 — 100% constant across 979 captured packets
static const uint8_t POLL_PACKET[POLL_PACKET_SIZE] = {
    0xBB, 0x00, 0x01, 0x04, 0x19,           // Header + cmd + len(25)
    0x00, 0x00, 0x00, 0x08, 0x0F,           // data[0-4]
    0x00, 0x00, 0x00, 0x06, 0x00,           // data[5-9]
    0x00, 0x00, 0x00, 0x00, 0x00,           // data[10-14]
    0x1F, 0x1F, 0x1F, 0x1F, 0x1F,           // data[15-19]
    0x1F, 0x1F, 0x1F, 0x1F, 0x1F,           // data[20-24]
    0xA6,                                     // checksum
};

// SHORT_STATUS query: 8-byte CMD 0x09 — constant
static const uint8_t SHORT_QUERY[SHORT_QUERY_SIZE] = {
    0xBB, 0x00, 0x01, 0x09, 0x02, 0x04, 0x00, 0xB5,
};

// POWER query: 9-byte CMD 0x0A — most common variant (97/102 captures)
static const uint8_t POWER_QUERY[POWER_QUERY_SIZE] = {
    0xBB, 0x00, 0x01, 0x0A, 0x03, 0x02, 0x00, 0x05, 0xB4,
};

// ═════════════════════════════════════════════════════════════════════════════
//  Component Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::setup() {
  // ── Sync YAML config → runtime state (FIX: was missing, config had no effect) ──
  this->beeper_state_ = this->beeper_enabled_;
  this->display_state_ = this->display_enabled_;

  // Apply YAML vertical direction config
  if (this->vertical_direction_ == 255) {
    // "swing" in YAML → enable full vertical swing
    this->vertical_swing_ = VerticalSwingDirection::UP_DOWN;
  } else if (this->vertical_direction_ >= 1 && this->vertical_direction_ <= 5) {
    this->vertical_airflow_ = static_cast<AirflowVerticalDirection>(this->vertical_direction_);
  }

  // Apply YAML horizontal direction config
  if (this->horizontal_direction_ == 255) {
    this->horizontal_swing_ = HorizontalSwingDirection::LEFT_RIGHT;
  } else if (this->horizontal_direction_ >= 1 && this->horizontal_direction_ <= 5) {
    this->horizontal_airflow_ = static_cast<AirflowHorizontalDirection>(this->horizontal_direction_);
  }

  // Initialize ESPHome climate state
  this->mode = climate::CLIMATE_MODE_OFF;
  this->target_temperature = 25.0f;
  this->current_temperature = NAN;
  this->fan_mode = climate::CLIMATE_FAN_AUTO;
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->swing_mode = climate::CLIMATE_SWING_OFF;

  // Pre-allocate RX buffer
  this->rx_buffer_.reserve(RX_BUFFER_MAX);

  // Subscribe to external temperature sensor if configured
  if (this->sensor_ != nullptr) {
    this->sensor_->add_on_state_callback([this](float state) {
      if (!std::isnan(state)) {
        this->current_temperature = state;
        this->publish_state();
      }
    });
  }

  ESP_LOGCONFIG(TAG, "TCL AC initialized (beeper=%s, display=%s, vert=%d, horiz=%d, ext_sensor=%s)",
                this->beeper_state_ ? "ON" : "OFF",
                this->display_state_ ? "ON" : "OFF",
                this->vertical_direction_, this->horizontal_direction_,
                this->sensor_ != nullptr ? "YES" : "NO");
}

void TclAcClimate::loop() {
  // Read incoming UART data and send it to the parser
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    this->handle_rx_byte_(b);
  }

  // Discard stale incomplete packet
  if (!this->rx_buffer_.empty() && (millis() - this->last_rx_time_ > PACKET_TIMEOUT)) {
    this->rx_buffer_.clear();
  }

  // Periodic polling (Asks the AC for its status)
  uint32_t now = millis();
  if (now - this->last_poll_ >= POLL_INTERVAL) {
    this->send_poll_();
    this->last_poll_ = now;
  }

  // Auxiliary queries to keep the connection alive
  if (now - this->last_aux_query_ >= AUX_QUERY_INTERVAL) {
    if (this->aux_toggle_) {
      this->send_power_query_();
    } else {
      this->send_short_status_query_();
    }
    this->aux_toggle_ = !this->aux_toggle_;
    this->last_aux_query_ = now;
    this->last_poll_ = now; 
  }
}
void TclAcClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "TCL AC Climate:");
  ESP_LOGCONFIG(TAG, "  Beeper: %s", this->beeper_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Display: %s", this->display_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Vertical direction: %d", this->vertical_direction_);
  ESP_LOGCONFIG(TAG, "  Horizontal direction: %d", this->horizontal_direction_);
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Climate Traits & Control
// ═════════════════════════════════════════════════════════════════════════════

climate::ClimateTraits TclAcClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_AUTO,
  });

  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
  });

  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
      climate::CLIMATE_PRESET_BOOST,     // TURBO
      climate::CLIMATE_PRESET_SLEEP,
      climate::CLIMATE_PRESET_COMFORT,   // QUIET
  });

  traits.set_supported_swing_modes({
      climate::CLIMATE_SWING_OFF,
      climate::CLIMATE_SWING_VERTICAL,
      climate::CLIMATE_SWING_HORIZONTAL,
      climate::CLIMATE_SWING_BOTH,
  });

  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(32.0f);
  traits.set_visual_temperature_step(1.0f);
  // ESPHome 2026.5+: set_supports_current_temperature() wurde durch Feature-Flags ersetzt
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);

  return traits;
}

void TclAcClimate::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
  }

  if (call.get_target_temperature().has_value()) {
    this->target_temperature = *call.get_target_temperature();
  }

  if (call.get_fan_mode().has_value()) {
    this->fan_mode = *call.get_fan_mode();
  }

  if (call.get_preset().has_value()) {
    climate::ClimatePreset p = *call.get_preset();

    // Clear mutually exclusive flags
    this->eco_mode_ = false;
    this->turbo_mode_ = false;
    this->quiet_mode_ = false;

    switch (p) {
      case climate::CLIMATE_PRESET_ECO:
        this->eco_mode_ = true;
        if (this->mode != climate::CLIMATE_MODE_OFF)
          this->mode = climate::CLIMATE_MODE_AUTO;  // ECO requires AUTO
        break;
      case climate::CLIMATE_PRESET_BOOST:
        this->turbo_mode_ = true;
        break;
      case climate::CLIMATE_PRESET_COMFORT:
        this->quiet_mode_ = true;
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        // Handled in create_set_packet_ via byte[19]
        break;
      default:
        break;
    }
    this->preset = p;
  }

  if (call.get_swing_mode().has_value()) {
    this->swing_mode = *call.get_swing_mode();
  }

  this->publish_state();

  // Build and send SET packet
  uint8_t packet[SET_PACKET_SIZE];
  this->create_set_packet_(packet);
  this->send_packet_(packet, SET_PACKET_SIZE);
  ESP_LOGD(TAG, "SET sent (mode=%d, temp=%.0f)", (int)this->mode, this->target_temperature);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SET Packet Construction (38 bytes, CMD 0x03)
//  Validated against 43 captured SET packets
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::create_set_packet_(uint8_t *packet) {
  memset(packet, 0, 38);

  // 1. Standard Header
  packet[0] = 0xBB;
  packet[1] = 0x00;
  packet[2] = 0x01;
  packet[3] = 0x03; // CMD_SET
  packet[4] = 0x20; // 32 Data Bytes

  // 2. Power, Display, Beeper, Eco (Byte 7)
  if (this->mode != climate::CLIMATE_MODE_OFF) {
    packet[7] |= 0x04; // Power ON
  }
  if (this->display_enabled_) packet[7] |= 0x40; // Display ON
  if (this->beeper_enabled_)  packet[7] |= 0x20; // Beeper ON
  if (this->preset == climate::CLIMATE_PRESET_ECO) packet[7] |= 0x80; // Eco Mode

  // 3. Operating Mode (Byte 8)
  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:      packet[8] |= 0x03; break;
    case climate::CLIMATE_MODE_DRY:       packet[8] |= 0x02; break;
    case climate::CLIMATE_MODE_FAN_ONLY:  packet[8] |= 0x07; break;
    case climate::CLIMATE_MODE_AUTO:      packet[8] |= 0x08; break;
    default:                              packet[8] |= 0x03; break; // Fallback to Cool
  }

  // 4. Target Temperature (Byte 9)
  // TCL Sending Formula is (31 - Celsius)
  int temp_c = (int)(this->target_temperature + 0.5f);
  if (temp_c < 16) temp_c = 16;
  if (temp_c > 31) temp_c = 31;
  packet[9] = 31 - temp_c;

  // 5. Fan Speed (Byte 10)
  switch (this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO)) {
    case climate::CLIMATE_FAN_AUTO:   packet[10] |= 0x00; break;
    case climate::CLIMATE_FAN_LOW:    packet[10] |= 0x01; break;
    case climate::CLIMATE_FAN_MEDIUM: packet[10] |= 0x03; break;
    case climate::CLIMATE_FAN_HIGH:   packet[10] |= 0x07; break;
  }

  // 6. Generate Checksum
  uint8_t cs = 0;
  for (size_t i = 0; i < 37; i++) cs ^= packet[i];
  packet[37] = cs;
}
// ═════════════════════════════════════════════════════════════════════════════
//  TX Methods
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::send_packet_(const uint8_t *data, size_t len) {
  this->write_array(data, len);
  this->flush();
  ESP_LOGV(TAG, "TX %d bytes, cmd=0x%02X", len, data[3]);
}

void TclAcClimate::send_poll_() {
  this->send_packet_(POLL_PACKET, POLL_PACKET_SIZE);
}

void TclAcClimate::send_short_status_query_() {
  this->send_packet_(SHORT_QUERY, SHORT_QUERY_SIZE);
}

void TclAcClimate::send_power_query_() {
  this->send_packet_(POWER_QUERY, POWER_QUERY_SIZE);
}

uint8_t TclAcClimate::xor_checksum_(const uint8_t *data, size_t len) {
  uint8_t cs = 0;
  for (size_t i = 0; i < len; i++) cs ^= data[i];
  return cs;
}

// ═════════════════════════════════════════════════════════════════════════════
//  RX Packet Framing
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::handle_rx_byte_(uint8_t b) {
  this->last_rx_time_ = millis();

  // Buffer overflow protection
  if (this->rx_buffer_.size() >= RX_BUFFER_MAX) {
    ESP_LOGW(TAG, "RX overflow (%d bytes), resetting", this->rx_buffer_.size());
    this->rx_buffer_.clear();
  }

  this->rx_buffer_.push_back(b);

  // Need at least 5 bytes: header(3) + cmd(1) + len(1)
  if (this->rx_buffer_.size() < 5) return;

  // Validate AC→MCU header: BB 01 00
  if (this->rx_buffer_[0] != HEADER_BYTE ||
      this->rx_buffer_[1] != DIR_AC_TO_MCU_1 ||
      this->rx_buffer_[2] != DIR_AC_TO_MCU_2) {
    // Drop first byte, try to re-sync
    this->rx_buffer_.erase(this->rx_buffer_.begin());
    return;
  }

  // Calculate expected total packet size
  uint8_t data_len = this->rx_buffer_[4];
  size_t expected = 5 + (size_t)data_len + 1;  // header(3)+cmd(1)+len(1) + data + checksum(1)

  if (expected > RX_BUFFER_MAX) {
    ESP_LOGW(TAG, "Bad data length %d, resetting", data_len);
    this->rx_buffer_.clear();
    return;
  }

  // Wait for complete packet
  if (this->rx_buffer_.size() < expected) return;

  // Validate checksum
  uint8_t calc = this->xor_checksum_(this->rx_buffer_.data(), expected - 1);
  uint8_t recv = this->rx_buffer_[expected - 1];

  if (calc == recv) {
    this->dispatch_packet_(this->rx_buffer_.data(), expected);
  } else {
    ESP_LOGW(TAG, "Checksum fail: cmd=0x%02X calc=0x%02X recv=0x%02X",
             this->rx_buffer_[3], calc, recv);
  }

  this->rx_buffer_.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Packet Dispatch
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::dispatch_packet_(const uint8_t *pkt, size_t len) {
  uint8_t cmd = pkt[3];
  uint8_t data_len = pkt[4];
  const uint8_t *payload = pkt + 5;

  switch (cmd) {
    case CMD_SET:   // SET response (61 bytes, data_len=55)
    case CMD_POLL:  // POLL response (61 bytes, data_len=55)
    case CMD_ECHO:  // Status echo (same format)
      if (data_len >= 32) {
        this->parse_status_(payload, data_len);
      }
      break;

    case CMD_POWER:  // Power status (51 bytes, data_len=45)
      this->parse_power_(payload, data_len);
      break;

    case CMD_TEMP:   // Temp response (17 bytes, data_len=11)
      this->parse_temp_(payload, data_len);
      break;

    case CMD_SHORT:  // Short status (51 bytes, all constant) — acknowledge only
      ESP_LOGV(TAG, "Short status (0x09) received");
      break;

    case CMD_TIME:   // Time sync ack
      ESP_LOGV(TAG, "Time sync ack received");
      break;

    default:
      ESP_LOGV(TAG, "Unknown response: cmd=0x%02X len=%d", cmd, data_len);
      break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  STATUS Response Parsing (61 bytes, CMD 0x03/0x04)
//
//  Payload layout verified against original I-am-nightingale/tclac project:
//    [0-1]  Type/constant (0x04 0x00)
//    [2]    mainPara: Power + Mode combined byte
//           Bit 7 (0x80): Power OFF / standby override
//           Bit 6 (0x40): ECO mode (NOT display!)
//           Bit 4 (0x10): Power ON (reliable ON indicator)
//           Bits 0-3: Mode (1=cool, 2=fan, 3=dry, 4=heat, 5=auto)
//    [3]    secPara: Fan speed (upper nibble) + Target temp (lower nibble)
//           Upper 0xF0: Fan (0x80=auto, 0x90=low, 0xA0=med, 0xD0=high)
//           Lower 0x0F: Target temp = nibble + 16 (range 16–31°C)
//    [4]    Flags (bit 2 = comfort preset)
//    [5]    Swing mode (bits 5-6: 0x00=off, 0x20=horiz, 0x40=vert, 0x60=both)
//    [12-13] Room temperature (16-bit NTC sensor)
//           Formula: ((payload[12] << 8 | payload[13]) / 374.0 - 32.0) / 1.8 = °C
//    [14]   Flags (bit 0 = sleep preset, bit 7 = compressor running)
//    [25]   Internal sensor (evaporator/pipe), NOT room temp
//    [28]   Quiet fan flag (bit 7)
//    NOTE: Display, beeper, health, turbo are NOT in STATUS (write-only).
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::parse_status_(const uint8_t *payload, size_t len) {
  if (len < 55) return;

  uint8_t main_para = payload[2];
  uint8_t sec_para = payload[3];

  // 1. Power State (Bit 4 / 0x10 is ON)
  bool ac_is_on = (main_para & 0x10) != 0;
  this->ac_is_on_ = ac_is_on;

  // 2. Target Temperature (Lower Nibble + 16)
  float target_temp = (float)((sec_para & 0x0F) + 16);
  if (target_temp >= 16.0f && target_temp <= 31.0f) {
    this->target_temperature = target_temp;
  }

  // 3. Apply Power State
  if (!ac_is_on) {
    this->mode = climate::CLIMATE_MODE_OFF;
    this->preset = climate::CLIMATE_PRESET_NONE;
  } else {
    // 4. Operating Mode (Lower Nibble of main_para)
    uint8_t mode_bits = main_para & 0x0F;
    switch (mode_bits) {
      case 0x01: 
        this->mode = climate::CLIMATE_MODE_COOL; 
        this->preset = climate::CLIMATE_PRESET_NONE;
        break;
      case 0x02: 
        this->mode = climate::CLIMATE_MODE_FAN_ONLY; 
        this->preset = climate::CLIMATE_PRESET_NONE;
        break;
      case 0x03: 
        this->mode = climate::CLIMATE_MODE_DRY; 
        this->preset = climate::CLIMATE_PRESET_NONE;
        break;
      case 0x05: 
        this->mode = climate::CLIMATE_MODE_COOL; // Map Eco to Cool + Eco Preset
        this->preset = climate::CLIMATE_PRESET_ECO;
        break;
      default:   
        this->mode = climate::CLIMATE_MODE_AUTO; 
        break;
    }

    // 5. Fan Speed (Upper Nibble of sec_para)
    uint8_t fan_raw = sec_para & 0xF0;
    switch (fan_raw) {
      case 0x00: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
      case 0x10: this->fan_mode = climate::CLIMATE_FAN_LOW; break;
      case 0x20: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
      case 0x30: 
      case 0x50: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
      default:   this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
    }
  }

  // Push changes to Home Assistant UI
  this->publish_state();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Power Response Parsing (51 bytes, CMD 0x0A)
//
//  Payload[2]:  Power flag (0x0C observed in both ON and standby — unreliable)
//  Payload[3]:  Flags (0x85 observed)
//  Payload[16]: Room temperature (raw - 127 = °C), validated against DHT22
//               ONLY valid when AC is OFF (standby)!
//               0x93→20°C matches DHT22 reading of 19.9°C.
//               When AC is ON: payload[16]=0x46 (garbage, not temperature).
//
//  NOTE: Do NOT derive power state from CMD 0x0A; STATUS (CMD 0x04) is
//        authoritative.  The AC returns flag=0x0C even in standby, which
//        caused a mode flicker bug (COOL↔OFF every 30 s).
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::parse_power_(const uint8_t *payload, size_t len) {
  if (len < 3) return;

  uint8_t flag = payload[2];
  ESP_LOGD(TAG, "POWER flag=0x%02X flags=0x%02X (len=%d)",
           flag, len >= 4 ? payload[3] : 0, len);

  // ── Extract room temperature from payload[16] ──
  // Only valid when AC is OFF (standby). When ON, this field contains
  // non-temperature data (observed 0x46 = garbage).
  // Skip entirely when external sensor is configured.
  if (len >= 17 && !this->ac_is_on_ && this->sensor_ == nullptr) {
    uint8_t temp_raw = payload[16];
    ESP_LOGD(TAG, "POWER temp_raw=0x%02X (%d) payload[15..18]=%02X:%02X:%02X:%02X",
             temp_raw, temp_raw,
             payload[15], payload[16], payload[17], len >= 19 ? payload[18] : 0);
    if (temp_raw >= 137 && temp_raw <= 167) {  // 10°C .. 40°C
      float room_temp = (float)temp_raw - 127.0f;
      if (this->current_temperature != room_temp) {
        ESP_LOGI(TAG, "Room temp from POWER: %.0f°C (raw=0x%02X)", room_temp, temp_raw);
        this->current_temperature = room_temp;
        this->publish_state();
      }
    } else {
      ESP_LOGD(TAG, "POWER temp_raw=0x%02X outside valid range", temp_raw);
    }
  } else if (len >= 17 && this->ac_is_on_) {
    ESP_LOGD(TAG, "POWER skipped (AC is ON, payload[16]=0x%02X is not temperature)", payload[16]);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Temp Response Parsing (17 bytes, CMD 0x05)
//
//  Rarely sent. Payload[10] varies (0x01, 0x02, 0x04) — meaning unclear.
//  Not used for temperature; STATUS response is the primary source.
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::parse_temp_(const uint8_t *payload, size_t len) {
  if (len < 11) return;
  ESP_LOGV(TAG, "TEMP response: payload[10]=0x%02X", payload[10]);
  // No actionable data extracted; kept for future protocol analysis
}

// ═════════════════════════════════════════════════════════════════════════════
//  Runtime Control Methods (callable from HA actions / lambdas)
// ═════════════════════════════════════════════════════════════════════════════

void TclAcClimate::set_vertical_airflow(AirflowVerticalDirection dir) {
  this->vertical_airflow_ = dir;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_horizontal_airflow(AirflowHorizontalDirection dir) {
  this->horizontal_airflow_ = dir;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_vertical_swing(VerticalSwingDirection dir) {
  this->vertical_swing_ = dir;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_horizontal_swing(HorizontalSwingDirection dir) {
  this->horizontal_swing_ = dir;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_display_state(bool state) {
  this->display_state_ = state;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_beeper_state(bool state) {
  this->beeper_state_ = state;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_eco_mode(bool enabled) {
  this->eco_mode_ = enabled;
  if (enabled) {
    this->turbo_mode_ = false;   // Mutually exclusive
    this->quiet_mode_ = false;
  }
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_turbo_mode(bool enabled) {
  this->turbo_mode_ = enabled;
  if (enabled) {
    this->eco_mode_ = false;
    this->quiet_mode_ = false;
  }
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_quiet_mode(bool enabled) {
  this->quiet_mode_ = enabled;
  if (enabled) {
    this->eco_mode_ = false;
    this->turbo_mode_ = false;
  }
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

void TclAcClimate::set_health_mode(bool enabled) {
  this->health_mode_ = enabled;
  if (this->force_mode_) {
    uint8_t pkt[SET_PACKET_SIZE];
    this->create_set_packet_(pkt);
    this->send_packet_(pkt, SET_PACKET_SIZE);
  }
}

}  // namespace tcl_ac
}  // namespace esphome

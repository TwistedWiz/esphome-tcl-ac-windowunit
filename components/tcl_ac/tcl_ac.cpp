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
  // ── Read incoming UART data ──
  while (this->available()) {
    uint8_t b;
    this->read_byte(&b);
    this->handle_rx_byte_(b);
  }

  // ── Discard stale incomplete packet ──
  if (!this->rx_buffer_.empty() && (millis() - this->last_rx_time_ > PACKET_TIMEOUT)) {
    ESP_LOGV(TAG, "RX timeout, discarding %d bytes", this->rx_buffer_.size());
    this->rx_buffer_.clear();
  }

  // ── Periodic polling ──
  uint32_t now = millis();

  if (now - this->last_poll_ >= POLL_INTERVAL) {
    this->send_poll_();
    this->last_poll_ = now;
  }

  // Auxiliary queries: alternate between CMD 0x09 and 0x0A, one per cycle.
  // Original dongle sent these individually between POLLs, NOT batched.
  if (now - this->last_aux_query_ >= AUX_QUERY_INTERVAL) {
    if (this->aux_toggle_) {
      this->send_power_query_();
    } else {
      this->send_short_status_query_();
    }
    this->aux_toggle_ = !this->aux_toggle_;
    this->last_aux_query_ = now;
    this->last_poll_ = now;  // Skip next POLL to give AC time to respond
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
  traits.set_supports_current_temperature(true);

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
  memset(packet, 0, SET_PACKET_SIZE);

  // Header [0-2]
  packet[0] = HEADER_BYTE;
  packet[1] = DIR_MCU_TO_AC_1;
  packet[2] = DIR_MCU_TO_AC_2;

  // Command + data length [3-4]
  packet[3] = CMD_SET;
  packet[4] = 0x20;  // 32 data bytes

  // Constants [5-6, 13, 29] — must match captures exactly
  packet[5]  = 0x03;
  packet[6]  = 0x01;
  packet[13] = 0x01;  // Required constant — AC rejects packet without this
  packet[29] = 0x20;  // Required constant

  // ── Byte[7]: Power / Display / Beeper / ECO ──
  if (this->eco_mode_)      packet[7] |= SET_ECO;
  if (this->display_state_) packet[7] |= SET_DISPLAY;
  if (this->beeper_state_)  packet[7] |= SET_BEEPER;

  // ── Byte[8]: Operating mode + special flags ──
  switch (this->mode) {
    case climate::CLIMATE_MODE_OFF:
      // No SET_POWER bit → AC interprets as OFF
      break;
    case climate::CLIMATE_MODE_HEAT:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_HEAT;
      break;
    case climate::CLIMATE_MODE_DRY:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_DRY;
      break;
    case climate::CLIMATE_MODE_COOL:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_COOL;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_FAN_ONLY;
      break;
    case climate::CLIMATE_MODE_AUTO:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_AUTO;
      break;
    default:
      packet[7] |= SET_POWER;
      packet[8] |= MODE_COOL;  // Safe default
      break;
  }

  // Special mode flags (byte[8] upper bits)
  if (this->quiet_mode_) packet[8] |= SET_QUIET;
  if (this->turbo_mode_) packet[8] |= SET_TURBO;
  if (this->health_mode_) packet[8] |= SET_HEALTH;

  // ── Byte[9]: Target temperature (formula: 111 - celsius) ──
  // Validated: SET byte[9] = 0x56 for 25°C (111-25=86=0x56) ✓
  int raw_temp = 111 - (int)(this->target_temperature + 0.5f);
  if (raw_temp < 79) raw_temp = 79;    // min 32°C
  if (raw_temp > 95) raw_temp = 95;    // max 16°C
  packet[9] = (uint8_t)raw_temp;

  // ── Byte[10]: Fan speed (bits 0-2) + vertical swing enable (bits 3-5) ──
  switch (this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO)) {
    case climate::CLIMATE_FAN_AUTO:   break;  // 0x00
    case climate::CLIMATE_FAN_LOW:    packet[10] |= FAN_LOW;  break;
    case climate::CLIMATE_FAN_MEDIUM: packet[10] |= FAN_MED;  break;
    case climate::CLIMATE_FAN_HIGH:   packet[10] |= FAN_MAX;  break;
    default:                          break;
  }

  // ESPHome swing modes → byte[10] and byte[11] enable flags
  switch (this->swing_mode) {
    case climate::CLIMATE_SWING_VERTICAL:
      packet[10] |= VERT_SWING_ENABLE;
      break;
    case climate::CLIMATE_SWING_HORIZONTAL:
      packet[11] |= HORIZ_SWING_ENABLE;
      break;
    case climate::CLIMATE_SWING_BOTH:
      packet[10] |= VERT_SWING_ENABLE;
      packet[11] |= HORIZ_SWING_ENABLE;
      break;
    default:
      break;
  }

  // ── Byte[19]: Sleep mode ──
  if (this->preset.value_or(climate::CLIMATE_PRESET_NONE) == climate::CLIMATE_PRESET_SLEEP) {
    packet[19] = 0x01;
  }

  // ── Presets applied to packet (in case control() already set flags) ──
  switch (this->preset.value_or(climate::CLIMATE_PRESET_NONE)) {
    case climate::CLIMATE_PRESET_ECO:
      packet[7] |= SET_ECO;
      break;
    case climate::CLIMATE_PRESET_COMFORT:
      packet[8] |= SET_COMFORT;
      break;
    default:
      break;
  }

  // ── Byte[32]: Vertical swing direction (bits 3-4) + airflow position (bits 0-2) ──
  switch (this->vertical_swing_) {
    case VerticalSwingDirection::OFF:      break;
    case VerticalSwingDirection::UP_DOWN:  packet[32] |= 0b00001000; break;
    case VerticalSwingDirection::UPSIDE:   packet[32] |= 0b00010000; break;
    case VerticalSwingDirection::DOWNSIDE: packet[32] |= 0b00011000; break;
  }
  switch (this->vertical_airflow_) {
    case AirflowVerticalDirection::LAST:     break;
    case AirflowVerticalDirection::MAX_UP:   packet[32] |= 0b00000001; break;
    case AirflowVerticalDirection::UP:       packet[32] |= 0b00000010; break;
    case AirflowVerticalDirection::CENTER:   packet[32] |= 0b00000011; break;
    case AirflowVerticalDirection::DOWN:     packet[32] |= 0b00000100; break;
    case AirflowVerticalDirection::MAX_DOWN: packet[32] |= 0b00000101; break;
  }

  // ── Byte[33]: Horizontal swing direction (bits 3-5) + airflow position (bits 0-2) ──
  switch (this->horizontal_swing_) {
    case HorizontalSwingDirection::OFF:        break;
    case HorizontalSwingDirection::LEFT_RIGHT: packet[33] |= 0b00001000; break;
    case HorizontalSwingDirection::LEFTSIDE:   packet[33] |= 0b00010000; break;
    case HorizontalSwingDirection::CENTER:     packet[33] |= 0b00011000; break;
    case HorizontalSwingDirection::RIGHTSIDE:  packet[33] |= 0b00100000; break;
  }
  switch (this->horizontal_airflow_) {
    case AirflowHorizontalDirection::LAST:      break;
    case AirflowHorizontalDirection::MAX_LEFT:  packet[33] |= 0b00000001; break;
    case AirflowHorizontalDirection::LEFT:      packet[33] |= 0b00000010; break;
    case AirflowHorizontalDirection::CENTER:    packet[33] |= 0b00000011; break;
    case AirflowHorizontalDirection::RIGHT:     packet[33] |= 0b00000100; break;
    case AirflowHorizontalDirection::MAX_RIGHT: packet[33] |= 0b00000101; break;
  }

  // ── Checksum ──
  packet[SET_PACKET_SIZE - 1] = this->xor_checksum_(packet, SET_PACKET_SIZE - 1);
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
  if (len < 55) {
    ESP_LOGW(TAG, "Status too short: %d bytes", len);
    return;
  }

  // ── mainPara at payload[2] — Power + Mode combined byte ──
  // Bit 7 (0x80) = power OFF / standby override
  // Bit 6 (0x40) = ECO mode (NOT display! Original: dataRX[7] & (1<<6))
  // Bit 4 (0x10) = reliable ON indicator
  // Bits 0-3 = mode (1=cool, 2=fan, 3=dry, 4=heat, 5=auto)
  // NOTE: Display/beeper NOT in STATUS responses (write-only in SET byte[7]).
  uint8_t main_para = payload[2];
  bool pwr_on_bit  = (main_para & STA_POWER_ON)  != 0;  // bit 4 (0x10)
  bool pwr_off_bit = (main_para & STA_POWER_OFF) != 0;  // bit 7 (0x80)
  bool ac_is_on = pwr_on_bit && !pwr_off_bit;
  this->ac_is_on_ = ac_is_on;

  // ── secPara at payload[3] — Fan speed (upper nibble) + Target temp (lower nibble) ──
  uint8_t sec_para = payload[3];

  // ── Target temperature: always read (AC remembers setting even when OFF) ──
  // Original: target_temperature = (dataRX[FAN_SPEED_POS] & SET_TEMP_MASK) + 16
  float target_temp = (float)((sec_para & STA_TEMP_MASK) + 16);
  if (target_temp >= 16.0f && target_temp <= 31.0f) {
    this->target_temperature = target_temp;
  }

  ESP_LOGD(TAG, "STATUS main=0x%02X sec=0x%02X (on=%d) target=%.0f fan=0x%02X swing=0x%02X",
           main_para, sec_para, ac_is_on, target_temp,
           sec_para & STA_FAN_MASK, payload[5] & STA_SWING_MASK);

  // ── When OFF: clear active modes ──
  if (!ac_is_on) {
    if (this->mode != climate::CLIMATE_MODE_OFF) {
      ESP_LOGI(TAG, "AC reported OFF (mainPara=0x%02X)", main_para);
      this->mode = climate::CLIMATE_MODE_OFF;
    }
    this->swing_mode = climate::CLIMATE_SWING_OFF;
    this->preset = climate::CLIMATE_PRESET_NONE;
    this->eco_mode_ = false;
    this->quiet_mode_ = false;
  } else {
    // ── Operating Mode (mainPara lower nibble) ──
    uint8_t mode_bits = main_para & STA_MODE_MASK;
    climate::ClimateMode new_mode;
    switch (mode_bits) {
      case STA_MODE_COOL:     new_mode = climate::CLIMATE_MODE_COOL; break;
      case STA_MODE_HEAT:     new_mode = climate::CLIMATE_MODE_HEAT; break;
      case STA_MODE_DRY:      new_mode = climate::CLIMATE_MODE_DRY; break;
      case STA_MODE_FAN_ONLY: new_mode = climate::CLIMATE_MODE_FAN_ONLY; break;
      case STA_MODE_AUTO:     new_mode = climate::CLIMATE_MODE_AUTO; break;
      default:
        ESP_LOGW(TAG, "Unknown mode bits: 0x%02X in mainPara=0x%02X", mode_bits, main_para);
        new_mode = climate::CLIMATE_MODE_AUTO;
        break;
    }
    if (this->mode != new_mode) {
      ESP_LOGI(TAG, "Mode changed: %d -> %d (mainPara=0x%02X)", this->mode, new_mode, main_para);
    }
    this->mode = new_mode;

    // ── Fan Speed (secPara upper nibble) ──
    // Quiet fan override: payload[28] bit 7 (original: dataRX[33] & FAN_QUIET)
    if (len >= 29 && (payload[28] & 0x80)) {
      this->fan_mode = climate::CLIMATE_FAN_LOW;  // Quiet → map to LOW
      this->quiet_mode_ = true;
    } else {
      this->quiet_mode_ = false;
      uint8_t fan_raw = sec_para & STA_FAN_MASK;
      switch (fan_raw) {
        case STA_FAN_AUTO:   this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
        case STA_FAN_LOW:    this->fan_mode = climate::CLIMATE_FAN_LOW; break;
        case STA_FAN_MEDIUM: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
        case STA_FAN_MIDDLE: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
        case STA_FAN_HIGH:   this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
        case STA_FAN_FOCUS:  this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
        default:
          ESP_LOGW(TAG, "Unknown fan: 0x%02X in sec=0x%02X", fan_raw, sec_para);
          this->fan_mode = climate::CLIMATE_FAN_AUTO;
          break;
      }
    }

    // ── Swing Mode (payload[5] bits 5-6) ──
    // Original: SWING_MODE_MASK(0x60) on dataRX[10] = our payload[5]
    uint8_t swing_raw = payload[5] & STA_SWING_MASK;
    switch (swing_raw) {
      case STA_SWING_OFF:   this->swing_mode = climate::CLIMATE_SWING_OFF; break;
      case STA_SWING_VERT:  this->swing_mode = climate::CLIMATE_SWING_VERTICAL; break;
      case STA_SWING_HORIZ: this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL; break;
      case STA_SWING_BOTH:  this->swing_mode = climate::CLIMATE_SWING_BOTH; break;
    }

    // ── Presets: ECO / Comfort / Sleep ──
    // ECO: mainPara bit 6 (original: dataRX[7] & (1<<6))
    // Comfort: payload[4] bit 2 (original: dataRX[9] & (1<<2))
    // Sleep: payload[14] bit 0 (original: dataRX[19] & (1<<0))
    // NOTE: Turbo (BOOST) and Health are write-only — not in STATUS.
    bool eco = (main_para & 0x40) != 0;
    bool comfort = (payload[4] & 0x04) != 0;
    bool sleep_on = (len >= 15) && ((payload[14] & 0x01) != 0);
    this->eco_mode_ = eco;
    if (eco) {
      this->preset = climate::CLIMATE_PRESET_ECO;
    } else if (comfort) {
      this->preset = climate::CLIMATE_PRESET_COMFORT;
    } else if (sleep_on) {
      this->preset = climate::CLIMATE_PRESET_SLEEP;
    } else {
      this->preset = climate::CLIMATE_PRESET_NONE;
    }
  }

  // ── Room temperature: payload[12:13] with 16-bit NTC formula ──
  if (len >= 14) {
    uint16_t temp_raw_16 = ((uint16_t)payload[12] << 8) | payload[13];
    float room_temp = ((float)temp_raw_16 / 374.0f - 32.0f) / 1.8f;

    ESP_LOGD(TAG, "Internal NTC: raw=0x%04X (%.1f°C) payload[12:13]=%02X:%02X",
             temp_raw_16, room_temp, payload[12], payload[13]);

    if (this->sensor_ == nullptr) {
      if (room_temp >= 0.0f && room_temp <= 50.0f) {
        if (this->current_temperature != room_temp || std::isnan(this->current_temperature)) {
          this->current_temperature = room_temp;
        }
      } else {
        ESP_LOGV(TAG, "NTC temp out of range: %.1f°C (raw=0x%04X)", room_temp, temp_raw_16);
      }
    }
  }

  // Log internal pipe/evaporator sensor at payload[25] for reference only
  if (ac_is_on) {
    ESP_LOGV(TAG, "Pipe sensor raw=0x%02X payload[25]", payload[25]);
  }

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

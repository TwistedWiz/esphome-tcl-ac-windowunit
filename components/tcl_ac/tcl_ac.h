#pragma once

//  TCL AC UART Protocol — ESPHome Climate Component
//  Protocol reverse-engineered from original RTL8710C WiFi module captures (Oct 2025).
//  Communication: 9600 baud, 8E1 (8 data bits, EVEN parity, 1 stop bit).
//
//  Packet structure:
//    [0]     0xBB header
//    [1-2]   Direction (00 01 = MCU→AC, 01 00 = AC→MCU)
//    [3]     Command ID
//    [4]     Data length N
//    [5..4+N] Data payload
//    [5+N]   XOR checksum of bytes [0..4+N]
//
//  Validated against 1757 captured packets (1253 MCU→AC, 504 AC→MCU).

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace tcl_ac {

// ─── Packet Header ──────────────────────────────────────────────────────────
static const uint8_t HEADER_BYTE = 0xBB;
static const uint8_t DIR_MCU_TO_AC_1 = 0x00;
static const uint8_t DIR_MCU_TO_AC_2 = 0x01;
static const uint8_t DIR_AC_TO_MCU_1 = 0x01;
static const uint8_t DIR_AC_TO_MCU_2 = 0x00;

// ─── Command IDs ────────────────────────────────────────────────────────────
static const uint8_t CMD_SET   = 0x03;  // MCU→AC set params / AC→MCU set response (61 bytes)
static const uint8_t CMD_POLL  = 0x04;  // MCU→AC poll (31 bytes) / AC→MCU poll response (61 bytes)
static const uint8_t CMD_TEMP  = 0x05;  // AC→MCU temperature response (17 bytes)
static const uint8_t CMD_ECHO  = 0x06;  // AC→MCU status echo
static const uint8_t CMD_SHORT = 0x09;  // MCU→AC query (8 bytes) / AC→MCU short status (51 bytes, all constant)
static const uint8_t CMD_POWER = 0x0A;  // MCU→AC query (9 bytes) / AC→MCU power status (51 bytes)
static const uint8_t CMD_TIME  = 0x0B;  // MCU→AC time sync (22 bytes) / AC→MCU ack (8 bytes)

// ─── Packet Sizes ───────────────────────────────────────────────────────────
static const size_t SET_PACKET_SIZE    = 38;   // MCU→AC SET command
static const size_t POLL_PACKET_SIZE   = 31;   // MCU→AC POLL (was 7, fixed to match captures)
static const size_t SHORT_QUERY_SIZE   = 8;    // MCU→AC CMD 0x09 short status query
static const size_t POWER_QUERY_SIZE   = 9;    // MCU→AC CMD 0x0A power query

// ─── Safety Limits ──────────────────────────────────────────────────────────
static const size_t   RX_BUFFER_MAX    = 128;  // Largest AC response is 61 bytes
static const uint32_t PACKET_TIMEOUT   = 500;  // ms — discard stale incomplete packet
static const uint32_t POLL_INTERVAL    = 5000; // ms — poll AC for status
static const uint32_t AUX_QUERY_INTERVAL = 30000; // ms — short status + power query

// ─── SET Byte[7]: Power / Display / Beeper / ECO ────────────────────────────
//   Bit 7 (0x80): ECO mode
//   Bit 6 (0x40): Display ON
//   Bit 5 (0x20): Beeper ON   (default ON — 98% of captured SET packets)
//   Bit 2 (0x04): POWER ON    (absence = OFF)
static const uint8_t SET_ECO     = 0x80;
static const uint8_t SET_DISPLAY = 0x40;
static const uint8_t SET_BEEPER  = 0x20;
static const uint8_t SET_POWER   = 0x04;

// ─── SET Byte[8]: Operating Mode (bits 0-3) + Special Flags (bits 4-7) ──────
//   Bit 7 (0x80): Quiet mode
//   Bit 6 (0x40): Turbo mode
//   Bit 5 (0x20): Health mode
//   Bit 4 (0x10): Comfort mode
//   Bits 0-3: Mode (HEAT=1, DRY=2, COOL=3, FAN=7, AUTO=8)
static const uint8_t SET_QUIET   = 0x80;
static const uint8_t SET_TURBO   = 0x40;
static const uint8_t SET_HEALTH  = 0x20;
static const uint8_t SET_COMFORT = 0x10;
static const uint8_t MODE_HEAT     = 0x01;
static const uint8_t MODE_DRY      = 0x02;
static const uint8_t MODE_COOL     = 0x03;
static const uint8_t MODE_FAN_ONLY = 0x07;
static const uint8_t MODE_AUTO     = 0x08;

// ─── SET Byte[10]: Fan Speed (bits 0-2) + Vertical Swing Enable (bits 3-5) ──
static const uint8_t FAN_AUTO   = 0x00;
static const uint8_t FAN_LOW    = 0x01;
static const uint8_t FAN_MED    = 0x03;
static const uint8_t FAN_HIGH   = 0x05;
static const uint8_t FAN_MAX    = 0x07;
static const uint8_t VERT_SWING_ENABLE = 0x38;  // bits 3-5 all set = enable vertical swing

// ─── SET Byte[11]: Horizontal Swing Enable ───────────────────────────────────
static const uint8_t HORIZ_SWING_ENABLE = 0x08;

// ─── STATUS Response Byte[2] (mainPara) — Power + Mode (differs from SET!) ──
//   Bit 7 (0x80): Power OFF / standby override
//   Bit 6 (0x40): ECO mode (NOT display! Original: dataRX[7] & (1<<6) = ECO)
//   Bits 5-4 (0x30): Always set when running ("ON pattern")
//   Bit 4 (0x10): Power ON / mode active (THE reliable ON indicator)
//   Bits 3-0: Operating mode (1=cool, 2=fan, 3=dry, 4=heat, 5=auto)
//   NOTE: Display and beeper are NOT in STATUS responses (write-only in SET).
//         Health and turbo are also write-only.
static const uint8_t STA_POWER_OFF = 0x80;
static const uint8_t STA_POWER_ON  = 0x10;
static const uint8_t STA_MODE_MASK     = 0x0F;  // lower nibble of mainPara
static const uint8_t STA_MODE_COOL     = 0x01;
static const uint8_t STA_MODE_FAN_ONLY = 0x02;
static const uint8_t STA_MODE_DRY      = 0x03;
static const uint8_t STA_MODE_HEAT     = 0x04;
static const uint8_t STA_MODE_AUTO     = 0x05;

// ─── STATUS Response Byte[3] (secPara) — Fan speed + Target temp ────────────
//   Upper nibble (0xF0): Fan speed (original: FAN_SPEED_MASK on dataRX[8])
//   Lower nibble (0x0F): Target temp = nibble + 16 (range 16–31°C)
static const uint8_t STA_FAN_MASK   = 0xF0;
static const uint8_t STA_FAN_AUTO   = 0x80;
static const uint8_t STA_FAN_LOW    = 0x90;
static const uint8_t STA_FAN_MEDIUM = 0xA0;
static const uint8_t STA_FAN_FOCUS  = 0xB0;  // mapped to HIGH (closest)
static const uint8_t STA_FAN_MIDDLE = 0xC0;  // mapped to MEDIUM (closest)
static const uint8_t STA_FAN_HIGH   = 0xD0;
static const uint8_t STA_TEMP_MASK  = 0x0F;

// ─── STATUS Response Byte[5] — Swing mode ───────────────────────────────────
//   Bits 5-6 (0x60): Swing mode (original: SWING_MODE_MASK on dataRX[10])
static const uint8_t STA_SWING_MASK  = 0x60;
static const uint8_t STA_SWING_OFF   = 0x00;
static const uint8_t STA_SWING_HORIZ = 0x20;
static const uint8_t STA_SWING_VERT  = 0x40;
static const uint8_t STA_SWING_BOTH  = 0x60;

// ─── CMD 0x0A Power Response Byte[2] ────────────────────────────────────────
static const uint8_t PWR_OFF = 0x04;
static const uint8_t PWR_ON  = 0x0C;

// ─── Direction Enums ────────────────────────────────────────────────────────
enum class AirflowVerticalDirection : uint8_t {
  LAST = 0, MAX_UP = 1, UP = 2, CENTER = 3, DOWN = 4, MAX_DOWN = 5,
};
enum class AirflowHorizontalDirection : uint8_t {
  LAST = 0, MAX_LEFT = 1, LEFT = 2, CENTER = 3, RIGHT = 4, MAX_RIGHT = 5,
};
enum class VerticalSwingDirection : uint8_t {
  OFF = 0, UP_DOWN = 1, UPSIDE = 2, DOWNSIDE = 3,
};
enum class HorizontalSwingDirection : uint8_t {
  OFF = 0, LEFT_RIGHT = 1, LEFTSIDE = 2, CENTER = 3, RIGHTSIDE = 4,
};

// ─── Climate Component Class ────────────────────────────────────────────────
class TclAcClimate : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // YAML configuration setters
  void set_beeper_enabled(bool enabled) { beeper_enabled_ = enabled; }
  void set_display_enabled(bool enabled) { display_enabled_ = enabled; }
  void set_vertical_direction(uint8_t dir) { vertical_direction_ = dir; }
  void set_horizontal_direction(uint8_t dir) { horizontal_direction_ = dir; }
  void set_vertical_swing_direction(uint8_t dir) { vertical_swing_direction_ = dir; }
  void set_horizontal_swing_direction(uint8_t dir) { horizontal_swing_direction_ = dir; }
  void set_force_mode(bool enabled) { force_mode_ = enabled; }
  void set_sensor(sensor::Sensor *sensor) { sensor_ = sensor; }

  // Runtime control (callable from HA actions/lambdas)
  void set_vertical_airflow(AirflowVerticalDirection dir);
  void set_horizontal_airflow(AirflowHorizontalDirection dir);
  void set_vertical_swing(VerticalSwingDirection dir);
  void set_horizontal_swing(HorizontalSwingDirection dir);
  void set_display_state(bool state);
  void set_beeper_state(bool state);
  void set_eco_mode(bool enabled);
  void set_turbo_mode(bool enabled);
  void set_quiet_mode(bool enabled);
  void set_health_mode(bool enabled);

  // State getters
  bool get_beeper_state() const { return beeper_state_; }
  bool get_display_state() const { return display_state_; }
  bool get_eco_mode() const { return eco_mode_; }
  bool get_turbo_mode() const { return turbo_mode_; }
  bool get_quiet_mode() const { return quiet_mode_; }
  bool get_health_mode() const { return health_mode_; }
  AirflowVerticalDirection get_vertical_airflow() const { return vertical_airflow_; }
  AirflowHorizontalDirection get_horizontal_airflow() const { return horizontal_airflow_; }
  VerticalSwingDirection get_vertical_swing() const { return vertical_swing_; }
  HorizontalSwingDirection get_horizontal_swing() const { return horizontal_swing_; }

  climate::ClimateTraits traits() override;

 protected:
  void control(const climate::ClimateCall &call) override;

  // TX methods
  void create_set_packet_(uint8_t *packet);
  void send_packet_(const uint8_t *data, size_t len);
  void send_poll_();
  void send_short_status_query_();
  void send_power_query_();
  uint8_t xor_checksum_(const uint8_t *data, size_t len);

  // RX methods
  void handle_rx_byte_(uint8_t b);
  void dispatch_packet_(const uint8_t *pkt, size_t len);
  void parse_status_(const uint8_t *payload, size_t len);
  void parse_power_(const uint8_t *payload, size_t len);
  void parse_temp_(const uint8_t *payload, size_t len);

  // YAML config (set once in code generation)
  bool beeper_enabled_{true};
  bool display_enabled_{false};
  uint8_t vertical_direction_{5};     // MAX_DOWN
  uint8_t horizontal_direction_{5};   // MAX_RIGHT
  uint8_t vertical_swing_direction_{0};
  uint8_t horizontal_swing_direction_{0};
  bool force_mode_{true};

  // Runtime state
  bool beeper_state_{true};
  bool display_state_{false};
  bool eco_mode_{false};
  bool turbo_mode_{false};
  bool quiet_mode_{false};
  bool health_mode_{false};
  AirflowVerticalDirection vertical_airflow_{AirflowVerticalDirection::LAST};
  AirflowHorizontalDirection horizontal_airflow_{AirflowHorizontalDirection::LAST};
  VerticalSwingDirection vertical_swing_{VerticalSwingDirection::OFF};
  HorizontalSwingDirection horizontal_swing_{HorizontalSwingDirection::OFF};

  // Timing
  uint32_t last_poll_{0};
  uint32_t last_aux_query_{0};
  uint32_t last_rx_time_{0};
  bool aux_toggle_{false};  // Alternates between CMD 0x09 and CMD 0x0A

  // Power state (set by STATUS, used by POWER to ignore temp when ON)
  bool ac_is_on_{false};

  // External temperature sensor (e.g. DHT22) — takes priority over internal AC sensor
  sensor::Sensor *sensor_{nullptr};

  // RX buffer
  std::vector<uint8_t> rx_buffer_;
};

}  // namespace tcl_ac
}  // namespace esphome

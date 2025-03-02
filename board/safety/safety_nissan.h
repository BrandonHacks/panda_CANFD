const SteeringLimits NISSAN_STEERING_LIMITS = {
  .angle_deg_to_can = 100,
  .angle_rate_up_lookup = {
    {0., 5., 15.},
    {5., .8, .15}
  },
  .angle_rate_down_lookup = {
    {0., 5., 15.},
    {5., 3.5, .4}
  },
};

const CanMsg NISSAN_TX_MSGS[] = {
  {0x169, 0, 8},  // LKAS
  {0x2b1, 0, 8},  // PROPILOT_HUD
  {0x4cc, 0, 8},  // PROPILOT_HUD_INFO_MSG
  {0x20b, 2, 6},  // CRUISE_THROTTLE (X-Trail)
  {0x20b, 1, 6},  // CRUISE_THROTTLE (Altima)
  {0x280, 2, 8}   // CANCEL_MSG (Leaf)
};

// Signals duplicated below due to the fact that these messages can come in on either CAN bus, depending on car model.
AddrCheckStruct nissan_addr_checks[] = {
  {.msg = {{0x2, 0, 5, .expected_timestep = 10000U},
           {0x2, 1, 5, .expected_timestep = 10000U}, { 0 }}},  // STEER_ANGLE_SENSOR (100Hz)
  {.msg = {{0x285, 0, 8, .expected_timestep = 20000U},
           {0x285, 1, 8, .expected_timestep = 20000U}, { 0 }}}, // WHEEL_SPEEDS_REAR (50Hz)
  {.msg = {{0x30f, 2, 3, .expected_timestep = 100000U},
           {0x30f, 1, 3, .expected_timestep = 100000U}, { 0 }}}, // CRUISE_STATE (10Hz)
  {.msg = {{0x15c, 0, 8, .expected_timestep = 20000U},
           {0x15c, 1, 8, .expected_timestep = 20000U},
           {0x239, 0, 8, .expected_timestep = 20000U}}}, // GAS_PEDAL (100Hz / 50Hz)
  {.msg = {{0x454, 0, 8, .expected_timestep = 100000U},
           {0x454, 1, 8, .expected_timestep = 100000U},
           {0x1cc, 0, 4, .expected_timestep = 10000U}}}, // DOORS_LIGHTS (10Hz) / BRAKE (100Hz)
};
#define NISSAN_ADDR_CHECK_LEN (sizeof(nissan_addr_checks) / sizeof(nissan_addr_checks[0]))
addr_checks nissan_rx_checks = {nissan_addr_checks, NISSAN_ADDR_CHECK_LEN};

// EPS Location. false = V-CAN, true = C-CAN
bool nissan_alt_eps = false;

static int nissan_rx_hook(CANPacket_t *to_push) {

  bool valid = addr_safety_check(to_push, &nissan_rx_checks, NULL, NULL, NULL, NULL);

  if (valid) {
    int bus = GET_BUS(to_push);
    int addr = GET_ADDR(to_push);

    if (((bus == 0) && (!nissan_alt_eps)) || ((bus == 1) && (nissan_alt_eps))) {
      if (addr == 0x2) {
        // Current steering angle
        // Factor -0.1, little endian
        int angle_meas_new = (GET_BYTES(to_push, 0, 4) & 0xFFFFU);
        // Need to multiply by 10 here as LKAS and Steering wheel are different base unit
        angle_meas_new = to_signed(angle_meas_new, 16) * 10;

        // update array of samples
        update_sample(&angle_meas, angle_meas_new);
      }

      if (addr == 0x285) {
        // Get current speed and standstill
        uint16_t right_rear = (GET_BYTE(to_push, 0) << 8) | (GET_BYTE(to_push, 1));
        uint16_t left_rear = (GET_BYTE(to_push, 2) << 8) | (GET_BYTE(to_push, 3));
        vehicle_moving = (right_rear | left_rear) != 0U;
        update_sample(&vehicle_speed, ((right_rear + left_rear) / 2.0 * 0.005 / 3.6 * VEHICLE_SPEED_FACTOR) + 0.5);
      }

      // X-Trail 0x15c, Leaf 0x239
      if ((addr == 0x15c) || (addr == 0x239)) {
        if (addr == 0x15c){
          gas_pressed = ((GET_BYTE(to_push, 5) << 2) | ((GET_BYTE(to_push, 6) >> 6) & 0x3U)) > 3U;
        } else {
          gas_pressed = GET_BYTE(to_push, 0) > 3U;
        }
      }
    }

    // X-trail 0x454, Leaf  0x239
    if ((addr == 0x454) || (addr == 0x239)) {
      if (addr == 0x454){
        brake_pressed = (GET_BYTE(to_push, 2) & 0x80U) != 0U;
      } else {
        brake_pressed = ((GET_BYTE(to_push, 4) >> 5) & 1U) != 0U;
      }
    }

    // Handle cruise enabled
    if ((addr == 0x30f) && (((bus == 2) && (!nissan_alt_eps)) || ((bus == 1) && (nissan_alt_eps)))) {
      bool cruise_engaged = (GET_BYTE(to_push, 0) >> 3) & 1U;
      pcm_cruise_check(cruise_engaged);
    }

    generic_rx_checks((addr == 0x169) && (bus == 0));
  }
  return valid;
}


static int nissan_tx_hook(CANPacket_t *to_send) {

  int tx = 1;
  int addr = GET_ADDR(to_send);
  bool violation = false;

  if (!msg_allowed(to_send, NISSAN_TX_MSGS, sizeof(NISSAN_TX_MSGS) / sizeof(NISSAN_TX_MSGS[0]))) {
    tx = 0;
  }

  // steer cmd checks
  if (addr == 0x169) {
    int desired_angle = ((GET_BYTE(to_send, 0) << 10) | (GET_BYTE(to_send, 1) << 2) | ((GET_BYTE(to_send, 2) >> 6) & 0x3U));
    bool lka_active = (GET_BYTE(to_send, 6) >> 4) & 1U;

    // offeset 1310 * NISSAN_STEERING_LIMITS.angle_deg_to_can
    desired_angle =  desired_angle - 131000;

    if (steer_angle_cmd_checks(desired_angle, lka_active, NISSAN_STEERING_LIMITS)) {
      violation = true;
    }
  }

  // acc button check, only allow cancel button to be sent
  if (addr == 0x20b) {
    // Violation of any button other than cancel is pressed
    violation |= ((GET_BYTE(to_send, 1) & 0x3dU) > 0U);
  }

  if (violation) {
    tx = 0;
  }

  return tx;
}


static int nissan_fwd_hook(int bus_num, int addr) {
  int bus_fwd = -1;

  if (bus_num == 0) {
    int block_msg = (addr == 0x280); // CANCEL_MSG
    if (!block_msg) {
      bus_fwd = 2;  // ADAS
    }
  }

  if (bus_num == 2) {
    // 0x169 is LKAS, 0x2b1 LKAS_HUD, 0x4cc LKAS_HUD_INFO_MSG
    int block_msg = ((addr == 0x169) || (addr == 0x2b1) || (addr == 0x4cc));
    if (!block_msg) {
      bus_fwd = 0;  // V-CAN
    }
  }

  return bus_fwd;
}

static const addr_checks* nissan_init(uint16_t param) {
  nissan_alt_eps = param ? 1 : 0;
  return &nissan_rx_checks;
}

const safety_hooks nissan_hooks = {
  .init = nissan_init,
  .rx = nissan_rx_hook,
  .tx = nissan_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = nissan_fwd_hook,
};

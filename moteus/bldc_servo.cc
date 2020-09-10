// Copyright 2018-2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bldc_servo.h"

#include <atomic>
#include <cmath>
#include <functional>

#include "mbed.h"
#include "serial_api_hal.h"

#include "PeripheralPins.h"

#include "mjlib/base/assert.h"
#include "mjlib/base/limit.h"
#include "mjlib/base/windowed_average.h"

#include "moteus/foc.h"
#include "moteus/math.h"
#include "moteus/moteus_hw.h"
#include "moteus/stm32_serial.h"
#include "moteus/torque_model.h"

#if defined(TARGET_STM32G4)
#include "moteus/stm32g4_async_uart.h"
#else
#error "Unknown target"
#endif

#ifdef wait_us
#undef wait_us
#endif

namespace micro = mjlib::micro;

namespace moteus {

namespace {
#if defined(TARGET_STM32G4)
using HardwareUart = Stm32G4AsyncUart;
#else
#error "Unknown target"
#endif


using mjlib::base::Limit;

float Threshold(float, float, float) MOTEUS_CCM_ATTRIBUTE;

float Threshold(float value, float lower, float upper) {
  if (value > lower && value < upper) { return 0.0f; }
  return value;
}

float Offset(float minval, float blend, float val) MOTEUS_CCM_ATTRIBUTE;

float Offset(float minval, float blend, float val) {
  if (val == 0.0f) { return 0.0f; }
  if (std::abs(val) >= blend) {
    return (val < 0.0f) ? (-minval + val) : (minval + val);
  }
  const float ratio = val / blend;
  return ratio * (blend + minval);
}

// From make_thermistor_table.py
constexpr float g_thermistor_lookup[] = {
  -74.17f, // 0
  -11.36f, // 128
  1.53f, // 256
  9.97f, // 384
  16.51f, // 512
  21.98f, // 640
  26.79f, // 768
  31.15f, // 896
  35.19f, // 1024
  39.00f, // 1152
  42.65f, // 1280
  46.18f, // 1408
  49.64f, // 1536
  53.05f, // 1664
  56.45f, // 1792
  59.87f, // 1920
  63.33f, // 2048
  66.87f, // 2176
  70.51f, // 2304
  74.29f, // 2432
  78.25f, // 2560
  82.44f, // 2688
  86.92f, // 2816
  91.78f, // 2944
  97.13f, // 3072
  103.13f, // 3200
  110.01f, // 3328
  118.16f, // 3456
  128.23f, // 3584
  141.49f, // 3712
  161.02f, // 3840
  197.66f, // 3968
};

template <typename Array>
int MapConfig(const Array& array, int value) {
  static_assert(sizeof(array) > 0);
  int result = 0;
  for (const auto& item : array) {
    if (value <= item) { return result; }
    result++;
  }
  // Never return past the end.
  return result - 1;
}

#if MOTEUS_HW_REV >= 3
// r4.1 and above have more DC-link capacitance and can run at the
// slower 40kHz PWM frequency.
constexpr int kIntRateHz = 40000;
constexpr int kPwmRateHz = 40000;
#elif MOTEUS_HW_REV <= 2
constexpr int kIntRateHz = 30000;
constexpr int kPwmRateHz = 60000;
#endif
constexpr int kInterruptDivisor = kPwmRateHz / kIntRateHz;
static_assert(kPwmRateHz % kIntRateHz == 0);

// This is used to determine the maximum allowable PWM value so that
// the current sampling is guaranteed to occur while the FETs are
// still low.  It was calibrated using the scope and trial and error.
//
// The primary test is a high torque pulse with absolute position
// limits in place of +-1.0.  Something like "d pos nan 0 1 p0 d0 f1".
// This all but ensures the current controller will saturate.
//
// As of 2020-08-20, 1.79 was the highest value that failed.
constexpr float kCurrentSampleTime = 1.85e-6f;

constexpr float kMinPwm = kCurrentSampleTime / (0.5f / static_cast<float>(kPwmRateHz));
constexpr float kMaxPwm = 1.0f - kMinPwm;

constexpr float kRateHz = kIntRateHz;
constexpr float kPeriodS = 1.0f / kRateHz;

constexpr int kCalibrateCount = 256;

// The maximum amount the absolute encoder can change in one cycle
// without triggering a fault.  Measured as a fraction of a uint16_t
// and corresponds to roughly 28krpm, which is the limit of the AS5047
// encoder.
//  28000 / 60 = 467 Hz
//  467 Hz * 65536 / kIntRate ~= 763
constexpr int16_t kMaxPositionDelta = 28000 / 60 * 65536 / kIntRateHz;

constexpr float kDefaultTorqueConstant = 0.1f;
constexpr float kMaxUnconfiguredCurrent = 5.0f;

constexpr int kMaxVelocityFilter = 256;

IRQn_Type FindUpdateIrq(TIM_TypeDef* timer) {
#if defined(TARGET_STM32G4)
  if (timer == TIM2) {
    return TIM2_IRQn;
  } else if (timer == TIM3) {
    return TIM3_IRQn;
  } else if (timer == TIM4) {
    return TIM4_IRQn;
  }
  MJ_ASSERT(false);
  return TIM2_IRQn;
#else
#error "Unknown target"
#endif
}

volatile uint32_t* FindCcr(TIM_TypeDef* timer, PinName pin) {
  const auto function = pinmap_function(pin, PinMap_PWM);

  const auto inverted = STM_PIN_INVERTED(function);
  MJ_ASSERT(!inverted);

  const auto channel = STM_PIN_CHANNEL(function);

  switch (channel) {
    case 1: { return &timer->CCR1; }
    case 2: { return &timer->CCR2; }
    case 3: { return &timer->CCR3; }
    case 4: { return &timer->CCR4; }
  }
  MJ_ASSERT(false);
  return nullptr;
}

uint32_t FindSqr(PinName pin) {
  const auto function = pinmap_function(pin, PinMap_ADC);

  const auto channel = STM_PIN_CHANNEL(function);
  return channel;
}

/// Read a digital input, but without configuring it in any way.
class DigitalMonitor {
 public:
  DigitalMonitor(PinName pin) {
    const uint32_t port_index = STM_PORT(pin);
    GPIO_TypeDef* gpio = reinterpret_cast<GPIO_TypeDef*>([&]() {
      switch (port_index) {
        case PortA: return GPIOA_BASE;
        case PortB: return GPIOB_BASE;
        case PortC: return GPIOC_BASE;
        case PortD: return GPIOD_BASE;
        case PortE: return GPIOE_BASE;
        case PortF: return GPIOF_BASE;
      }
      MJ_ASSERT(false);
      return GPIOA_BASE;
      }());
    reg_in_ = &gpio->IDR;
    mask_ = static_cast<uint32_t>(1 << (static_cast<uint32_t>(pin) & 0xf));
  }

  bool read() {
    return (*reg_in_ & mask_) != 0;
  }

 private:
  volatile uint32_t* reg_in_ = nullptr;
  uint32_t mask_ = 0;
};
}

class BldcServo::Impl {
 public:
  Impl(micro::PersistentConfig* persistent_config,
       micro::TelemetryManager* telemetry_manager,
       MillisecondTimer* millisecond_timer,
       AS5047* position_sensor,
       MotorDriver* motor_driver,
       const Options& options)
      : options_(options),
        ms_timer_(millisecond_timer),
        position_sensor_(position_sensor),
        motor_driver_(motor_driver),
        pwm1_(options.pwm1),
        pwm2_(options.pwm2),
        pwm3_(options.pwm3),
        monitor1_(options.pwm1),
        monitor2_(options.pwm2),
        monitor3_(options.pwm3),
        current1_(options.current1),
        current2_(options.current2),
        current3_(options.current3),
        vsense_(options.vsense),
        vsense_sqr_(FindSqr(options.vsense)),
        tsense_(options.tsense),
        tsense_sqr_(FindSqr(options.tsense)),
        msense_(options.msense),
        msense_sqr_(FindSqr(options.msense)),
        debug_dac_(options.debug_dac),
        debug_out_(options.debug_out),
        debug_out2_(options.debug_out2),
        debug_serial_([&]() {
            Stm32Serial::Options d_options;
            d_options.tx = options.debug_uart_out;
            d_options.baud_rate = 5450000;
            return d_options;
          }()) {

    clock_.store(0);

    persistent_config->Register("motor", &motor_,
                                std::bind(&Impl::UpdateConfig, this));
    persistent_config->Register("servo", &config_,
                                std::bind(&Impl::UpdateConfig, this));
    persistent_config->Register("servopos", &position_config_,
                                std::bind(&Impl::UpdateConfig, this));
    telemetry_manager->Register("servo_stats", &status_);
    telemetry_manager->Register("servo_cmd", &telemetry_data_);
    telemetry_manager->Register("servo_control", &control_);

    UpdateConfig();

    MJ_ASSERT(!g_impl_);
    g_impl_ = this;
  }

  void Start() {
    ConfigureADC();
    ConfigurePwmTimer();
  }

  ~Impl() {
    g_impl_ = nullptr;
  }

  void Command(const CommandData& data) {
    MJ_ASSERT(data.mode != kFault);
    MJ_ASSERT(data.mode != kEnabling);
    MJ_ASSERT(data.mode != kCalibrating);
    MJ_ASSERT(data.mode != kCalibrationComplete);

    // Actually setting values will happen in the interrupt routine,
    // so we need to update this atomically.
    CommandData* next = next_data_;
    *next = data;

    // If we have a case where the position is left unspecified, but
    // we have a velocity and stop condition, then we pick the sign of
    // the velocity so that we actually move.
    if (std::isnan(next->position) &&
        !std::isnan(next->stop_position) &&
        !std::isnan(next->velocity) &&
        next->velocity != 0.0f) {
      next->velocity = std::abs(next->velocity) *
          ((next->stop_position > status_.unwrapped_position) ?
           1.0f : -1.0f);
    }

    if (next->timeout_s == 0.0f) {
      next->timeout_s = config_.default_timeout_s;
    }

    telemetry_data_ = *next;

    std::swap(current_data_, next_data_);
  }

  const Status& status() const { return status_; }
  const Config& config() const { return config_; }
  const Control& control() const { return control_; }
  const Motor& motor() const { return motor_; }

  bool is_torque_constant_configured() const {
    return motor_.v_per_hz != 0.0f;
  }

  float current_to_torque(float current) const MOTEUS_CCM_ATTRIBUTE {
    TorqueModel model(torque_constant_,
                      motor_.rotation_current_cutoff_A,
                      motor_.rotation_current_scale,
                      motor_.rotation_torque_scale);
    return model.current_to_torque(current);
  }

  float torque_to_current(float torque) const MOTEUS_CCM_ATTRIBUTE {
    TorqueModel model(torque_constant_,
                      motor_.rotation_current_cutoff_A,
                      motor_.rotation_current_scale,
                      motor_.rotation_torque_scale);
    return model.torque_to_current(torque);
  }

  void UpdateConfig() {
    const float kv = 0.5f * 60.0f / motor_.v_per_hz;

    // I have no idea why this fudge is necessary, but it seems to be
    // consistent across every motor I have tried.
    constexpr float kFudge = 0.78;

    torque_constant_ =
        is_torque_constant_configured() ?
        kFudge * 60.0f / (2.0f * kPi * kv) :
        kDefaultTorqueConstant;

    position_constant_ = motor_.poles / 2;

    adc_scale_ = 3.3f / (4096.0f * MOTEUS_CURRENT_SENSE_OHM * config_.i_gain);

    velocity_filter_ = {std::min<size_t>(
          kMaxVelocityFilter, config_.velocity_filter_length)};
  }

  void PollMillisecond() {
    volatile Mode* mode_volatile = &status_.mode;
    Mode mode = *mode_volatile;
    if (mode == kEnabling) {
      motor_driver_->Enable(true);
      *mode_volatile = kCalibrating;
    }
    startup_count_++;
  }

  uint32_t clock() const {
    return clock_.load();
  }

 private:
  void ConfigurePwmTimer() {
    const auto pwm1_timer = pinmap_peripheral(options_.pwm1, PinMap_PWM);
    const auto pwm2_timer = pinmap_peripheral(options_.pwm2, PinMap_PWM);
    const auto pwm3_timer = pinmap_peripheral(options_.pwm3, PinMap_PWM);

    // All three must be the same and be valid.
    MJ_ASSERT(pwm1_timer != 0 &&
              pwm1_timer == pwm2_timer &&
              pwm2_timer == pwm3_timer);
    timer_ = reinterpret_cast<TIM_TypeDef*>(pwm1_timer);
    timer_sr_ = &timer_->SR;
    timer_cr1_ = &timer_->CR1;


    pwm1_ccr_ = FindCcr(timer_, options_.pwm1);
    pwm2_ccr_ = FindCcr(timer_, options_.pwm2);
    pwm3_ccr_ = FindCcr(timer_, options_.pwm3);


    // Enable the update interrupt.
    timer_->DIER = TIM_DIER_UIE;

    // Enable the update interrupt.
    timer_->CR1 =
        // Center-aligned mode 2.  The counter counts up and down
        // alternatively.  Output compare interrupt flags of channels
        // configured in output are set only when the counter is
        // counting up.
        (2 << TIM_CR1_CMS_Pos) |

        // ARR register is buffered.
        TIM_CR1_ARPE;

    // Update once per up/down of the counter.
    timer_->RCR |= 0x01;

    // Set up PWM.

    timer_->PSC = 0; // No prescaler.
    pwm_counts_ = HAL_RCC_GetPCLK1Freq() * 2 / (2 * kPwmRateHz);
    timer_->ARR = pwm_counts_;

    // NOTE: We don't use micro::CallbackTable here because we need the
    // absolute minimum latency possible.
    const auto irqn = FindUpdateIrq(timer_);
    NVIC_SetVector(irqn, reinterpret_cast<uint32_t>(&Impl::GlobalInterrupt));
    HAL_NVIC_SetPriority(irqn, 0, 0);
    NVIC_EnableIRQ(irqn);

    // Reinitialize the counter and update all registers.
    timer_->EGR |= TIM_EGR_UG;

    // Finally, enable the timer.
    timer_->CR1 |= TIM_CR1_CEN;
  }

  void ConfigureADC() {
    constexpr uint16_t kCycleMap[] = {
      2, 6, 12, 24, 47, 92, 247, 640,
    };

    const uint32_t cur_cycles = MapConfig(kCycleMap, config_.adc_cur_cycles);
    const uint32_t aux_cycles = MapConfig(kCycleMap, config_.adc_aux_cycles);
    auto make_cycles = [](auto v) {
      return
        (v << 0) |
        (v << 3) |
        (v << 6) |
        (v << 9) |
        (v << 12) |
        (v << 15) |
        (v << 18) |
        (v << 21) |
        (v << 24);
    };
    const uint32_t all_cur_cycles = make_cycles(cur_cycles);
    const uint32_t all_aux_cycles = make_cycles(aux_cycles);

    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC345_CLK_ENABLE();

    auto disable_adc = [](auto* adc) {
      if (adc->CR & ADC_CR_ADEN) {
        adc->CR |= ADC_CR_ADDIS;
        while (adc->CR & ADC_CR_ADEN);
      }
    };

    // First, we have to disable everything to ensure we are in a
    // known state.
    disable_adc(ADC1);
    disable_adc(ADC2);
    disable_adc(ADC3);
    disable_adc(ADC4);
    disable_adc(ADC5);

    // The prescaler must be at least 6x to be able to accurately read
    // across all channels.  If it is too low, you'll see errors that
    // look like quantization, but not in a particularly uniform way
    // and not consistently across each of the channels.
    ADC12_COMMON->CCR =
        (3 << ADC_CCR_PRESC_Pos);  // Prescaler /6
    ADC345_COMMON->CCR =
        (3 << ADC_CCR_PRESC_Pos);  // Prescaler /6

    // 20.4.6: ADC Deep power-down mode startup procedure
    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC2->CR &= ~ADC_CR_DEEPPWD;
    ADC3->CR &= ~ADC_CR_DEEPPWD;
    ADC4->CR &= ~ADC_CR_DEEPPWD;
    ADC5->CR &= ~ADC_CR_DEEPPWD;

    ADC1->CR |= ADC_CR_ADVREGEN;
    ADC2->CR |= ADC_CR_ADVREGEN;
    ADC3->CR |= ADC_CR_ADVREGEN;
    ADC4->CR |= ADC_CR_ADVREGEN;
    ADC5->CR |= ADC_CR_ADVREGEN;

    // tADCREG_S = 20us per STM32G474xB datasheet
    ms_timer_->wait_us(20);

    // 20.4.8: Calibration
    ADC1->CR |= ADC_CR_ADCAL;
    ADC2->CR |= ADC_CR_ADCAL;
    ADC3->CR |= ADC_CR_ADCAL;
    ADC4->CR |= ADC_CR_ADCAL;
    ADC5->CR |= ADC_CR_ADCAL;

    while ((ADC1->CR & ADC_CR_ADCAL) ||
           (ADC2->CR & ADC_CR_ADCAL) ||
           (ADC3->CR & ADC_CR_ADCAL) ||
           (ADC4->CR & ADC_CR_ADCAL) ||
           (ADC5->CR & ADC_CR_ADCAL));

    ms_timer_->wait_us(1);

    // 20.4.9: Software procedure to enable the ADC
    ADC1->ISR |= ADC_ISR_ADRDY;
    ADC2->ISR |= ADC_ISR_ADRDY;
    ADC3->ISR |= ADC_ISR_ADRDY;
    ADC4->ISR |= ADC_ISR_ADRDY;
    ADC5->ISR |= ADC_ISR_ADRDY;

    ADC1->CR |= ADC_CR_ADEN;
    ADC2->CR |= ADC_CR_ADEN;
    ADC3->CR |= ADC_CR_ADEN;
    ADC4->CR |= ADC_CR_ADEN;
    ADC5->CR |= ADC_CR_ADEN;

    while (!(ADC1->ISR & ADC_ISR_ADRDY) ||
           !(ADC2->ISR & ADC_ISR_ADRDY) ||
           !(ADC3->ISR & ADC_ISR_ADRDY) ||
           !(ADC4->ISR & ADC_ISR_ADRDY) ||
           !(ADC5->ISR & ADC_ISR_ADRDY));

    ADC1->ISR |= ADC_ISR_ADRDY;
    ADC2->ISR |= ADC_ISR_ADRDY;
    ADC3->ISR |= ADC_ISR_ADRDY;
    ADC4->ISR |= ADC_ISR_ADRDY;
    ADC5->ISR |= ADC_ISR_ADRDY;

    ADC1->SQR1 =
        (0 << ADC_SQR1_L_Pos) |  // length 1
        FindSqr(options_.current2) << ADC_SQR1_SQ1_Pos;
    ADC2->SQR1 =
        (0 << ADC_SQR1_L_Pos) |  // length 1
        FindSqr(options_.current3) << ADC_SQR1_SQ1_Pos;
    ADC3->SQR1 =
        (0 << ADC_SQR1_L_Pos) |  // length 1
        FindSqr(options_.current1) << ADC_SQR1_SQ1_Pos;
    if (hw_rev_ <= 4) {
      // For version <=4, we sample the motor temperature and the
      // battery sense first.
      ADC4->SQR1 =
          (1 << ADC_SQR1_L_Pos) |  // length 1
          (msense_sqr_ << ADC_SQR1_SQ1_Pos);
      ADC5->SQR1 =
          (1 << ADC_SQR1_L_Pos) |  // length 1
          (vsense_sqr_ << ADC_SQR1_SQ1_Pos);
    } else if (hw_rev_ >= 5) {
      // For 5+, ADC4 always stays on the battery.
      ADC4->SQR1 =
          (1 << ADC_SQR1_L_Pos) |  // length 1
          (vsense_sqr_ << ADC_SQR1_SQ1_Pos);
      ADC5->SQR1 =
          (1 << ADC_SQR1_L_Pos) |  // length 1
          (tsense_sqr_ << ADC_SQR1_SQ1_Pos);
    }

    ADC1->SMPR1 = all_cur_cycles;
    ADC1->SMPR2 = all_cur_cycles;
    ADC2->SMPR1 = all_cur_cycles;
    ADC2->SMPR2 = all_cur_cycles;
    ADC3->SMPR1 = all_cur_cycles;
    ADC3->SMPR2 = all_cur_cycles;

    ADC4->SMPR1 = all_aux_cycles;
    ADC4->SMPR2 = all_aux_cycles;
    ADC5->SMPR1 = all_aux_cycles;
    ADC5->SMPR2 = all_aux_cycles;
  }

  static void WaitForAdc(ADC_TypeDef* adc) MOTEUS_CCM_ATTRIBUTE {
    while ((adc->ISR & ADC_ISR_EOC) == 0);
    adc->ISR |= ADC_ISR_EOC;
  }

  // CALLED IN INTERRUPT CONTEXT.
  static void GlobalInterrupt() MOTEUS_CCM_ATTRIBUTE {
    g_impl_->ISR_HandleTimer();
  }

  // CALLED IN INTERRUPT CONTEXT.
  void ISR_HandleTimer() __attribute__((always_inline)) MOTEUS_CCM_ATTRIBUTE {
    // From here, until when we finish sampling the ADC has a critical
    // speed requirement.  Any extra cycles will result in a lower
    // maximal duty cycle of the controller.  Thus there are lots of
    // micro-optimizations to try and speed things up by little bits.
    const auto sr = *timer_sr_;
    const auto cr = *timer_cr1_;

    // Reset the status register.
    timer_->SR = 0x00;

    if ((sr & TIM_SR_UIF) &&
        (cr & TIM_CR1_DIR)) {
      ISR_DoTimer();
    }
  }

  void ISR_DoTimer() __attribute__((always_inline)) MOTEUS_CCM_ATTRIBUTE {
    // We start our conversion here so that it can work while we get
    // ready.  This means we will throw away the result if our control
    // timer says it isn't our turn yet, but that is a relatively
    // minor waste.
    ADC1->CR |= ADC_CR_ADSTART;
    ADC2->CR |= ADC_CR_ADSTART;
    ADC3->CR |= ADC_CR_ADSTART;

    ADC4->CR |= ADC_CR_ADSTART;
    ADC5->CR |= ADC_CR_ADSTART;

    if constexpr (kInterruptDivisor != 1) {
      phase_ = (phase_ + 1) % kInterruptDivisor;

      if (phase_ != 0) { return; }
    }

#ifdef MOTEUS_PERFORMANCE_MEASURE
    DWT->CYCCNT = 0;
#endif

    // No matter what mode we are in, always sample our ADC and
    // position sensors.
    ISR_DoSense();
#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.sense = DWT->CYCCNT;
#endif

    SinCos sin_cos = cordic_(RadiansToQ31(status_.electrical_theta));
    status_.sin = sin_cos.s;
    status_.cos = sin_cos.c;

    ISR_CalculateCurrentState(sin_cos);
#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.curstate = DWT->CYCCNT;
#endif

    ISR_DoControl(sin_cos);

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.control = DWT->CYCCNT;
#endif

    clock_++;

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.done = DWT->CYCCNT;
#endif

    const uint32_t cnt = timer_->CNT;
    status_.final_timer =
        ((*timer_cr1_) & TIM_CR1_DIR) ?
        (pwm_counts_ - cnt) :
        (pwm_counts_ + cnt);
    status_.total_timer = 2 * pwm_counts_;
    debug_out_ = 0;
  }

  void ISR_DoSense() __attribute__((always_inline)) MOTEUS_CCM_ATTRIBUTE {
    // Wait for conversion to complete.
    WaitForAdc(ADC1);
    WaitForAdc(ADC2);
    WaitForAdc(ADC3);

    // We would like to set this debug pin as soon as possible.
    // However, if we flip it while the current ADCs are sampling,
    // they can get a lot more noise in some situations.  Thus just
    // wait until now.
    debug_out_ = 1;

    // We are now out of the most time critical portion of the ISR,
    // although it is still all pretty time critical since it runs
    // at 40kHz.  But time spent until now actually limits the
    // maximum duty cycle we can achieve, whereas time spent below
    // just eats cycles the rest of the code could be using.

    // Check to see if any motor outputs are now high.  If so, fault,
    // because we have exceeded the maximum duty cycle we can achieve
    // while still sampling current correctly.
    if (status_.mode != kFault &&
        (monitor1_.read() ||
         monitor2_.read() ||
         monitor3_.read())) {
      status_.mode = kFault;
      status_.fault = errc::kPwmCycleOverrun;
    }

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.adc_done = DWT->CYCCNT;
#endif

    position_sensor_->StartSample();

    if (current_data_->rezero_position) {
      status_.position_to_set = *current_data_->rezero_position;
      status_.rezeroed = true;
      current_data_->rezero_position = {};
    }

    if (std::isnan(current_data_->timeout_s) ||
        current_data_->timeout_s != 0.0f) {
      status_.timeout_s = current_data_->timeout_s;
      current_data_->timeout_s = 0.0;
    }

    status_.adc_cur1_raw = ADC3->DR;
    status_.adc_cur2_raw = ADC1->DR;
    status_.adc_cur3_raw = ADC2->DR;
    WaitForAdc(ADC4);
    WaitForAdc(ADC5);

    if (hw_rev_ <= 4) {
      status_.adc_motor_temp_raw = ADC4->DR;
      status_.adc_voltage_sense_raw = ADC5->DR;
    } else {
      status_.adc_voltage_sense_raw = ADC4->DR;
      status_.adc_fet_temp_raw = ADC5->DR;
    }

    // Start sampling the temperature.
    //
    // The datasheet says that ADSTP *must* be activated before
    // switching channels to guarantee that a conversion is not in
    // progress.  At this point, we know a conversion is not in
    // progress, since we're in one-shot mode.  However, if we don't
    // assert ADSTP, then the channel doesn't switch properly.  Guess
    // it is needed for other reasons too?
    ADC5->CR |= ADC_CR_ADSTP;
    while (ADC5->CR & ADC_CR_ADSTP);

    if (hw_rev_ <= 4) {
      ADC5->SQR1 =
          (0 << ADC_SQR1_L_Pos) |  // length 1
          tsense_sqr_ << ADC_SQR1_SQ1_Pos;
    } else {
      ADC5->SQR1 =
          (0 << ADC_SQR1_L_Pos) |  // length 1
          msense_sqr_ << ADC_SQR1_SQ1_Pos;
    }
    ADC5->CR |= ADC_CR_ADSTART;

    // Wait for the position sample to finish.
    const uint16_t old_position = status_.position;

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.start_pos_sample = DWT->CYCCNT;
#endif

    status_.position_raw = position_sensor_->FinishSample();

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.done_pos_sample = DWT->CYCCNT;
#endif

    status_.position =
        (motor_.invert ? (65536 - status_.position_raw) : status_.position_raw);

    const int offset_size = motor_.offset.size();
    const int offset_index = status_.position * offset_size / 65536;
    MJ_ASSERT(offset_index >= 0 && offset_index < offset_size);

    constexpr float kU16ToTheta = k2Pi / 65536.0f;
    status_.electrical_theta =
        WrapZeroToTwoPi(
            ((position_constant_ * status_.position) % 65536) * kU16ToTheta +
            motor_.offset[offset_index]);

    const int16_t delta_position =
        static_cast<int16_t>(status_.position - old_position);
    if ((status_.mode != kStopped && status_.mode != kFault) &&
        std::abs(delta_position) > kMaxPositionDelta) {
      // We probably had an error when reading the position.  We must fault.
      status_.mode = kFault;
      status_.fault = errc::kEncoderFault;
    }

    // While we are in the first calibrating state, our unwrapped
    // position is forced to be within one rotation of 0.  Also, the
    // AS5047 isn't guaranteed to be valid until 10ms after startup.
    if (!std::isnan(status_.position_to_set) && startup_count_.load() > 10) {
      const int16_t zero_position =
          static_cast<int16_t>(
              static_cast<int32_t>(status_.position) +
              motor_.position_offset * (motor_.invert ? -1 : 1));
      const float error = status_.position_to_set -
          zero_position * motor_.unwrapped_position_scale/ 65536.0f;;
      const float integral_offsets =
          std::round(error / motor_.unwrapped_position_scale);
      status_.unwrapped_position_raw =
          zero_position + integral_offsets * 65536.0f;
      status_.position_to_set = std::numeric_limits<float>::quiet_NaN();
    } else {
      status_.unwrapped_position_raw += delta_position;
    }

    {
      // We construct the velocity in a careful way so as to maximize
      // the available resolution.  The windowed filter is calculated
      // losslessly.  Then, the average is conducted in the floating
      // point domain, so as to not suffer from rounding error.
      velocity_filter_.Add(delta_position);
      constexpr float velocity_scale = 1.0f / 65536.0f;
      status_.velocity =
          static_cast<float>(velocity_filter_.total()) *
          motor_.unwrapped_position_scale * velocity_scale * kRateHz /
          static_cast<float>(velocity_filter_.size());
    }

    status_.unwrapped_position =
        status_.unwrapped_position_raw * motor_.unwrapped_position_scale *
        (1.0f / 65536.0f);

    // The temperature sensing should be done by now, but just double
    // check.
    WaitForAdc(ADC5);
    if (hw_rev_ <= 4) {
      status_.adc_fet_temp_raw = ADC5->DR;
    } else {
      status_.adc_motor_temp_raw = ADC5->DR;
    }

    ADC5->CR |= ADC_CR_ADSTP;
    while (ADC5->CR & ADC_CR_ADSTP);

    if (hw_rev_ <= 4) {
      // Switch back to the voltage sense resistor.
      ADC5->SQR1 =
          (0 << ADC_SQR1_L_Pos) |  // length 1
          (vsense_sqr_ << ADC_SQR1_SQ1_Pos);
    } else {
      // Switch back to FET temp sense.
      ADC5->SQR1 =
          (0 << ADC_SQR1_L_Pos) |  // length 1
          (tsense_sqr_ << ADC_SQR1_SQ1_Pos);
    }

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.done_temp_sample = DWT->CYCCNT;
#endif

    // Kick off a conversion just to get the FET temp out of the system.
    ADC5->CR |= ADC_CR_ADSTART;

    {
      constexpr int adc_max = 4096;
      constexpr size_t size_thermistor_table =
          sizeof(g_thermistor_lookup) / sizeof(*g_thermistor_lookup);
      size_t offset = std::max<size_t>(
          1, std::min<size_t>(
              size_thermistor_table - 2,
              status_.adc_fet_temp_raw * size_thermistor_table / adc_max));
      const int16_t this_value = offset * adc_max / size_thermistor_table;
      const int16_t next_value = (offset + 1) * adc_max / size_thermistor_table;
      const float temp1 = g_thermistor_lookup[offset];
      const float temp2 = g_thermistor_lookup[offset + 1];
      status_.fet_temp_C = temp1 +
          (temp2 - temp1) *
          static_cast<float>(status_.adc_fet_temp_raw - this_value) /
          static_cast<float>(next_value - this_value);
    }
  }

  void ISR_UpdateFilteredBusV(float* filtered, float period_s) const MOTEUS_CCM_ATTRIBUTE {
    if (std::isnan(*filtered)) {
      *filtered = status_.bus_V;
    } else {
      const float alpha = 1.0f / (kRateHz * period_s);
      *filtered = alpha * status_.bus_V + (1.0f - alpha) * *filtered;
    }
  }

  // This is called from the ISR.
  void ISR_CalculateCurrentState(const SinCos& sin_cos) MOTEUS_CCM_ATTRIBUTE {
    status_.cur1_A = (status_.adc_cur1_raw - status_.adc_cur1_offset) * adc_scale_;
    status_.cur2_A = (status_.adc_cur2_raw - status_.adc_cur2_offset) * adc_scale_;
    status_.cur3_A = (status_.adc_cur3_raw - status_.adc_cur3_offset) * adc_scale_;
    status_.bus_V = status_.adc_voltage_sense_raw * config_.v_scale_V;

    ISR_UpdateFilteredBusV(&status_.filt_bus_V, 0.5f);
    ISR_UpdateFilteredBusV(&status_.filt_1ms_bus_V, 0.001f);

    DqTransform dq{sin_cos,
          status_.cur1_A,
          status_.cur3_A,
          status_.cur2_A
          };
    status_.d_A = dq.d;
    status_.q_A = dq.q;
    status_.torque_Nm = torque_on() ? (
        current_to_torque(status_.q_A) /
        motor_.unwrapped_position_scale) : 0.0f;

    DAC->DHR12R1 = 1024 + std::max<int>(
        0, std::min<int>(
            2047, static_cast<int>(1024.0f * status_.d_A / 30.0f)));
  }

  bool torque_on() const {
    switch (status_.mode) {
      case kNumModes: {
        MJ_ASSERT(false);
        return false;
      }
      case kFault:
      case kCalibrating:
      case kCalibrationComplete:
      case kEnabling:
      case kStopped: {
        return false;
      }
      case kPwm:
      case kVoltage:
      case kVoltageFoc:
      case kVoltageDq:
      case kCurrent:
      case kPosition:
      case kPositionTimeout:
      case kZeroVelocity:
      case kStayWithinBounds: {
        return true;
      }
    }
    return false;
  }

  void ISR_MaybeChangeMode(CommandData* data) MOTEUS_CCM_ATTRIBUTE {
    // We are requesting a different mode than we are in now.  Do our
    // best to advance if possible.
    switch (data->mode) {
      case kNumModes:
      case kFault:
      case kCalibrating:
      case kCalibrationComplete: {
        // These should not be possible.
        MJ_ASSERT(false);
        return;
      }
      case kStopped: {
        // It is always valid to enter stopped mode.
        status_.mode = kStopped;
        return;
      }
      case kEnabling: {
        // We can never change out from enabling in ISR context.
        return;
      }
      case kPwm:
      case kVoltage:
      case kVoltageFoc:
      case kVoltageDq:
      case kCurrent:
      case kPosition:
      case kPositionTimeout:
      case kZeroVelocity:
      case kStayWithinBounds: {
        switch (status_.mode) {
          case kNumModes: {
            MJ_ASSERT(false);
            return;
          }
          case kFault: {
            // We cannot leave a fault state directly into an active state.
            return;
          }
          case kStopped: {
            // From a stopped state, we first have to enter the
            // calibrating state.
            ISR_StartCalibrating();
            return;
          }
          case kEnabling:
          case kCalibrating: {
            // We can only leave this state when calibration is
            // complete.
            return;
          }
          case kCalibrationComplete:
          case kPwm:
          case kVoltage:
          case kVoltageFoc:
          case kVoltageDq:
          case kCurrent:
          case kPosition:
          case kZeroVelocity:
          case kStayWithinBounds: {
            if ((data->mode == kPosition || data->mode == kStayWithinBounds) &&
                ISR_IsOutsideLimits()) {
              status_.mode = kFault;
              status_.fault = errc::kStartOutsideLimit;
            } else {
              // Yep, we can do this.
              status_.mode = data->mode;

              // Start from scratch if we are in a new mode.
              ISR_ClearPid(kAlwaysClear);
            }

            return;
          }
          case kPositionTimeout: {
            // We cannot leave this mode except through a stop.
            return;
          }
        }
      }
    }
  }

  bool ISR_IsOutsideLimits() {
    return ((!std::isnan(position_config_.position_min) &&
             status_.unwrapped_position < position_config_.position_min) ||
            (!std::isnan(position_config_.position_max) &&
             status_.unwrapped_position > position_config_.position_max));
  }

  void ISR_StartCalibrating() {
    status_.mode = kEnabling;

    // The main context will set our state to kCalibrating when the
    // motor driver is fully enabled.

    (*pwm1_ccr_) = 0;
    (*pwm2_ccr_) = 0;
    (*pwm3_ccr_) = 0;

    // Power should already be false for any state we could possibly
    // be in, but lets just be certain.
    motor_driver_->Power(false);

    calibrate_adc1_ = 0;
    calibrate_adc2_ = 0;
    calibrate_adc3_ = 0;
    calibrate_count_ = 0;
  }

  enum ClearMode {
    kClearIfMode,
    kAlwaysClear,
  };

  void ISR_ClearPid(ClearMode force_clear) MOTEUS_CCM_ATTRIBUTE {
    const bool current_pid_active = [&]() MOTEUS_CCM_ATTRIBUTE {
      switch (status_.mode) {
        case kNumModes:
        case kStopped:
        case kFault:
        case kEnabling:
        case kCalibrating:
        case kCalibrationComplete:
        case kPwm:
        case kVoltage:
        case kVoltageFoc:
        case kVoltageDq:
          return false;
        case kCurrent:
        case kPosition:
        case kPositionTimeout:
        case kZeroVelocity:
        case kStayWithinBounds:
          return true;
      }
      return false;
    }();

    if (!current_pid_active || force_clear == kAlwaysClear) {
      status_.pid_d.Clear();
      status_.pid_q.Clear();

      // We always want to start from 0 current when initiating
      // current control of some form.
      status_.pid_d.desired = 0.0f;
      status_.pid_q.desired = 0.0f;
    }

    const bool position_pid_active = [&]() MOTEUS_CCM_ATTRIBUTE {
      switch (status_.mode) {
        case kNumModes:
        case kStopped:
        case kFault:
        case kEnabling:
        case kCalibrating:
        case kCalibrationComplete:
        case kPwm:
        case kVoltage:
        case kVoltageFoc:
        case kVoltageDq:
        case kCurrent:
          return false;
        case kPosition:
        case kPositionTimeout:
        case kZeroVelocity:
        case kStayWithinBounds:
          return true;
      }
      return false;
    }();

    if (!position_pid_active || force_clear == kAlwaysClear) {
      status_.pid_position.Clear();
      status_.control_position = std::numeric_limits<float>::quiet_NaN();
    }
  }

  void ISR_DoControl(const SinCos& sin_cos) MOTEUS_CCM_ATTRIBUTE {
    // current_data_ is volatile, so read it out now, and operate on
    // the pointer for the rest of the routine.
    CommandData* data = current_data_;

    control_.Clear();

    if (data->set_position) {
      status_.unwrapped_position_raw =
          static_cast<int32_t>(*data->set_position * 65536.0f);
      data->set_position = {};
    }

    if (!std::isnan(status_.timeout_s) && status_.timeout_s > 0.0f) {
      status_.timeout_s = std::max(0.0f, status_.timeout_s - kPeriodS);
    }

    // See if we need to update our current mode.
    if (data->mode != status_.mode) {
      ISR_MaybeChangeMode(data);
    }

    // Handle our persistent fault conditions.
    if (status_.mode != kStopped && status_.mode != kFault) {
      if (motor_driver_->fault()) {
        status_.mode = kFault;
        status_.fault = errc::kMotorDriverFault;
      }
      if (status_.bus_V > config_.max_voltage) {
        status_.mode = kFault;
        status_.fault = errc::kOverVoltage;
      }
      if (status_.fet_temp_C > config_.fault_temperature) {
        status_.mode = kFault;
        status_.fault = errc::kOverTemperature;
      }
    }

    if ((status_.mode == kPosition || status_.mode == kStayWithinBounds) &&
        !std::isnan(status_.timeout_s) &&
        status_.timeout_s <= 0.0f) {
      status_.mode = kPositionTimeout;
    }

    // Ensure unused PID controllers have zerod state.
    ISR_ClearPid(kClearIfMode);

    if (status_.mode != kFault) {
      status_.fault = errc::kSuccess;
    }

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.control_sel_mode = DWT->CYCCNT;
#endif

    switch (status_.mode) {
      case kNumModes:
      case kStopped: {
        ISR_DoStopped();
        break;
      }
      case kFault: {
        ISR_DoFault();
        break;
      }
      case kEnabling: {
        break;
      }
      case kCalibrating: {
        ISR_DoCalibrating();
        break;
      }
      case kCalibrationComplete: {
        break;
      }
      case kPwm: {
        ISR_DoPwmControl(data->pwm);
        break;
      }
      case kVoltage: {
        ISR_DoVoltageControl(data->phase_v);
        break;
      }
      case kVoltageFoc: {
        ISR_DoVoltageFOC(data->theta, data->voltage);
        break;
      }
      case kVoltageDq: {
        ISR_DoVoltageDQ(sin_cos, data->d_V, data->q_V);
        break;
      }
      case kCurrent: {
        ISR_DoCurrent(sin_cos, data->i_d_A, data->i_q_A);
        break;
      }
      case kPosition: {
        ISR_DoPosition(sin_cos, data);
        break;
      }
      case kPositionTimeout:
      case kZeroVelocity: {
        ISR_DoZeroVelocity(sin_cos, data);
        break;
      }
      case kStayWithinBounds: {
        ISR_DoStayWithinBounds(sin_cos, data);
        break;
      }
    }
  }

  void ISR_DoStopped() MOTEUS_CCM_ATTRIBUTE {
    motor_driver_->Enable(false);
    motor_driver_->Power(false);
    *pwm1_ccr_ = 0;
    *pwm2_ccr_ = 0;
    *pwm3_ccr_ = 0;
  }

  void ISR_DoFault() MOTEUS_CCM_ATTRIBUTE {
    motor_driver_->Power(false);
    *pwm1_ccr_ = 0;
    *pwm2_ccr_ = 0;
    *pwm3_ccr_ = 0;
  }

  void ISR_DoCalibrating() {
    calibrate_adc1_ += status_.adc_cur1_raw;
    calibrate_adc2_ += status_.adc_cur2_raw;
    calibrate_adc3_ += status_.adc_cur3_raw;
    calibrate_count_++;

    if (calibrate_count_ < kCalibrateCount) {
      return;
    }

    const uint16_t new_adc1_offset = calibrate_adc1_ / kCalibrateCount;
    const uint16_t new_adc2_offset = calibrate_adc2_ / kCalibrateCount;
    const uint16_t new_adc3_offset = calibrate_adc3_ / kCalibrateCount;

    if (std::abs(static_cast<int>(new_adc1_offset) - 2048) > 200 ||
        std::abs(static_cast<int>(new_adc2_offset) - 2048) > 200 ||
        std::abs(static_cast<int>(new_adc3_offset) - 2048) > 200) {
      // Error calibrating.  Just fault out.
      status_.mode = kFault;
      status_.fault = errc::kCalibrationFault;
      return;
    }

    status_.adc_cur1_offset = new_adc1_offset;
    status_.adc_cur2_offset = new_adc2_offset;
    status_.adc_cur3_offset = new_adc3_offset;
    status_.mode = kCalibrationComplete;
  }

  void ISR_DoPwmControl(const Vec3& pwm) MOTEUS_CCM_ATTRIBUTE {
    control_.pwm.a = LimitPwm(pwm.a);
    control_.pwm.b = LimitPwm(pwm.b);
    control_.pwm.c = LimitPwm(pwm.c);

    const uint16_t pwm1 = static_cast<uint16_t>(control_.pwm.a * pwm_counts_);
    const uint16_t pwm2 = static_cast<uint16_t>(control_.pwm.b * pwm_counts_);
    const uint16_t pwm3 = static_cast<uint16_t>(control_.pwm.c * pwm_counts_);

    // NOTE(jpieper): We flip pwm2 and pwm3 here, which changes the
    // order of stepping.  Why you may ask?  No good reason.  It does
    // require that the currents be similarly swapped in
    // ISR_CalculateCurrentState.  Changing it back now would reverse
    // the sign of position for any existing motor, so it isn't an
    // easy change to make.
    *pwm1_ccr_ = pwm1;
    *pwm2_ccr_ = pwm3;
    *pwm3_ccr_ = pwm2;

    motor_driver_->Power(true);
  }

  float ISR_VoltageToPwm(float v) const MOTEUS_CCM_ATTRIBUTE {
    return 0.5f + Offset(config_.pwm_min, config_.pwm_min_blend,
                         v / status_.filt_bus_V);
  }

  void ISR_DoVoltageControl(const Vec3& voltage) MOTEUS_CCM_ATTRIBUTE {
    control_.voltage = voltage;

    ISR_DoPwmControl(Vec3{
        ISR_VoltageToPwm(voltage.a),
            ISR_VoltageToPwm(voltage.b),
            ISR_VoltageToPwm(voltage.c)});
  }

  void ISR_DoVoltageFOC(float theta, float voltage) MOTEUS_CCM_ATTRIBUTE {
    SinCos sc = cordic_(RadiansToQ31(theta));
    const float max_voltage = (0.5f - kMinPwm) * status_.filt_bus_V;
    InverseDqTransform idt(sc, Limit(voltage, -max_voltage, max_voltage), 0);
    ISR_DoVoltageControl(Vec3{idt.a, idt.b, idt.c});
  }

  void ISR_DoCurrent(const SinCos& sin_cos, float i_d_A_in, float i_q_A_in) MOTEUS_CCM_ATTRIBUTE {
    auto limit_q_current = [&](float in) MOTEUS_CCM_ATTRIBUTE {
      if (!std::isnan(position_config_.position_max) &&
          status_.unwrapped_position > position_config_.position_max &&
          in > 0.0f) {
        // We derate the request in the direction that moves it
        // further outside the position limits.  This is mostly useful
        // when feedforward is applied, as otherwise, the position
        // limits could easily be exceeded.  Without feedforward, we
        // shouldn't really be trying to push outside the limits
        // anyhow.
        return in *
            std::max(0.0f,
                     1.0f - (status_.unwrapped_position -
                             position_config_.position_max) /
                     config_.position_derate);
      }
      if (!std::isnan(position_config_.position_min) &&
          status_.unwrapped_position < position_config_.position_min &&
          in < 0.0f) {
        return in *
            std::max(0.0f,
                     1.0f - (position_config_.position_min -
                             status_.unwrapped_position) /
                     config_.position_derate);
      }

      return in;
    };

    auto limit_either_current = [&](float in) MOTEUS_CCM_ATTRIBUTE {
      const float derate_fraction = (
          status_.fet_temp_C - config_.derate_temperature) / (
              config_.fault_temperature - config_.derate_temperature);
      const float temp_limit_A = std::min<float>(
          config_.max_current_A,
          std::max<float>(
              0.0f,
              derate_fraction * (config_.derate_current_A -
                                 config_.max_current_A) + config_.max_current_A));
      return Limit(in, -temp_limit_A, temp_limit_A);
    };

    const float i_q_A = limit_either_current(limit_q_current(i_q_A_in));
    const float i_d_A = limit_either_current(i_d_A_in);

    control_.i_d_A = i_d_A;
    control_.i_q_A = i_q_A;

    const float d_V =
        (config_.feedforward_scale * i_d_A * motor_.resistance_ohm) +
        pid_d_.Apply(status_.d_A, i_d_A, 1.0f, 0.0f, kRateHz);

    const float q_V =
        (config_.feedforward_scale * (
            i_q_A * motor_.resistance_ohm -
            status_.velocity * motor_.v_per_hz /
            motor_.unwrapped_position_scale)) +
        pid_q_.Apply(status_.q_A, i_q_A, 0.0f, 0.0f, kRateHz);

    ISR_DoVoltageDQ(sin_cos, d_V, q_V);
  }

  void ISR_DoVoltageDQ(const SinCos& sin_cos, float d_V, float q_V) MOTEUS_CCM_ATTRIBUTE {
    if (motor_.poles == 0) {
      // We aren't configured yet.
      status_.mode = kFault;
      status_.fault = errc::kMotorNotConfigured;
      return;
    }

    control_.d_V = d_V;
    control_.q_V = q_V;

    const float max_voltage = (0.5f - kMinPwm) * status_.filt_bus_V;
    auto limit_v = [&](float in) MOTEUS_CCM_ATTRIBUTE {
      return Limit(in, -max_voltage, max_voltage);
    };
    InverseDqTransform idt(sin_cos, limit_v(control_.d_V),
                           limit_v(control_.q_V));

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.control_done_cur = DWT->CYCCNT;
#endif

    ISR_DoVoltageControl(Vec3{idt.a, idt.b, idt.c});
  }

  void ISR_DoZeroVelocity(const SinCos& sin_cos, CommandData* data) MOTEUS_CCM_ATTRIBUTE {
    PID::ApplyOptions apply_options;
    apply_options.kp_scale = 0.0;
    apply_options.kd_scale = 1.0;

    ISR_DoPositionCommon(sin_cos, data,
                         apply_options, config_.timeout_max_torque_Nm,
                         0.0f, 0.0f);
  }

  void ISR_DoPosition(const SinCos& sin_cos, CommandData* data) MOTEUS_CCM_ATTRIBUTE {
    PID::ApplyOptions apply_options;
    apply_options.kp_scale = data->kp_scale;
    apply_options.kd_scale = data->kd_scale;

    ISR_DoPositionCommon(sin_cos, data, apply_options, data->max_torque_Nm,
                         data->feedforward_Nm, data->velocity);
  }

  void ISR_DoPositionCommon(
      const SinCos& sin_cos, CommandData* data,
      const PID::ApplyOptions& pid_options,
      float max_torque_Nm,
      float feedforward_Nm,
      float velocity) MOTEUS_CCM_ATTRIBUTE {
    if (!std::isnan(data->position)) {
      status_.control_position = data->position;
      data->position = std::numeric_limits<float>::quiet_NaN();
    } else if (std::isnan(status_.control_position)) {
      status_.control_position = status_.unwrapped_position;
    }

    auto velocity_command = velocity;

    const auto old_position = status_.control_position;
    status_.control_position =
        Limit(status_.control_position + velocity_command / kRateHz,
              position_config_.position_min,
              position_config_.position_max);
    if (!std::isnan(data->stop_position)) {
      if ((status_.control_position -
           data->stop_position) * velocity_command > 0.0f) {
        // We are moving away from the stop position.  Force it to be there.
        status_.control_position = data->stop_position;
      }
    }
    if (status_.control_position == old_position) {
      // We have hit a limit.  Assume a velocity of 0.
      velocity_command = 0.0f;
    }

    const float measured_velocity = Threshold(
        status_.velocity, -config_.velocity_threshold,
        config_.velocity_threshold);

    const float unlimited_torque_Nm =
        pid_position_.Apply(status_.unwrapped_position,
                            status_.control_position,
                            measured_velocity, velocity_command,
                            kRateHz,
                            pid_options) +
        feedforward_Nm;

    const float limited_torque_Nm =
        Limit(unlimited_torque_Nm, -max_torque_Nm, max_torque_Nm);

    control_.torque_Nm = limited_torque_Nm;

    const float limited_q_A =
        torque_to_current(limited_torque_Nm * motor_.unwrapped_position_scale);

    const float q_A =
        is_torque_constant_configured() ?
        limited_q_A :
        Limit(limited_q_A, -kMaxUnconfiguredCurrent, kMaxUnconfiguredCurrent);

    const float d_A = [&]() MOTEUS_CCM_ATTRIBUTE {
      if (config_.flux_brake_min_voltage <= 0.0f) {
        return 0.0f;
      }

      const auto error = (
          status_.filt_1ms_bus_V - config_.flux_brake_min_voltage);

      if (error <= 0.0f) {
        return 0.0f;
      }

      return (error / config_.flux_brake_resistance_ohm);
    }();

#ifdef MOTEUS_PERFORMANCE_MEASURE
    status_.dwt.control_done_pos = DWT->CYCCNT;
#endif

    ISR_DoCurrent(sin_cos, d_A, q_A);
  }

  void ISR_DoStayWithinBounds(const SinCos& sin_cos, CommandData* data) {
    const auto target_position = [&]() MOTEUS_CCM_ATTRIBUTE -> std::optional<float> {
      if (!std::isnan(data->bounds_min) &&
          status_.unwrapped_position < data->bounds_min) {
        return data->bounds_min;
      }
      if (!std::isnan(data->bounds_max) &&
          status_.unwrapped_position > data->bounds_max) {
        return data->bounds_max;
      }
      return {};
    }();

    if (!target_position) {
      status_.pid_position.Clear();
      status_.control_position = std::numeric_limits<float>::quiet_NaN();

      // In this region, we still apply feedforward torques if they
      // are present.
      const float limited_torque_Nm =
          Limit(data->feedforward_Nm, -data->max_torque_Nm, data->max_torque_Nm);
      control_.torque_Nm = limited_torque_Nm;
      const float limited_q_A =
          torque_to_current(limited_torque_Nm * motor_.unwrapped_position_scale);

      ISR_DoCurrent(sin_cos, 0.0f, limited_q_A);
      return;
    }

    // Control position to whichever bound we are currently violating.
    PID::ApplyOptions apply_options;
    apply_options.kp_scale = data->kp_scale;
    apply_options.kd_scale = data->kd_scale;

    data->position = *target_position;
    data->velocity = 0.0;

    ISR_DoPositionCommon(
        sin_cos, data, apply_options,
        data->max_torque_Nm, data->feedforward_Nm, 0.0f);
  }

  float LimitPwm(float in) MOTEUS_CCM_ATTRIBUTE {
    // We can't go full duty cycle or we wouldn't have time to sample
    // the current.
    return Limit(in, kMinPwm, kMaxPwm);
  }

  const Options options_;
  MillisecondTimer* const ms_timer_;
  AS5047* const position_sensor_;
  MotorDriver* const motor_driver_;

  Motor motor_;
  Config config_;
  PositionConfig position_config_;
  TIM_TypeDef* timer_ = nullptr;
  volatile uint32_t* timer_sr_ = nullptr;
  volatile uint32_t* timer_cr1_ = nullptr;
  ADC_TypeDef* const adc1_ = ADC1;
  ADC_TypeDef* const adc2_ = ADC2;
  ADC_TypeDef* const adc3_ = ADC3;
#if defined(TARGET_STM32G4)
  ADC_TypeDef* const adc4_ = ADC4;
  ADC_TypeDef* const adc5_ = ADC5;
  ADC_Common_TypeDef* const adc12_common_ = ADC12_COMMON;
  ADC_Common_TypeDef* const adc345_common_ = ADC345_COMMON;
#endif
  DAC_TypeDef* const dac_ = DAC;

  // We create these to initialize our pins as output and PWM mode,
  // but otherwise don't use them.
  PwmOut pwm1_;
  PwmOut pwm2_;
  PwmOut pwm3_;

  DigitalMonitor monitor1_;
  DigitalMonitor monitor2_;
  DigitalMonitor monitor3_;

  volatile uint32_t* pwm1_ccr_ = nullptr;
  volatile uint32_t* pwm2_ccr_ = nullptr;
  volatile uint32_t* pwm3_ccr_ = nullptr;

  AnalogIn current1_;
  AnalogIn current2_;
  AnalogIn current3_;
  AnalogIn vsense_;
  uint32_t vsense_sqr_ = {};
  AnalogIn tsense_;
  uint32_t tsense_sqr_ = {};
  AnalogIn msense_;
  uint32_t msense_sqr_ = {};

  AnalogOut debug_dac_;

  // This is just for debugging.
  DigitalOut debug_out_;
  DigitalOut debug_out2_;

  int32_t phase_ = 0;

  CommandData data_buffers_[2] = {};

  // CommandData has its data updated to the ISR by first writing the
  // new command into (*next_data_) and then swapping it with
  // current_data_.
  CommandData* volatile current_data_{&data_buffers_[0]};
  CommandData* volatile next_data_{&data_buffers_[1]};

  // This copy of CommandData exists solely for telemetry, and should
  // never be read by an ISR.
  CommandData telemetry_data_;

  // These values should only be modified from within the ISR.
  mjlib::base::WindowedAverage<
    int16_t, kMaxVelocityFilter, int32_t> velocity_filter_;
  Status status_;
  Control control_;
  uint32_t calibrate_adc1_ = 0;
  uint32_t calibrate_adc2_ = 0;
  uint32_t calibrate_adc3_ = 0;
  uint16_t calibrate_count_ = 0;

  PID pid_d_{&config_.pid_dq, &status_.pid_d};
  PID pid_q_{&config_.pid_dq, &status_.pid_q};
  PID pid_position_{&config_.pid_position, &status_.pid_position};

  Stm32Serial debug_serial_;

  std::atomic<uint32_t> clock_;
  std::atomic<uint32_t> startup_count_{0};

  float torque_constant_ = 0.01f;
  int32_t position_constant_ = 0;
  float adc_scale_ = 0.0f;

  uint32_t pwm_counts_ = 0;
  Cordic cordic_;

  const uint8_t hw_rev_ = g_measured_hw_rev;

  static Impl* g_impl_;
};

BldcServo::Impl* BldcServo::Impl::g_impl_ = nullptr;

BldcServo::BldcServo(micro::Pool* pool,
                     micro::PersistentConfig* persistent_config,
                     micro::TelemetryManager* telemetry_manager,
                     MillisecondTimer* millisecond_timer,
                     AS5047* position_sensor,
                     MotorDriver* motor_driver,
                     const Options& options)
    : impl_(pool,
            persistent_config, telemetry_manager,
            millisecond_timer, position_sensor, motor_driver,
            options) {}
BldcServo::~BldcServo() {}

void BldcServo::Start() {
  impl_->Start();
}

void BldcServo::PollMillisecond() {
  impl_->PollMillisecond();
}

void BldcServo::Command(const CommandData& data) {
  impl_->Command(data);
}

const BldcServo::Status& BldcServo::status() const {
  return impl_->status();
}

const BldcServo::Config& BldcServo::config() const {
  return impl_->config();
}

const BldcServo::Control& BldcServo::control() const {
  return impl_->control();
}

const BldcServo::Motor& BldcServo::motor() const {
  return impl_->motor();
}

uint32_t BldcServo::clock() const {
  return impl_->clock();
}

}

#include "builtin.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <string.h>
#include <util/delay.h>

static inline void
interrupts_enable(void)
{
  sei();
}

static inline void
interrupts_disable(void)
{
  cli();
}

static inline void
delay_us(u32 value)
{
  _delay_us(value);
}

static inline void
delay_ms(u32 value)
{
  _delay_ms(value);
}

// GPIO

static inline void
gpio_set_mode_input(volatile u8 *port, u8 pin)
{
  *port &= ~(1 << pin);
}

static inline void
gpio_set_mode_output(volatile u8 *port, u8 pin)
{
  *port |= (1 << pin);
}

static inline void
gpio_write_low(volatile u8 *port, u8 pin)
{
  *port &= ~(1 << pin);
}

static inline void
gpio_write_height(volatile u8 *port, u8 pin)
{
  *port |= (1 << pin);
}

static inline u8
gpio_read(volatile u8 *port, u8 pin)
{
  return *port & (1 << pin);
}

// Time

#define SECONDS(value) ((u32)value * 1000UL)
#define MINUTES(value) ((u32)value * 60UL * 1000UL)

typedef struct Timer32 {
  bool wait_done, sleep_pending;
  u32  ticks;
} Timer32;

static volatile u32 s_ticks;

static inline u32
get_ticks(void)
{
  return s_ticks;
}

static inline void
timer_reset(Timer32 *timer)
{
  memset(timer, 0, sizeof(Timer32));
}

static inline bool
timer_expired(u32 *ticks, u32 period, u32 now_ticks)
{
  if (ticks == 0) {
    return false;
  }

  // Если первый опрос, установить время завершения
  if (*ticks == 0) {
    *ticks = now_ticks + period;
  }

  if (now_ticks < *ticks) {
    return false;
  }

  *ticks = now_ticks + period;

  return true;
}

static inline bool
timer_expired_ext(Timer32 *self, u32 wait, u32 period, u32 sleep_duration,
                  u32 now_ticks)
{
  if (self == 0) {
    return false;
  }

  if (!self->wait_done) {
    // Если первый опрос, установить время завершения
    if (self->ticks == 0) {
      self->ticks = now_ticks + wait;
      return false;
    }

    if (now_ticks < self->ticks) {
      return false;
    }

    self->wait_done = true;
    self->ticks     = now_ticks + period;
  }

  // Засыпание
  if (self->sleep_pending && now_ticks <= self->ticks) {
    return false;
  } else if (self->sleep_pending) {
    self->sleep_pending = false;
    self->ticks         = now_ticks + period;
  }

  // Работа
  if (self->ticks >= now_ticks) {
    return true;
  }

  self->sleep_pending = true;
  self->ticks         = now_ticks + sleep_duration;

  return false;
}

#define ARRAY_COUNT(a) (sizeof((a)) / sizeof(*(a)))

#define CLAMP(value, min, max)                                                \
  ((value <= min) ? min : (value >= max) ? max : value)
#define CLAMP_TOP(value, max) ((value >= max) ? max : value)

// Пины для кнопок
#define PIN_BUTTON_DOWN PB5
#define PIN_BUTTON_MENU PB6
#define PIN_BUTTON_UP   PB7
#define PIN_BUTTON_READ PINB
#define PIN_BUTTON_DDR  DDRB
#define PIN_BUTTON_PORT PORTB

// Пины для индикации
#define PIN_LED_STOP     PC0
#define PIN_LED_RASTOPKA PC1
#define PIN_LED_CONTROL  PC2
#define PIN_LED_ALARM    PC3
#define PIN_LED_PUMP     PC4
#define PIN_LED_FAN      PC5
#define PIN_LED_DDR      DDRC
#define PIN_LED_PORT     PORTC

// Пины для термодатчика
#define PIN_OW      PB0
#define PIN_OW_READ PINB
#define PIN_OW_DDR  DDRB
#define PIN_OW_PORT PORTB

// Пины для вентилятора
#define PIN_FAN      PB1
#define PIN_FAN_DDR  DDRB
#define PIN_FAN_PORT PORTB

// Массив значениий для семисегментного индикатора
static char display_segment_numbers[12] = {
  0b11111100, // 0
  0b01100000, // 1
  0b11011010, // 2
  0b11110010, // 3
  0b01100110, // 4
  0b10110110, // 5
  0b10111110, // 6
  0b11100000, // 7
  0b11111110, // 8
  0b11110110, // 9
  0b00000010, // -
  0b00000000, // пусто
};

static char display_segment_menu[10][2] = {
  { 0b10011100, 0b11001111 }, // CP
  { 0b11001110, 0b11001111 }, // PP
  { 0b11111100, 0b00111111 }, // Ob
  { 0b11111100, 0b11001111 }, // OP
  { 0b00011110, 0b11001111 }, // TP
  { 0b01101110, 0b00001101 }, // HI
  { 0b00011110, 0b11111101 }, // TO
  { 0b00011110, 0b01111101 }, // TU
  { 0b00111110, 0b01111101 }, // bU
  { 0b01111100, 0b10001111 }, // UF
};

typedef enum Error {
  Error_None             = 0,
  Error_Temp_Sensor      = 1 << 0, // 00000001
  Error_Low_Temperature  = 1 << 1, // 00000010
  Error_High_Temperature = 1 << 2, // 00000100
} Error;

typedef enum Mode {
  MODE_STOP,
  MODE_RASTOPKA,
  MODE_CONTROL,
} Mode;

typedef enum State {
  STATE_HOME = 0,
  STATE_MENU,
  STATE_MENU_TEMP_CHANGE,
  STATE_MENU_PARAMETERS,
  STATE_ALARM,
} State;

typedef enum Button {
  BUTTON_UP = 0,
  BUTTON_MENU,
  BUTTON_DOWN,
  BUTTON_COUNT,
} Button;

typedef enum Parameters {
  CP = 0,
  PP,
  OB,
  OP,
  TP,
  HI,
  TO,
  TU,
  BU,
  UF,
} Parameters;

typedef enum Temp_Step {
  Temp_Step_Convert,
  Temp_Step_Read,
  Temp_Step_Done,
} Temp_Step;

typedef struct Temp_Ctx {
  // u32 temp_point; // Переменная для дробного значения температуры
  u32       last_temp, temp;
  Temp_Step step;
  Timer32   timer;
} Temp_Ctx;

typedef struct Option {
  u8 value, min, max;
} Option;

#define OPTIONS_MAX 10

// Структура для хранения параметров меню
typedef union Options {
  struct {
    Option fan_work_duration;  // CP - ПРОДУВКА РАБОТА (Сек.)
    Option fan_pause_duration; // PP - ПРОДУВКА ПЕРЕРЫВ (Мин.)
    Option fan_speed; // Ob - СКОРОСТЬ ОБОРОТОВ ВЕНТИЛЯТОРА
    Option fan_power_during_ventilation; // OP - ОБОРОТЫ ВЕНТИЛЯТОРА ВО ВРЕМЯ
                                         // ПРОДУВКИ
    Option pump_connection_temperature; // ТЕМПЕРАТУРА ПОДКЛЮЧЕНИЯ НАСОСА ЦО
    Option hysteresis;          // HI - ГИСТЕРЕЗИС
    Option fan_power_reduction; // tO – УМЕНЬШЕНИЕ СИЛЫ ПРОДУВКИ
    Option controller_shutdown_temperature; // tU – ТЕМПЕРАТУРА ОТКЛЮЧЕНИЯ
                                            // КОНТРОЛЛЕРА
    Option
        sound_signal_enabled; // bU – ВКЛЮЧЕНИЕ И ОТКЛЮЧЕНИЕ ЗВУКОВОГО СИГНАЛА
    Option factory_settings;  // Uf – ЗАВОДСКИЕ НАСТРОЙКИ
  };

  Option e[OPTIONS_MAX];
} Options; // 10-bytes

// Глобальные переменные
static bool display_enable = true;

static u8   menu_idx               = CP;
static bool timer_out_menu_enabled = false;

static Error   error_flags = Error_None;
static Mode    mode        = MODE_STOP;
static State   last_state, state = STATE_HOME;
static Options options;
static Option  current_option1, current_option2;
static Option  option_temp_target;

// Temp
static Temp_Ctx temp_ctx;

static u8 buttons[BUTTON_COUNT];
static u8 last_buttons[BUTTON_COUNT];

// Прототипы функций
static void init_io(void);
static bool get_temp(Temp_Ctx *self);
static void display_menu(u8 display1, u8 display2);
static void handle_buttons(void);
static void start_alarm(void);
static void stop_alarm(void);

static inline void
change_state(u8 new_state)
{
  last_state = state;
  state      = new_state;
}

static inline void
restore_last_state(void)
{
  state = last_state;
}

static void options_default(void);
static void options_change_params(i8 value);
static void options_save(void);
static void options_load(void);

static u8 button_pressed(u8 code);
static u8 button_released(u8 code);
static u8 button_down(u8 code);

static u8   ow_reset(void);
static u8   ow_read(void);
static u8   ow_read_bit(void);
static void ow_send_bit(u8 bit);
static void ow_send(u8 data);
static bool ow_skip(void);
static u8   ow_crc_update(u8 crc, u8 byte);

#define LEDS_MAX 6

typedef enum Leds {
  Leds_Stop     = PIN_LED_STOP,
  Leds_Rastopka = PIN_LED_RASTOPKA,
  Leds_Control  = PIN_LED_CONTROL,
  Leds_Alarm    = PIN_LED_ALARM,
  Leds_Pump     = PIN_LED_PUMP,
  Leds_Fan      = PIN_LED_FAN
} Leds;

static bool leds_list[LEDS_MAX];

static void leds_init(void);
static void leds_display(Leds led);
static void leds_change(Leds led, bool enable);
static void leds_off(void);

static Timer32 timer_cp;
static Timer32 timer_pp;
static Timer32 timer_controller_shutdown_temperature;
static Timer32 timer_menu;
static Timer32 timer_temp_alarm;
static Timer32 timer_ow_alarm;
static Timer32 timer_in_menu;
static Timer32 timer_out_menu;

static inline void menu_button(u8 code, i8 value);
static inline void menu_parameters_button(u8 code, i8 value);
static inline void menu_change_temp_button(u8 code, i8 value);

// System

static inline void
system_tick_init(void)
{
  // Настройка таймера 0 для глобального таймера с периодом прерывания в 1 мс
  {
#if 1
    // Set prescaler to 1024
    // Timer Overflow Period = Prescaler Value * (1 / CPU Frequency)
    // Prescaler Value = Timer Overflow Period * CPU Frequency
    // Since Timer0 is an 8-bit timer and has prescaler options of 1, 8, 64,
    // 256, or 1024, the closest prescaler value that makes the timer overflow
    // every 1ms is 1024.

    // Set prescaler to 1024
    TCCR0 |= (1 << CS01);

    // Enable overflow interrupt
    TIMSK |= (1 << TOIE0);
#endif
  }

  // Настройка таймера 1 для ШИМ
  {
#if 1
    // Настройка таймера 1 в режиме Fast PWM, TOP = 0xFF
    TCCR1A |= (1 << WGM10);
    TCCR1B |= (1 << WGM12) | (1 << CS11); // Предделитель = 8

    // Установка начального значения для регистра сравнения (скважность)
    OCR1A = 255;

    gpio_set_mode_output(&PIN_FAN_DDR, PIN_FAN);
#endif
  }

  // Настройка таймера 2 для глобального таймера с периодом прерывания в 1 мс
  {
    // Настройка предделителя и запуск таймера
    TCCR2 = (1 << WGM21) | (1 << CS22) | (1 << CS21)
            | (1 << CS20); // Prescaler 1024
    TIMSK |= (1 << OCIE2);
    OCR2 = 0; // 1ms for 1MHz clock
  }

  interrupts_enable();
}

static inline void
fan_start(void)
{
  leds_change(Leds_Control, false);
  leds_change(Leds_Rastopka, true);
  leds_change(Leds_Fan, true);

  // Включение ШИМ
  TCCR1A |= (1 << COM1A1);
}

static inline void
fan_stop(void)
{
  leds_change(Leds_Fan, false);

  // Выключение ШИМ
  TCCR1A &= ~(1 << COM1A1);
}

int
main(void)
{
#if 0
  u16 i;
  for (i = 0; i < 512; i++) {
    eeprom_write_byte((u8 *)i, 0);
  }

  options_default();
  options_save();

  return 0;
#endif
  system_tick_init();

  init_io();
  leds_init();

  options_default();
  options_load();

  do {
    static u8 rep = 0;
    if (!ow_reset()) {
      if (timer_expired_ext(&timer_ow_alarm, SECONDS(1), 0, 0, s_ticks)) {
        if (rep >= 5) {
          rep = 0;
          if (!ow_reset()) {
            error_flags = Error_Temp_Sensor;
            start_alarm();
            timer_reset(&timer_ow_alarm);
          }
        }
        rep -= 1;
      }
    } else {
      rep = 0;
      timer_reset(&timer_ow_alarm);
    }

  } while (timer_expired_ext(&timer_ow_alarm, 0, SECONDS(1), 0, s_ticks));

  timer_reset(&timer_ow_alarm);

  for (;;) {
    if (timer_expired_ext(&timer_ow_alarm, 0, 0, SECONDS(1), s_ticks)) {
      static u8 rep = 0;
      if (!ow_reset() && state != STATE_ALARM) {
        if (rep >= 5) {
          rep = 0;
          if (!ow_reset()) {
            error_flags = Error_Temp_Sensor;
            start_alarm();
            timer_reset(&timer_ow_alarm);
          }
        }
        rep -= 1;
      } else {
        rep = 0;
        timer_reset(&timer_ow_alarm);
      }
    }

    {
      static Timer32 timer;
      if (timer_expired_ext(&timer, 0, 0, 10, s_ticks)) {
        handle_buttons();
        current_option1.value = options.e[menu_idx].value % 100 / 10;
        current_option2.value = options.e[menu_idx].value % 10;
      }
    }

    if (timer_out_menu_enabled
        && timer_expired_ext(&timer_out_menu, SECONDS(5), 0, 0, s_ticks)
        && state != STATE_HOME) {
      change_state(STATE_HOME);
      if (last_state == STATE_MENU_TEMP_CHANGE
          || last_state == STATE_MENU_PARAMETERS || last_state == STATE_MENU) {
        last_state = STATE_HOME;
      }

      timer_out_menu_enabled = false;
      timer_reset(&timer_out_menu);

      display_enable = false;
      gpio_write_low(&PORTB, PB3);
      gpio_write_low(&PORTB, PB2);
      options_save();
      display_enable = true;

      // PWM
      {
        OCR1A = (options.fan_speed.value - 0) * (255 - 0) / (99 - 0) + 0;
      }
    }

    get_temp(&temp_ctx);

#if 1
    if (state == STATE_ALARM) {
      if (options.sound_signal_enabled.value) {
        // ...
      }
    }

    if (state == STATE_HOME && last_state == STATE_ALARM) {
      stop_alarm();
    }

    if (state != STATE_ALARM) {
      if (state == STATE_MENU_TEMP_CHANGE) {
        if (timer_expired_ext(&timer_menu, 0, 0, 250, s_ticks)) {
          display_enable ^= 1;
        }
      } else {
        display_enable = 1;
      }

      if (state == STATE_HOME) {
        if (options.factory_settings.value == 1) {
          options_default();

          display_enable = false;
          gpio_write_low(&PORTB, PB3);
          gpio_write_low(&PORTB, PB2);
          options_save();
          display_enable = true;

          // PWM
          {
            OCR1A = (options.fan_speed.value - 0) * (255 - 0) / (99 - 0) + 0;
          }
        }
      }

      if (mode != MODE_STOP) {
        leds_change(Leds_Stop, false);

        if (temp_ctx.temp > 90) {
          if (timer_expired_ext(&timer_temp_alarm, SECONDS(5), 0, 0,
                                s_ticks)) {
            error_flags = Error_High_Temperature;
            start_alarm();
            timer_reset(&timer_temp_alarm);
            continue;
          }
        } else if (temp_ctx.temp < 90) {
          timer_reset(&timer_temp_alarm);
        }

        if (options.controller_shutdown_temperature.value > temp_ctx.temp) {
          if (timer_expired_ext(&timer_controller_shutdown_temperature,
                                MINUTES(5), 0, 0, s_ticks)) {
            error_flags = Error_Low_Temperature;
            start_alarm();
            timer_reset(&timer_controller_shutdown_temperature);
            continue;
          }
        }

        if (options.controller_shutdown_temperature.value < temp_ctx.temp) {
          timer_reset(&timer_controller_shutdown_temperature);
        }

        // Алгоритм работы
        if (temp_ctx.temp < 35) {
          // Вентилятор начнет работу в ручном режиме.
          mode = MODE_RASTOPKA;

          fan_start();

          timer_reset(&timer_cp);
          timer_reset(&timer_pp);
        } else {
          // Вентилятор начнет работу в автоматическом режиме.
          if (temp_ctx.temp
              >= option_temp_target.value + options.hysteresis.value) {
            mode = MODE_CONTROL;
            leds_change(Leds_Control, true);
            leds_change(Leds_Rastopka, false);

            if (!timer_pp.wait_done && timer_cp.wait_done) {
              timer_reset(&timer_cp);
            }

            if (timer_expired_ext(
                    &timer_pp, MINUTES(options.fan_pause_duration.value),
                    MINUTES(options.fan_pause_duration.value),
                    MINUTES(options.fan_pause_duration.value), s_ticks)) {
              if (timer_expired_ext(
                      &timer_cp, 0, SECONDS(options.fan_work_duration.value),
                      SECONDS(options.fan_work_duration.value), s_ticks)) {
                fan_start();
              } else {
                fan_stop();
              }
            } else {
              fan_stop();
            }
          } else if (temp_ctx.temp
                     <= option_temp_target.value - options.hysteresis.value) {
            mode = MODE_RASTOPKA;
            leds_change(Leds_Rastopka, true);
            leds_change(Leds_Control, false);

            if (timer_pp.wait_done) {
              timer_reset(&timer_cp);
              timer_reset(&timer_pp);
            }

            if (timer_expired_ext(
                    &timer_cp, 0, SECONDS(options.fan_work_duration.value),
                    SECONDS(options.fan_work_duration.value), s_ticks)) {
              fan_start();
            } else {
              fan_stop();
            }
          }
        }

        if (temp_ctx.temp >= options.pump_connection_temperature.value) {
          leds_change(Leds_Pump, true);
        } else {
          leds_change(Leds_Pump, false);
        }
      }
    }
#endif

    {
      static Timer32 timer;
      if (timer_expired_ext(&timer, 0, 0, SECONDS(1), s_ticks)) {
        leds_display(Leds_Stop);
        leds_display(Leds_Rastopka);
        leds_display(Leds_Control);
        leds_display(Leds_Alarm);
        leds_display(Leds_Pump);
        leds_display(Leds_Fan);
      }
    }
  }

  return 0;
}

void
init_io(void)
{
  gpio_set_mode_input(&PIN_BUTTON_DDR, PIN_BUTTON_DOWN);
  gpio_set_mode_input(&PIN_BUTTON_DDR, PIN_BUTTON_MENU);
  gpio_set_mode_input(&PIN_BUTTON_DDR, PIN_BUTTON_UP);

  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_STOP);
  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_RASTOPKA);
  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_CONTROL);
  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_ALARM);
  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_PUMP);
  gpio_set_mode_output(&PIN_LED_DDR, PIN_LED_FAN);

  gpio_set_mode_output(&DDRB, PB2);
  gpio_set_mode_output(&DDRB, PB3);

  gpio_set_mode_output(&DDRD, PD0);
  gpio_set_mode_output(&DDRD, PD1);
  gpio_set_mode_output(&DDRD, PD2);
  gpio_set_mode_output(&DDRD, PD3);
  gpio_set_mode_output(&DDRD, PD4);
  gpio_set_mode_output(&DDRD, PD5);
  gpio_set_mode_output(&DDRD, PD6);
  gpio_set_mode_output(&DDRD, PD7);
}

bool
get_temp(Temp_Ctx *self)
{
  bool res = false;

  self->last_temp = self->temp;

  self->step = self->step == Temp_Step_Done ? Temp_Step_Convert : self->step;

  switch (self->step) {
  case Temp_Step_Convert: {
    if (ow_reset()) {
      ow_send(0xCC); // Проверка кода датчика
      ow_send(0x44); // Запуск температурного преобразования

      timer_reset(&self->timer);
      self->step = Temp_Step_Read;
    }
  } break;

  case Temp_Step_Read: {
    if (timer_expired_ext(&self->timer, SECONDS(1), 0, 0, s_ticks)) {
      if (ow_reset()) {
        u8 crc = 0;
        u8 scratchpad[8];

        ow_send(0xCC); // Проверка кода датчика
        ow_send(0xBE); // Считываем содержимое ОЗУ

        u8 i = 0;
        for (i = 0; i < 8; i++) {
          u8 b          = ow_read();
          scratchpad[i] = b;
          crc           = ow_crc_update(crc, b);
        }
        if (ow_read() == crc) {
          self->temp = ((scratchpad[1] << 4) & 0x70) | (scratchpad[0] >> 4);
          res        = true;
        }
#if 0
          temp = (Temp_LSB & 0x0F);
          temp_point = temp * 625 / 1000; // Точность
          темпер.преобразования(0.0625)
#endif
      }

      self->step = Temp_Step_Done;
    }
  } break;

  default:
    break;
  }

  return res;
}

void
display_menu(u8 display1, u8 display2)
{
  static u8 display_idx = 0;

  if (!display_enable) {
    gpio_write_low(&PORTB, PB3);
    gpio_write_low(&PORTB, PB2);
    return;
  }

  switch (display_idx) {
  case 0:
    gpio_write_low(&PORTB, PB3);
    gpio_write_height(&PORTB, PB2);
    PORTD       = ~(display2);
    display_idx = 1;
    break;
  case 1:
    gpio_write_low(&PORTB, PB2);
    gpio_write_height(&PORTB, PB3);
    PORTD       = ~(display1);
    display_idx = 0;
    break;
  default:
    break;
  }
}

void
menu_button(u8 code, i8 value)
{
  static Timer32 timer;

  if (button_pressed(code)) {
    timer_reset(&timer_out_menu);

    menu_idx
        = CLAMP(menu_idx + value == UINT8_MAX ? 0 : menu_idx + value, 0, 9);
  }

  if (button_released(code)) {
    timer_reset(&timer);
  }

  if (button_down(code)) {
    timer_reset(&timer_out_menu);

    if (timer_expired_ext(&timer, 500, 0, 50, s_ticks)) {
      menu_idx
          = CLAMP(menu_idx + value == UINT8_MAX ? 0 : menu_idx + value, 0, 9);
    }
  }
}

void
menu_parameters_button(u8 code, i8 value)
{
  static Timer32 timer;

  if (button_pressed(code)) {
    timer_reset(&timer_out_menu);

    options_change_params(value);
  }

  if (button_released(code)) {
    timer_reset(&timer);
  }

  if (button_down(code)) {
    timer_reset(&timer_out_menu);

    if (timer_expired_ext(&timer, 500, 0, 10, s_ticks)) {
      options_change_params(value);
    }
  }
}

void
menu_change_temp_button(u8 code, i8 value)
{
  static Timer32 timer;

  if (button_pressed(code)) {
    timer_reset(&timer_out_menu);

    option_temp_target.value
        = CLAMP(option_temp_target.value + value == UINT8_MAX
                    ? 0
                    : option_temp_target.value + value,
                option_temp_target.min, option_temp_target.max);
    change_state(STATE_MENU_TEMP_CHANGE);
  }

  if (button_released(code)) {
    timer_reset(&timer);
  }

  if (button_down(code)) {
    timer_reset(&timer_out_menu);

    if (timer_expired_ext(&timer, 500, 0, 10, s_ticks)) {
      option_temp_target.value
          = CLAMP(option_temp_target.value + value == UINT8_MAX
                      ? 0
                      : option_temp_target.value + value,
                  option_temp_target.min, option_temp_target.max);
    }
  }
}

void
handle_buttons(void)
{
  memcpy(&last_buttons, &buttons, sizeof(last_buttons));

  buttons[BUTTON_UP]   = PIN_BUTTON_READ & (1 << PIN_BUTTON_UP);
  buttons[BUTTON_MENU] = PIN_BUTTON_READ & (1 << PIN_BUTTON_MENU);
  buttons[BUTTON_DOWN] = PIN_BUTTON_READ & (1 << PIN_BUTTON_DOWN);

  switch (state) {
  case STATE_HOME: {
    if (button_pressed(BUTTON_MENU)) {
      // timer_out_menu_enabled = true;
      // timer_reset(&timer_out_menu);
    } else if (button_down(BUTTON_MENU)) {
      if (timer_expired_ext(&timer_in_menu, SECONDS(2), 0, 0, s_ticks)) {
        change_state(STATE_MENU);
        timer_reset(&timer_in_menu);
      }
    } else if (button_released(BUTTON_MENU)) {
      timer_reset(&timer_in_menu);
      timer_reset(&timer_temp_alarm);

      if (last_state == STATE_HOME || last_state == STATE_ALARM) {
        if (mode == MODE_STOP) {
          mode = MODE_RASTOPKA;
          leds_off();
          leds_change(Leds_Rastopka, true);
          fan_stop();
        } else if (mode != MODE_STOP) {
          mode = MODE_STOP;
          leds_off();
          leds_change(Leds_Stop, true);
          fan_stop();
        }
        break;
      } else if (last_state == STATE_MENU_TEMP_CHANGE
                 || last_state == STATE_ALARM
                 || last_state == STATE_MENU_PARAMETERS
                 || last_state == STATE_MENU) {
        last_state = STATE_HOME;
      }
    }

    menu_change_temp_button(BUTTON_UP, 1);
    menu_change_temp_button(BUTTON_DOWN, -1);
  } break;

  case STATE_MENU: {
    timer_out_menu_enabled = true;

    if (button_pressed(BUTTON_MENU)) {
      timer_reset(&timer_out_menu);
      change_state(STATE_MENU_PARAMETERS);
      break;
    }

    menu_button(BUTTON_UP, 1);
    menu_button(BUTTON_DOWN, -1);
  } break;

  case STATE_MENU_PARAMETERS: {
    if (button_pressed(BUTTON_MENU)) {
      timer_reset(&timer_out_menu);
      change_state(STATE_MENU);
      break;
    }

    menu_parameters_button(BUTTON_UP, 1);
    menu_parameters_button(BUTTON_DOWN, -1);
  } break;

  case STATE_MENU_TEMP_CHANGE: {
    timer_out_menu_enabled = true;

    if (button_pressed(BUTTON_MENU)) {
      timer_out_menu_enabled = false;
      timer_reset(&timer_out_menu);
      change_state(STATE_HOME);

      display_enable = false;
      gpio_write_low(&PORTB, PB3);
      gpio_write_low(&PORTB, PB2);
      options_save();
      display_enable = true;
      break;
    }

    menu_change_temp_button(BUTTON_UP, 1);
    menu_change_temp_button(BUTTON_DOWN, -1);
  } break;

  case STATE_ALARM: {
    if (button_released(BUTTON_MENU)) {
      stop_alarm();

      // disable sound alarm
      {
        // ...
      }
    }
    break;
  }
  }
}

void
start_alarm(void)
{
  mode = MODE_STOP;
  change_state(STATE_ALARM);
  leds_off();
  leds_change(Leds_Stop, true);
  leds_change(Leds_Alarm, true);
  timer_reset(&timer_cp);
  timer_reset(&timer_pp);
  timer_reset(&timer_controller_shutdown_temperature);
  timer_reset(&timer_menu);
  timer_reset(&timer_temp_alarm);
  timer_reset(&timer_ow_alarm);
  timer_reset(&timer_in_menu);
  timer_reset(&timer_out_menu);
  fan_stop();
  timer_out_menu_enabled = false;
  display_enable         = true;
}

void
stop_alarm(void)
{
  error_flags = Error_None;
  mode        = MODE_STOP;
  change_state(STATE_HOME);

  leds_change(Leds_Stop, true);
  leds_change(Leds_Rastopka, false);
  leds_change(Leds_Control, false);
  leds_change(Leds_Alarm, false);
  leds_change(Leds_Pump, false);
  leds_change(Leds_Fan, false);
}

void
options_default(void)
{
  options.fan_work_duration            = (Option){ 10, 5, 95 }; // 5-95 seconds
  options.fan_pause_duration           = (Option){ 3, 1, 99 };  // 1-99 minutes
  options.fan_speed                    = (Option){ 99, 30, 99 };    // 30-99
  options.fan_power_during_ventilation = (Option){ 90, 30, 99 };    // 30-99
  options.pump_connection_temperature  = (Option){ 40, 25, 70 };    // 25-70
  options.hysteresis                   = (Option){ 3, 1, 9 };       // 1-9
  options.fan_power_reduction          = (Option){ 5, 0, 10 };      // 0-10
  options.controller_shutdown_temperature = (Option){ 30, 25, 50 }; // 25-50
  options.sound_signal_enabled            = (Option){ 1, 0, 1 };    // 0-1
  options.factory_settings                = (Option){ 0, 0, 1 };    // 0-1
  option_temp_target                      = (Option){ 60, 35, 80 };
}

void
options_change_params(i8 value)
{
  options.e[menu_idx].value
      = CLAMP(options.e[menu_idx].value + value == UINT8_MAX
                  ? 0
                  : options.e[menu_idx].value + value,
              options.e[menu_idx].min, options.e[menu_idx].max);
}

void
options_save(void)
{
  return;
  u16 eeprom_pos = sizeof(u8);

  interrupts_disable();

  // u16 i;
  // for (i = 0; i < 512; i++) {
  //   eeprom_write_byte((u8 *)i, 0);
  // }

  eeprom_write_byte((u8 *)0x0, 1);

  eeprom_write_block((const void *)&options, (void *)eeprom_pos,
                     sizeof(Options));
  eeprom_pos += sizeof(Options);

  eeprom_write_block((const void *)&option_temp_target, (void *)eeprom_pos,
                     sizeof(Option));

  interrupts_enable();
}

void
options_load(void)
{
  return;
  // u16 i;
  // for (i = 0; i < 512; i++) {
  //   eeprom_write_byte((u8 *)i, 0);
  // }

  // return;

  interrupts_disable();

  u16 eeprom_pos = sizeof(u8);

  if (eeprom_read_byte((u8 *)0x0) == 1) {
    eeprom_read_block((void *)&options, (void *)eeprom_pos, sizeof(Options));
    eeprom_pos += sizeof(Options);

    eeprom_read_block((void *)&option_temp_target, (void *)eeprom_pos,
                      sizeof(Option));

    OCR1A = (options.fan_speed.value - 0) * (255 - 0) / (99 - 0) + 0;
  } else {
    options_default();
    options_save();
  }
  interrupts_enable();
}

u8
button_pressed(u8 code)
{
  return !last_buttons[code] && buttons[code];
}

u8
button_released(u8 code)
{
  return last_buttons[code] && !buttons[code];
}

u8
button_down(u8 code)
{
  return last_buttons[code] && buttons[code];
}

// Инициализация DS18B20
u8
ow_reset(void)
{
  bool res = false;

  res = ow_skip();
  if (res) {
    interrupts_disable();
    gpio_write_low(&PIN_OW_PORT, PIN_OW);
    gpio_set_mode_output(&PIN_OW_DDR, PIN_OW);
    _delay_us(640);
    gpio_set_mode_input(&PIN_OW_DDR, PIN_OW);
    _delay_us(80);
    interrupts_enable();
    res = !gpio_read(&PIN_OW_READ, PIN_OW);
    _delay_us(410);
  }

  return res;
}

u8
ow_read_bit(void)
{
  u8 res = 0;

  interrupts_disable();
  gpio_set_mode_output(&PIN_OW_DDR, PIN_OW);
  _delay_us(2);
  gpio_set_mode_input(&PIN_OW_DDR, PIN_OW);
  _delay_us(8);
  res = gpio_read(&PIN_OW_READ, PIN_OW);
  interrupts_enable();
  _delay_us(80);

  return res;
}

u8
ow_read(void)
{
  u8 r = 0;
  u8 p = 0;

  for (p = 8; p; p--) {
    r >>= 1;
    if (ow_read_bit()) {
      r |= 0x80;
    }
  }

  return r;
}

void
ow_send_bit(u8 bit)
{
  interrupts_disable();
  gpio_set_mode_output(&PIN_OW_DDR, PIN_OW);

  if (bit) {
    _delay_us(5);
    gpio_set_mode_input(&PIN_OW_DDR, PIN_OW);
    interrupts_enable();
    _delay_us(90);
  } else {
    _delay_us(90);
    gpio_set_mode_input(&PIN_OW_DDR, PIN_OW);
    interrupts_enable();
    _delay_us(5);
  }
}

void
ow_send(u8 byte)
{
  u8 p = 0;

  for (p = 8; p; p--) {
    ow_send_bit(byte & 1);
    byte >>= 1;
  }
}

bool
ow_skip(void)
{
  u8 retries = 80;

  interrupts_disable();
  gpio_set_mode_input(&PIN_OW_DDR, PIN_OW);
  interrupts_enable();

  do {
    if (--retries == 0) {
      return false;
    }
    _delay_us(1);
  } while (!gpio_read(&PIN_OW_READ, PIN_OW));

  return true;
}

// Обновляет значение контольной суммы crc применением всех бит байта b.
// Возвращает обновлённое значение контрольной суммы
u8
ow_crc_update(u8 crc, u8 byte)
{
  u8 p = 0;

  for (p = 8; p; p--) {
    crc = ((crc ^ byte) & 1) ? (crc >> 1) ^ 0b10001100 : (crc >> 1);
    byte >>= 1;
  }
  return crc;
}

void
leds_init(void)
{
  leds_off();
  leds_change(Leds_Stop, true);
}

void
leds_display(Leds led)
{
  if (leds_list[led]) {
    gpio_write_height(&PIN_LED_PORT, led);
  } else {
    gpio_write_low(&PIN_LED_PORT, led);
  }
}

void
leds_off(void)
{
  leds_list[Leds_Stop]     = false;
  leds_list[Leds_Rastopka] = false;
  leds_list[Leds_Control]  = false;
  leds_list[Leds_Alarm]    = false;
  leds_list[Leds_Pump]     = false;
  leds_list[Leds_Fan]      = false;
}

void
leds_change(Leds led, bool enable)
{
  leds_list[led] = enable;
}

ISR(TIMER0_OVF_vect)
{
  static u8 ticks = 0;

  if (ticks >= 4) {
    ticks = 0;

    switch (state) {
    case STATE_HOME:
      display_menu(display_segment_numbers[temp_ctx.temp % 100 / 10],
                   display_segment_numbers[temp_ctx.temp % 10]);
      break;
    case STATE_ALARM:
      if (error_flags == Error_Temp_Sensor) {
        display_menu(display_segment_numbers[10], display_segment_numbers[10]);
      } else {
        display_menu(display_segment_numbers[temp_ctx.temp % 100 / 10],
                     display_segment_numbers[temp_ctx.temp % 10]);
      }
      break;
    case STATE_MENU_TEMP_CHANGE:
      display_menu(
          display_segment_numbers[option_temp_target.value % 100 / 10],
          display_segment_numbers[option_temp_target.value % 10]);
      break;
    case STATE_MENU:
      display_menu(display_segment_menu[menu_idx][0],
                   display_segment_menu[menu_idx][1]);
      break;
    case STATE_MENU_PARAMETERS:
      display_menu(display_segment_numbers[current_option1.value],
                   display_segment_numbers[current_option2.value]);
      break;
    default:
      break;
    }
  }

  ticks += 1;
}

ISR(TIMER2_COMP_vect) { s_ticks += 1; }

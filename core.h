#ifndef CORE_H
#define CORE_H

#include <string.h>

#include "builtin.h"

#include <avr/interrupt.h>
#include <util/delay.h>

static inline void
enable_interrupts(void)
{
  sei();
}

static inline void
disable_interrupts(void)
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

#endif

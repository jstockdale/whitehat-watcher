#include "timer.h"

#include <stdio.h>
#include "esp_timer.h"

static bool running_ = false;
static int64_t base_ms_ = 0;  /* frozen accumulation while stopped */
static int64_t start_ms_ = 0; /* esp_timer ms timestamp of the running start */
/* Countdown mode: target duration in ms. 0 = stopwatch (count up). */
static int64_t countdown_ms_ = 0;
/* Latched when a countdown reaches zero. Cleared by toggle/reset. */
static bool finished_ = false;

void timer_init(void)
{
    running_ = false;
    base_ms_ = 0;
    start_ms_ = 0;
    countdown_ms_ = 0;
    finished_ = false;
}

void timer_toggle(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    finished_ = false;
    if (running_) {
        base_ms_ += now - start_ms_;
        /* Clamp countdown at zero. */
        if (countdown_ms_ > 0 && base_ms_ >= countdown_ms_) {
            base_ms_ = countdown_ms_;
        }
        running_ = false;
    } else {
        /* Restarting a finished countdown resets it to the full duration. */
        if (countdown_ms_ > 0 && base_ms_ >= countdown_ms_) {
            base_ms_ = 0;
        }
        start_ms_ = now;
        running_ = true;
    }
}

void timer_reset(void)
{
    running_ = false;
    base_ms_ = 0;
    finished_ = false;
    /* Keep the countdown target; only the progress resets. */
}

bool timer_running(void)
{
    return running_;
}

bool timer_is_countdown(void)
{
    return countdown_ms_ > 0;
}

bool timer_finished(void)
{
    return finished_;
}

void timer_set_countdown(int minutes)
{
    if (minutes < 0) {
        minutes = 0;
    }
    if (minutes > 99) {
        minutes = 99;
    }
    countdown_ms_ = (int64_t)minutes * 60000;
    running_ = false;
    base_ms_ = 0;
    finished_ = false;
}

int timer_countdown_minutes(void)
{
    return (int)(countdown_ms_ / 60000);
}

int64_t timer_elapsed_ms(void)
{
    int64_t elapsed;
    if (!running_) {
        elapsed = base_ms_;
    } else {
        elapsed = base_ms_ + (esp_timer_get_time() / 1000 - start_ms_);
    }
    if (countdown_ms_ > 0) {
        /* Countdown: return remaining time. Latch finished at zero. */
        int64_t remaining = countdown_ms_ - elapsed;
        if (remaining <= 0) {
            remaining = 0;
            if (running_) {
                running_ = false;
                base_ms_ = countdown_ms_;
                finished_ = true;
            }
        }
        return remaining;
    }
    return elapsed < 0 ? 0 : elapsed;
}

void timer_format(char *buf, size_t n)
{
    if (!buf || n == 0) {
        return;
    }
    int64_t ms = timer_elapsed_ms();
    if (ms < 0) {
        ms = 0;
    }
    int cs = (int)((ms / 10) % 100);
    int s = (int)((ms / 1000) % 60);
    int m = (int)(ms / 60000);
    if (m > 99) {
        m = 99; /* cap the readout; the counter keeps going */
    }
    snprintf(buf, n, "%02d:%02d.%02d", m, s, cs);
}

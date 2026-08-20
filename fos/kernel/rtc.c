/*
 * rtc.c — MC146818 CMOS RTC (ports 0x70/0x71).
 *
 * QEMU provides this by default. Reads wait for the update-in-progress
 * flag to clear. Writes set the SET bit so the clock is frozen first.
 */

#include "rtc.h"
#include "console.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SEC      0x00
#define RTC_MIN      0x02
#define RTC_HOUR     0x04
#define RTC_WEEKDAY  0x06
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_CENTURY  0x32

#define UIP     0x80
#define SET     0x80
#define DM_BIN  0x04
#define HOUR24  0x02
#define HOUR_PM 0x80

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, (uint8_t)(reg | 0x80));
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t value) {
    outb(CMOS_INDEX, (uint8_t)(reg | 0x80));
    outb(CMOS_DATA, value);
}

static void cmos_wait_stable(void) {
    int n = 0;

    while ((cmos_read(RTC_STATUS_A) & UIP) && n < 100000) {
        n++;
    }
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int is_leap(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap(year)) {
        return 29;
    }
    return days[month - 1];
}

static int rtc_valid(const fos_rtc_t *t) {
    if (!t) {
        return 0;
    }
    if (t->year < 1970 || t->year > 2099) {
        return 0;
    }
    if (t->month < 1 || t->month > 12) {
        return 0;
    }
    if (t->day < 1 || t->day > days_in_month(t->year, t->month)) {
        return 0;
    }
    if (t->hour > 23 || t->minute > 59 || t->second > 59) {
        return 0;
    }
    return 1;
}

int rtc_read(fos_rtc_t *out) {
    uint8_t stat_b;
    uint8_t sec, min, hour, day, month, year, century, weekday;
    int binary;
    int hour24;

    if (!out) {
        return -1;
    }

    cmos_wait_stable();
    sec = cmos_read(RTC_SEC);
    min = cmos_read(RTC_MIN);
    hour = cmos_read(RTC_HOUR);
    weekday = cmos_read(RTC_WEEKDAY);
    day = cmos_read(RTC_DAY);
    month = cmos_read(RTC_MONTH);
    year = cmos_read(RTC_YEAR);
    century = cmos_read(RTC_CENTURY);
    stat_b = cmos_read(RTC_STATUS_B);

    binary = (stat_b & DM_BIN) != 0;
    hour24 = (stat_b & HOUR24) != 0;

    if (!binary) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
        century = bcd_to_bin(century);
        weekday = bcd_to_bin(weekday);
    }

    if (!hour24) {
        int pm = (hour & HOUR_PM) != 0;
        uint8_t h = hour & 0x7F;
        if (!binary) {
            h = bcd_to_bin(h);
        }
        if (h == 12) {
            h = 0;
        }
        if (pm) {
            h = (uint8_t)(h + 12);
        }
        hour = h;
    } else if (!binary) {
        hour = bcd_to_bin(hour & 0x7F);
    }

    if (century < 19 || century > 20) {
        century = 20;
    }

    out->year = (uint16_t)(century * 100 + year);
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    out->weekday = weekday;
    return rtc_valid(out) ? 0 : -1;
}

int rtc_write(const fos_rtc_t *in) {
    uint8_t stat_b;
    uint8_t sec, min, hour, day, month, year, century, weekday;
    int binary;

    if (!rtc_valid(in)) {
        return -1;
    }

    cmos_wait_stable();
    stat_b = cmos_read(RTC_STATUS_B);
    binary = (stat_b & DM_BIN) != 0;

    sec = in->second;
    min = in->minute;
    hour = in->hour;
    day = in->day;
    month = in->month;
    year = (uint8_t)(in->year % 100);
    century = (uint8_t)(in->year / 100);
    weekday = in->weekday;
    if (weekday < 1 || weekday > 7) {
        weekday = cmos_read(RTC_WEEKDAY);
        if (!binary) {
            weekday = bcd_to_bin(weekday);
        }
        if (weekday < 1 || weekday > 7) {
            weekday = 1;
        }
    }

    if (!binary) {
        sec = bin_to_bcd(sec);
        min = bin_to_bcd(min);
        hour = bin_to_bcd(hour);
        day = bin_to_bcd(day);
        month = bin_to_bcd(month);
        year = bin_to_bcd(year);
        century = bin_to_bcd(century);
        weekday = bin_to_bcd(weekday);
    }

    cmos_write(RTC_STATUS_B, (uint8_t)(stat_b | SET | HOUR24));
    cmos_write(RTC_SEC, sec);
    cmos_write(RTC_MIN, min);
    cmos_write(RTC_HOUR, hour);
    cmos_write(RTC_WEEKDAY, weekday);
    cmos_write(RTC_DAY, day);
    cmos_write(RTC_MONTH, month);
    cmos_write(RTC_YEAR, year);
    cmos_write(RTC_CENTURY, century);
    cmos_write(RTC_STATUS_B, (uint8_t)((stat_b | HOUR24) & (uint8_t)~SET));
    return 0;
}

void rtc_print(void) {
    fos_rtc_t t;
    char buf[32];
    int n = 0;

    if (rtc_read(&t) != 0) {
        console_write_line("(RTC unreadable)");
        return;
    }

    buf[n++] = (char)('0' + t.year / 1000);
    buf[n++] = (char)('0' + (t.year / 100) % 10);
    buf[n++] = (char)('0' + (t.year / 10) % 10);
    buf[n++] = (char)('0' + t.year % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + t.month / 10);
    buf[n++] = (char)('0' + t.month % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + t.day / 10);
    buf[n++] = (char)('0' + t.day % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + t.hour / 10);
    buf[n++] = (char)('0' + t.hour % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + t.minute / 10);
    buf[n++] = (char)('0' + t.minute % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + t.second / 10);
    buf[n++] = (char)('0' + t.second % 10);
    buf[n] = 0;
    console_write_line(buf);
}

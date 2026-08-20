/*
 * date.c — Show or set the CMOS RTC.
 *
 *   date                         print local RTC time
 *   date YYYY-MM-DD HH:MM:SS     set the RTC
 */

#include "fos_api.h"

static void date_error(fos_api_t *api, const char *msg) {
    if (api->show_error) {
        api->show_error(msg);
    } else {
        api->write_line(msg);
    }
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_u(const char **pp, int digits, int *out) {
    const char *p = *pp;
    int v = 0;
    int i;

    for (i = 0; i < digits; i++) {
        if (!is_digit(p[i])) {
            return -1;
        }
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    *pp = p + digits;
    return 0;
}

static int expect(const char **pp, char c) {
    if (**pp != c) {
        return -1;
    }
    (*pp)++;
    return 0;
}

static int parse_datetime(const char *s, fos_rtc_t *out) {
    const char *p = s;
    int year, month, day, hour, minute, second;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (parse_u(&p, 4, &year) != 0 || expect(&p, '-') != 0) {
        return -1;
    }
    if (parse_u(&p, 2, &month) != 0 || expect(&p, '-') != 0) {
        return -1;
    }
    if (parse_u(&p, 2, &day) != 0) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (parse_u(&p, 2, &hour) != 0 || expect(&p, ':') != 0) {
        return -1;
    }
    if (parse_u(&p, 2, &minute) != 0 || expect(&p, ':') != 0) {
        return -1;
    }
    if (parse_u(&p, 2, &second) != 0) {
        return -1;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != 0) {
        return -1;
    }

    out->year = (uint16_t)year;
    out->month = (uint8_t)month;
    out->day = (uint8_t)day;
    out->hour = (uint8_t)hour;
    out->minute = (uint8_t)minute;
    out->second = (uint8_t)second;
    out->weekday = 0;
    return 0;
}

static void print_rtc(fos_api_t *api, const fos_rtc_t *t) {
    char buf[32];
    int n = 0;

    buf[n++] = (char)('0' + t->year / 1000);
    buf[n++] = (char)('0' + (t->year / 100) % 10);
    buf[n++] = (char)('0' + (t->year / 10) % 10);
    buf[n++] = (char)('0' + t->year % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + t->month / 10);
    buf[n++] = (char)('0' + t->month % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + t->day / 10);
    buf[n++] = (char)('0' + t->day % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + t->hour / 10);
    buf[n++] = (char)('0' + t->hour % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + t->minute / 10);
    buf[n++] = (char)('0' + t->minute % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + t->second / 10);
    buf[n++] = (char)('0' + t->second % 10);
    buf[n] = 0;
    api->write_line(buf);
}

static void print_uptime(fos_api_t *api) {
    uint64_t ms;
    char buf[40];
    int n = 0;
    uint64_t sec;
    uint64_t rem;

    if (!api->get_ticks_ms) {
        return;
    }
    ms = api->get_ticks_ms();
    sec = ms / 1000;
    rem = ms % 1000;

    buf[n++] = 'u';
    buf[n++] = 'p';
    buf[n++] = 't';
    buf[n++] = 'i';
    buf[n++] = 'm';
    buf[n++] = 'e';
    buf[n++] = ' ';
    if (sec == 0) {
        buf[n++] = '0';
    } else {
        char tmp[20];
        int t = 0;
        uint64_t v = sec;
        while (v) {
            tmp[t++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (t--) {
            buf[n++] = tmp[t];
        }
    }
    buf[n++] = '.';
    buf[n++] = (char)('0' + (rem / 100));
    buf[n++] = (char)('0' + (rem / 10) % 10);
    buf[n++] = (char)('0' + rem % 10);
    buf[n++] = 's';
    buf[n] = 0;
    api->write_line(buf);
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    fos_rtc_t t;

    if (!api->rtc_read || !api->rtc_write) {
        date_error(api, "DATE: RTC API missing — rebuild the kernel");
        return;
    }

    if (api->cmdline[0]) {
        if (parse_datetime(api->cmdline, &t) != 0) {
            api->write_line("usage: date");
            api->write_line("       date YYYY-MM-DD HH:MM:SS");
            return;
        }
        if (api->rtc_write(&t) != 0) {
            date_error(api, "DATE: invalid date/time");
            return;
        }
    }

    if (api->rtc_read(&t) != 0) {
        date_error(api, "DATE: RTC read failed");
        return;
    }
    print_rtc(api, &t);
    print_uptime(api);
}

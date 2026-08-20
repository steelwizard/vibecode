#pragma once

#include "fos_api.h"

int rtc_read(fos_rtc_t *out);
int rtc_write(const fos_rtc_t *in);
void rtc_print(void);

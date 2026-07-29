#include "RTC_NXP.h"

/* The PCF2131 calendar registers are UTC by firmware contract. libc mktime()
 * cannot be used here: it interprets its input as LOCAL civil time, so merely
 * installing Europe/Amsterdam's CEST rule made every RTC read/set appear one
 * hour early. Convert the calendar directly instead. Besides being independent
 * of process-global TZ/DST state, this keeps RTC->epoch and epoch->RTC exact
 * inverses for the chip's supported 2000..2099 range. */
static constexpr int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? -3 : 9)) + 2U) / 5U
                         + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe)
           - 719468LL;
}

static constexpr bool is_leap_year(int year)
{
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

static bool tm_to_utc_epoch(const struct tm *value, time_t *out)
{
    if (value == nullptr || out == nullptr) return false;

    const int year = value->tm_year + 1900;
    const int month = value->tm_mon + 1;
    static constexpr uint8_t month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        value->tm_hour < 0 || value->tm_hour > 23 ||
        value->tm_min < 0 || value->tm_min > 59 ||
        value->tm_sec < 0 || value->tm_sec > 59) {
        return false;
    }
    int max_day = month_days[month - 1];
    if (month == 2 && is_leap_year(year)) ++max_day;
    if (value->tm_mday < 1 || value->tm_mday > max_day) return false;

    const int64_t seconds = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(value->tm_mday))
                            * 86400LL + value->tm_hour * 3600LL
                            + value->tm_min * 60LL + value->tm_sec;
    *out = static_cast<time_t>(seconds);
    return static_cast<int64_t>(*out) == seconds;
}

static int utc_weekday(time_t epoch)
{
    /* 1970-01-01 was Thursday (4). The supported RTC range is post-epoch, but
     * retain a positive modulo so this helper stays correct if reused. */
    int64_t days = static_cast<int64_t>(epoch) / 86400LL;
    int weekday = static_cast<int>((days + 4LL) % 7LL);
    return weekday < 0 ? weekday + 7 : weekday;
}

/* Compile-time round-trip anchors: these values are unaffected by TZ, DST, or
 * the host running the build. 2024 also anchors the firmware clock-valid gate. */
static_assert(days_from_civil(1970, 1, 1) == 0, "UTC epoch anchor changed");
static_assert(days_from_civil(2024, 1, 1) * 86400LL == 1704067200LL,
              "UTC 2024 round-trip anchor changed");

PCF2131_base::PCF2131_base()
{
}

PCF2131_base::~PCF2131_base()
{
}

void PCF2131_base::begin(void)
{
    clear_error();
    (void)int_clear();
}

bool PCF2131_base::oscillator_stop(void)
{
    clear_error();
    const uint8_t seconds = _reg_r(Seconds);
    return (last_error() == ESP_OK) ? ((seconds & 0x80U) != 0U) : false;
}

time_t PCF2131_base::rtc_time(void)
{
    clear_error();

    struct tm now_tm = {};
    uint8_t bf[8] = {};

    _reg_r(_100th_Seconds, bf, sizeof(bf));
    if (last_error() != ESP_OK) {
        return (time_t)(-1);
    }

    /* Seconds register bit 7 is the oscillator-stop flag (OSF): set when the
     * oscillator has stopped (never-set or power-lost RTC), meaning the time is
     * invalid. Reject it so callers don't sync the system clock to a bogus value
     * — and mask it out of the BCD seconds regardless. */
    if (bf[1] & 0x80U) {
        set_error(ESP_ERR_INVALID_STATE);
        return (time_t)(-1);
    }

    now_tm.tm_sec = bcd2dec(bf[1] & 0x7FU);
    now_tm.tm_min = bcd2dec(bf[2]);
    now_tm.tm_hour = bcd2dec(bf[3]);
    now_tm.tm_mday = bcd2dec(bf[4]);
    now_tm.tm_mon = bcd2dec(bf[6]) - 1;
    now_tm.tm_year = bcd2dec(bf[7]) + 100;
    now_tm.tm_isdst = 0;

    time_t now = 0;
    if (!tm_to_utc_epoch(&now_tm, &now)) {
        set_error(ESP_FAIL);
        return (time_t)(-1);
    }

    return now;
}

void PCF2131_base::set(struct tm *now_tmp)
{
    clear_error();

    if (now_tmp == nullptr) {
        set_error(ESP_ERR_INVALID_ARG);
        return;
    }

    uint8_t bf[8] = {};

    time_t now_time = 0;
    if (!tm_to_utc_epoch(now_tmp, &now_time)) {
        set_error(ESP_ERR_INVALID_ARG);
        return;
    }

    bf[1] = dec2bcd(now_tmp->tm_sec);
    bf[2] = dec2bcd(now_tmp->tm_min);
    bf[3] = dec2bcd(now_tmp->tm_hour);
    bf[4] = dec2bcd(now_tmp->tm_mday);
    bf[6] = dec2bcd(now_tmp->tm_mon + 1);
    bf[7] = dec2bcd(now_tmp->tm_year - 100);

    bf[5] = dec2bcd(utc_weekday(now_time));

    _bit_op8(Control_1, static_cast<uint8_t>(~0x28U), 0x20U);
    _bit_op8(SR_Reset, static_cast<uint8_t>(~0x80U), 0x80U);
    _reg_w(_100th_Seconds, bf, sizeof(bf));
    _bit_op8(Control_1, static_cast<uint8_t>(~0x20U), 0x00U);
}

void PCF2131_base::alarm(alarm_setting digit, int val)
{
    alarm(digit, val, 0);
}

void PCF2131_base::alarm(alarm_setting digit, int val, int int_sel)
{
    clear_error();

    const int v = (val == 0x80) ? 0x80 : dec2bcd(static_cast<uint8_t>(val));
    _reg_w(static_cast<uint8_t>(Second_alarm + digit), static_cast<uint8_t>(v));
    _bit_op8(static_cast<uint8_t>(int_mask_reg[int_sel][0]), static_cast<uint8_t>(~0x04U), 0x00U);
    _bit_op8(Control_2, static_cast<uint8_t>(~0x02U), 0x02U);
}

void PCF2131_base::alarm_clear(void)
{
    clear_error();
    _bit_op8(Control_2, static_cast<uint8_t>(~0x10U), 0x00U);
}

void PCF2131_base::alarm_disable(void)
{
    clear_error();
    _bit_op8(Control_2, static_cast<uint8_t>(~0x02U), 0x00U);
}

void PCF2131_base::timestamp(int num, timestamp_setting ts_setting, int int_sel)
{
    clear_error();

    const int r_ofst = 7;
    const uint8_t fst = ts_setting ? 0x80U : 0x00U;

    num -= 1;

    const uint8_t reg = static_cast<uint8_t>(Timestp_ctl1 + (num * r_ofst));

    _bit_op8(reg, static_cast<uint8_t>(~0x80U), fst);
    _bit_op8(
        static_cast<uint8_t>(int_mask_reg[int_sel][1]),
        static_cast<uint8_t>(~(0x1U << (3 - num))),
        static_cast<uint8_t>(0x0U << (3 - num)));

    _bit_op8(Control_5, static_cast<uint8_t>(~(0x1U << (7 - num))), static_cast<uint8_t>(0x1U << (7 - num)));
}

time_t PCF2131_base::timestamp(int num)
{
    clear_error();

    const int r_ofst = 7;

    num -= 1;

    const uint8_t reg = static_cast<uint8_t>(Timestp_ctl1 + (num * r_ofst));
    uint8_t v[7] = {};

    _reg_r(reg, v, sizeof(v));
    if (last_error() != ESP_OK) {
        return (time_t)(-1);
    }

    struct tm ts_tm = {};

    ts_tm.tm_sec = bcd2dec(v[1]);
    ts_tm.tm_min = bcd2dec(v[2]);
    ts_tm.tm_hour = bcd2dec(v[3]);
    ts_tm.tm_mday = bcd2dec(v[4]);
    ts_tm.tm_mon = bcd2dec(v[5]) - 1;
    ts_tm.tm_year = bcd2dec(v[6]) + 100;
    ts_tm.tm_isdst = 0;

    time_t ts = 0;
    if (!tm_to_utc_epoch(&ts_tm, &ts)) {
        set_error(ESP_FAIL);
        return (time_t)(-1);
    }

    return ts;
}

uint8_t PCF2131_base::int_clear(void)
{
    uint8_t dummy[3] = {0};
    return int_clear(dummy);
}

uint8_t PCF2131_base::int_clear(uint8_t *rv)
{
    clear_error();

    uint8_t local_rv[3] = {0};
    if (rv == nullptr) {
        rv = local_rv;
    } else {
        rv[0] = 0;
        rv[1] = 0;
        rv[2] = 0;
    }

    _reg_r(Control_2, rv, 3);
    if (last_error() != ESP_OK) {
        return 0;
    }

    if (rv[0] & 0x90U) {
        _reg_w(Control_2, static_cast<uint8_t>(rv[0] & ~((rv[0] & 0x90U) | 0x49U)));
    }

    if (rv[1] & 0x08U) {
        _reg_w(Control_3, static_cast<uint8_t>(rv[1] & ~0x08U));
    }

    if (rv[2] & 0xF0U) {
        _reg_w(Control_4, static_cast<uint8_t>(rv[2] & ~(rv[2] & 0xF0U)));
    }

    return rv[0];
}

void PCF2131_base::periodic_interrupt_enable(periodic_int_select sel, int int_sel)
{
    clear_error();

    if (!sel) {
        _bit_op8(Control_1, static_cast<uint8_t>(~0x03U), 0x00U);
        _bit_op8(static_cast<uint8_t>(int_mask_reg[int_sel][0]), static_cast<uint8_t>(~0x30U), 0x30U);
        return;
    }

    const uint8_t v = (sel == EVERY_MINUTE) ? 0x02U : 0x01U;

    _bit_op8(Control_1, static_cast<uint8_t>(~0x03U), v);
    _bit_op8(static_cast<uint8_t>(int_mask_reg[int_sel][0]), static_cast<uint8_t>(~0x30U), static_cast<uint8_t>(~(v << 4)));
}

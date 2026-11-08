/*
 * time module (port of CPython 3.15 Modules/timemodule.c and the parts of
 * Python/pytime.c it uses, PSF-2.0).  POSIX build: clock_gettime clocks
 * (mach_absolute_time for monotonic/perf_counter on macOS like CPython),
 * struct_time with tm_zone/tm_gmtoff, strftime via the C library with the
 * result decoded as UTF-8/surrogateescape (the UTF-8 locale), strptime
 * delegated to _strptime._strptime_time.
 */
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#define QP_TIME_STRFTIME_FORMAT_CODES \
"Commonly used format codes:\n\
\n\
%Y  Year with century as a decimal number.\n\
%m  Month as a decimal number [01,12].\n\
%d  Day of the month as a decimal number [01,31].\n\
%H  Hour (24-hour clock) as a decimal number [00,23].\n\
%M  Minute as a decimal number [00,59].\n\
%S  Second as a decimal number [00,61].\n\
%z  Time zone offset from UTC.\n\
%a  Locale's abbreviated weekday name.\n\
%A  Locale's full weekday name.\n\
%b  Locale's abbreviated month name.\n\
%B  Locale's full month name.\n\
%c  Locale's appropriate date and time representation.\n\
%I  Hour (12-hour clock) as a decimal number [01,12].\n\
%p  Locale's equivalent of either AM or PM.\n\
\n\
Other codes may be available on your platform.  See documentation for\n\
the C library strftime function.\n"

static const char qp_time_doc_time[] =
"time() -> floating-point number\n\
\n\
Return the current time in seconds since the Epoch.\n\
Fractions of a second may be present if the system clock provides them.";

static const char qp_time_doc_time_ns[] =
"time_ns() -> int\n\
\n\
Return the current time in nanoseconds since the Epoch.";

static const char qp_time_doc_clock_settime[] =
"clock_settime(clk_id, time)\n\
\n\
Set the time of the specified clock clk_id.";

static const char qp_time_doc_clock_settime_ns[] =
"clock_settime_ns(clk_id, time)\n\
\n\
Set the time of the specified clock clk_id with nanoseconds.";

static const char qp_time_doc_clock_getres[] =
"clock_getres(clk_id) -> floating-point number\n\
\n\
Return the resolution (precision) of the specified clock clk_id.";

static const char qp_time_doc_pthread_getcpuclockid[] =
"pthread_getcpuclockid(thread_id) -> int\n\
\n\
Return the clk_id of a thread's CPU time clock.";

static const char qp_time_doc_sleep[] =
"sleep(seconds)\n\
\n\
Delay execution for a given number of seconds.  The argument may be\n\
a floating-point number for subsecond precision.";

static const char qp_time_doc_gmtime[] =
"gmtime([seconds]) -> (tm_year, tm_mon, tm_mday, tm_hour, tm_min,\n\
                       tm_sec, tm_wday, tm_yday, tm_isdst)\n\
\n\
Convert seconds since the Epoch to a time tuple expressing UTC (a.k.a.\n\
GMT).  When 'seconds' is not passed in, convert the current time instead.\n\
\n\
If the platform supports the tm_gmtoff and tm_zone, they are available as\n\
attributes only.";

static const char qp_time_doc_localtime[] =
"localtime([seconds]) -> (tm_year,tm_mon,tm_mday,tm_hour,tm_min,\n\
                          tm_sec,tm_wday,tm_yday,tm_isdst)\n\
\n\
Convert seconds since the Epoch to a time tuple expressing local time.\n\
When 'seconds' is not passed in, convert the current time instead.";

static const char qp_time_doc_strftime[] =
"strftime(format[, time_tuple]) -> string\n\
\n\
Convert a time tuple to a string according to a format specification.\n\
See the library reference manual for formatting codes. When the time tuple\n\
is not present, current time as returned by localtime() is used.\n\
\n" QP_TIME_STRFTIME_FORMAT_CODES;

static const char qp_time_doc_strptime[] =
"strptime(string[, format]) -> struct_time\n\
\n\
Parse a string to a time tuple according to a format specification.\n\
See the library reference manual for formatting codes (same as\n\
strftime()).\n\
\n" QP_TIME_STRFTIME_FORMAT_CODES;

static const char qp_time_doc_asctime[] =
"asctime([time_tuple]) -> string\n\
\n\
Convert a time tuple to a string, e.g. 'Sat Jun 06 16:26:11 1998'.\n\
When the time tuple is not present, current time as returned by localtime()\n\
is used.";

static const char qp_time_doc_ctime[] =
"ctime([seconds]) -> string\n\
\n\
Convert a time in seconds since the Epoch to a string in local time.\n\
This is equivalent to asctime(localtime(seconds)). When 'seconds' is not\n\
passed in, convert the current time instead.";

static const char qp_time_doc_mktime[] =
"mktime(time_tuple) -> floating-point number\n\
\n\
Convert a time tuple in local time to seconds since the Epoch.\n\
Note that mktime(gmtime(0)) will not generally return zero for most\n\
time zones; instead the returned value will either be equal to that\n\
of the timezone or altzone attributes on the time module.";

static const char qp_time_doc_tzset[] =
"tzset()\n\
\n\
Initialize, or reinitialize, the local timezone to the value stored in\n\
os.environ['TZ']. The TZ environment variable should be specified in\n\
standard Unix timezone format as documented in the tzset man page\n\
(eg. 'US/Eastern', 'Europe/Amsterdam'). Unknown timezones will silently\n\
fall back to UTC. If the TZ environment variable is not set, the local\n\
timezone is set to the systems best guess of wallclock time.\n\
Changing the TZ environment variable without calling tzset *may* change\n\
the local timezone used by methods such as localtime, but this behaviour\n\
should not be relied on.";

static const char qp_time_doc_monotonic[] =
"monotonic() -> float\n\
\n\
Monotonic clock, cannot go backward.";

static const char qp_time_doc_monotonic_ns[] =
"monotonic_ns() -> int\n\
\n\
Monotonic clock, cannot go backward, as nanoseconds.";

static const char qp_time_doc_perf_counter[] =
"perf_counter() -> float\n\
\n\
Performance counter for benchmarking.";

static const char qp_time_doc_perf_counter_ns[] =
"perf_counter_ns() -> int\n\
\n\
Performance counter for benchmarking as nanoseconds.";

static const char qp_time_doc_process_time[] =
"process_time() -> float\n\
\n\
Process time for profiling: sum of the kernel and user-space CPU time.";

static const char qp_time_doc_process_time_ns[] =
"process_time() -> int\n\
\n\
Process time for profiling as nanoseconds:\n\
sum of the kernel and user-space CPU time.";

static const char qp_time_doc_thread_time[] =
"thread_time() -> float\n\
\n\
Thread time for profiling: sum of the kernel and user-space CPU time.";

static const char qp_time_doc_thread_time_ns[] =
"thread_time() -> int\n\
\n\
Thread time for profiling as nanoseconds:\n\
sum of the kernel and user-space CPU time.";

static const char qp_time_doc_get_clock_info[] =
"get_clock_info(name: str) -> dict\n\
\n\
Get information of the specified clock.";

static const char qp_time_doc_module[] =
"This module provides various functions to manipulate time values.\n\
\n\
There are two standard representations of time.  One is the number\n\
of seconds since the Epoch, in UTC (a.k.a. GMT).  It may be an integer\n\
or a floating-point number (to represent fractions of seconds).\n\
The epoch is the point where the time starts, the return value of time.gmtime(0).\n\
It is January 1, 1970, 00:00:00 (UTC) on all platforms.\n\
\n\
The other representation is a tuple of 9 integers giving local time.\n\
The tuple items are:\n\
  year (including century, e.g. 1998)\n\
  month (1-12)\n\
  day (1-31)\n\
  hours (0-23)\n\
  minutes (0-59)\n\
  seconds (0-59)\n\
  weekday (0-6, Monday is 0)\n\
  Julian day (day in the year, 1-366)\n\
  DST (Daylight Savings Time) flag (-1, 0 or 1)\n\
If the DST flag is 0, the time is given in the regular time zone;\n\
if it is 1, the time is given in the DST time zone;\n\
if it is -1, mktime() should guess based on the date and time.\n";

static const char qp_time_doc_clock_gettime[] =
"clock_gettime($module, clk_id, /)\n"
"--\n"
"\n"
"Return the time of the specified clock clk_id as a float.";

static const char qp_time_doc_clock_gettime_ns[] =
"clock_gettime_ns($module, clk_id, /)\n"
"--\n"
"\n"
"Return the time of the specified clock clk_id as nanoseconds (int).";

/* ------------------------------------------------------------------------- */
/* Platform features (CPython pyconfig.h equivalents)                        */
/* ------------------------------------------------------------------------- */

#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define QP_TIME_HAVE_TM_ZONE 1
#endif
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(__NetBSD__)
#define QP_TIME_HAVE_THREAD_TIME 1
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#define QP_TIME_HAVE_PTHREAD_GETCPUCLOCKID 1
#endif
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__)
#define QP_TIME_HAVE_CLOCK_NANOSLEEP 1
#endif

#define QP_TIME_SEC_TO_NS INT64_C(1000000000)
#define QP_TIME_STRUCT_TIME_PROP "\x01qp_time_struct_time"

/* _PyTime_round_t */
enum {
    QP_TIME_ROUND_FLOOR,
    QP_TIME_ROUND_CEILING,
    QP_TIME_ROUND_HALF_EVEN,
    QP_TIME_ROUND_UP,
};
#define QP_TIME_ROUND_TIMEOUT QP_TIME_ROUND_UP

/* _Py_clock_info_t */
typedef struct QPTimeClockInfo {
    const char *implementation;
    int monotonic;
    int adjustable;
    double resolution;
} QPTimeClockInfo;

/* ------------------------------------------------------------------------- */
/* Python/pytime.c subset                                                    */
/* ------------------------------------------------------------------------- */

static JSValue qp_time_overflow(JSContext *ctx)
{
    return qp_raise_overflow_error(ctx, "timestamp out of range for C PyTime_t");
}

static JSValue qp_time_time_t_overflow(JSContext *ctx)
{
    return qp_raise_overflow_error(ctx, "timestamp out of range for platform time_t");
}

static double qp_time_round_half_even(double x)
{
    double rounded = round(x);
    if (fabs(x - rounded) == 0.5)
        rounded = 2.0 * round(x / 2.0);
    return rounded;
}

static double qp_time_round(double x, int rnd)
{
    volatile double d = x;
    if (rnd == QP_TIME_ROUND_HALF_EVEN)
        d = qp_time_round_half_even(d);
    else if (rnd == QP_TIME_ROUND_CEILING)
        d = ceil(d);
    else if (rnd == QP_TIME_ROUND_FLOOR)
        d = floor(d);
    else
        d = (d >= 0.0) ? ceil(d) : floor(d);
    return d;
}

/* PyIndex_Check */
static BOOL qp_time_index_check(JSContext *ctx, JSValueConst v)
{
    return qp_long_check(ctx, v) || qp_type_of(ctx, v)->slots.nb.nb_index != NULL;
}

/* PyLong_AsLongLong (via __index__); *overflow set instead of raising
   OverflowError. */
static int qp_time_as_int64(JSContext *ctx, JSValueConst obj, int64_t *out, int *overflow)
{
    JSValue v = qp_index(ctx, obj);
    if (JS_IsException(v))
        return -1;
    *overflow = 0;
    if (qp_long_as_int64_overflow(v, out, overflow) < 0 && !*overflow) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    JS_FreeValue(ctx, v);
    return 0;
}

/* _PyTime_FromSecondsObject */
static int qp_time_from_seconds_object(JSContext *ctx, JSValueConst obj, int64_t *tp, int rnd)
{
    if (qp_time_index_check(ctx, obj)) {
        int64_t sec;
        int overflow;
        if (qp_time_as_int64(ctx, obj, &sec, &overflow) < 0)
            return -1;
        if (overflow || sec > INT64_MAX / QP_TIME_SEC_TO_NS || sec < INT64_MIN / QP_TIME_SEC_TO_NS) {
            qp_time_overflow(ctx);
            return -1;
        }
        *tp = sec * QP_TIME_SEC_TO_NS;
        return 0;
    } else {
        double d;
        volatile double ns;
        if (qp_float_as_double(ctx, obj, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
        ns = d * 1e9;
        ns = qp_time_round(ns, rnd);
        if (!((double)INT64_MIN <= ns && ns < -(double)INT64_MIN)) {
            qp_time_overflow(ctx);
            *tp = 0;
            return -1;
        }
        *tp = (int64_t)ns;
        return 0;
    }
}

/* _PyTime_ObjectToTime_t */
static int qp_time_object_to_time_t(JSContext *ctx, JSValueConst obj, time_t *sec, int rnd)
{
    if (qp_time_index_check(ctx, obj)) {
        int64_t v;
        int overflow;
        if (qp_time_as_int64(ctx, obj, &v, &overflow) < 0)
            return -1;
        if (overflow || (int64_t)(time_t)v != v) {
            qp_time_time_t_overflow(ctx);
            return -1;
        }
        *sec = (time_t)v;
        return 0;
    } else {
        double d, intpart;
        volatile double vd;
        if (qp_float_as_double(ctx, obj, &d) < 0)
            return -1;
        if (isnan(d)) {
            qp_raise_value_error(ctx, "Invalid value NaN (not a number)");
            return -1;
        }
        vd = qp_time_round(d, rnd);
        (void)modf(vd, &intpart);
        if (!((double)INT64_MIN <= intpart && intpart < -(double)INT64_MIN) ||
            (sizeof(time_t) < 8 && (intpart < -2147483648.0 || intpart >= 2147483648.0))) {
            qp_time_time_t_overflow(ctx);
            return -1;
        }
        *sec = (time_t)intpart;
        return 0;
    }
}

/* _PyTime_FromTimespec */
static int qp_time_from_timespec(JSContext *ctx, int64_t *tp, const struct timespec *ts)
{
    int64_t sec = (int64_t)ts->tv_sec;
    if (sec > INT64_MAX / QP_TIME_SEC_TO_NS || sec < INT64_MIN / QP_TIME_SEC_TO_NS) {
        *tp = sec > 0 ? INT64_MAX : INT64_MIN;
        if (ctx)
            qp_time_overflow(ctx);
        return -1;
    }
    sec *= QP_TIME_SEC_TO_NS;
    if ((ts->tv_nsec > 0 && sec > INT64_MAX - ts->tv_nsec) ||
        (ts->tv_nsec < 0 && sec < INT64_MIN - ts->tv_nsec)) {
        *tp = INT64_MAX;
        if (ctx)
            qp_time_overflow(ctx);
        return -1;
    }
    *tp = sec + ts->tv_nsec;
    return 0;
}

/* _PyTime_FromTimeval */
static int qp_time_from_timeval(JSContext *ctx, int64_t *tp, const struct timeval *tv)
{
    struct timespec ts;
    ts.tv_sec = tv->tv_sec;
    ts.tv_nsec = (long)tv->tv_usec * 1000;
    return qp_time_from_timespec(ctx, tp, &ts);
}

/* _PyTime_AsTimespec (ROUND_FLOOR for the nanosecond part) */
static void qp_time_as_timespec(int64_t t, struct timespec *ts)
{
    int64_t sec = t / QP_TIME_SEC_TO_NS, ns = t % QP_TIME_SEC_TO_NS;
    if (ns < 0) {
        ns += QP_TIME_SEC_TO_NS;
        sec -= 1;
    }
    ts->tv_sec = (time_t)sec;
    ts->tv_nsec = (long)ns;
}

/* PyTime_AsSecondsDouble */
static double qp_time_as_seconds_double(int64_t ns)
{
    volatile double d;
    if (ns % QP_TIME_SEC_TO_NS == 0) {
        d = (double)(ns / QP_TIME_SEC_TO_NS);
    } else {
        d = (double)ns;
        d /= 1e9;
    }
    return d;
}

/* _PyTimeFraction_Mul: ticks * numer / denom without intermediate overflow */
static int64_t qp_time_fraction_mul(int64_t ticks, int64_t numer, int64_t denom)
{
    if (numer == 1 && denom == 1)
        return ticks;
    return (ticks / denom) * numer + (ticks % denom) * numer / denom;
}

#if defined(__APPLE__)
static int64_t qp_time_gcd(int64_t a, int64_t b)
{
    while (b != 0) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}
#endif

/* ---- clocks -------------------------------------------------------------- */

/* PyTime_Time / _PyTime_TimeWithInfo */
static int qp_time_realtime(JSContext *ctx, int64_t *tp, QPTimeClockInfo *info)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        if (ctx)
            qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    if (qp_time_from_timespec(ctx, tp, &ts) < 0)
        return -1;
    if (info) {
        struct timespec res;
        info->implementation = "clock_gettime(CLOCK_REALTIME)";
        info->monotonic = 0;
        info->adjustable = 1;
        if (clock_getres(CLOCK_REALTIME, &res) == 0)
            info->resolution = (double)res.tv_sec + (double)res.tv_nsec * 1e-9;
        else
            info->resolution = 1e-9;
    }
    return 0;
}

/* PyTime_Monotonic / _PyTime_MonotonicWithInfo (also perf_counter) */
static int qp_time_monotonic_clock(JSContext *ctx, int64_t *tp, QPTimeClockInfo *info)
{
#if defined(__APPLE__)
    static int64_t numer, denom;
    uint64_t uticks;
    if (denom == 0) {
        /* py_mach_timebase_info: mach_timebase_info() cannot fail (QA1398) */
        mach_timebase_info_data_t timebase;
        int64_t g;
        (void)mach_timebase_info(&timebase);
        g = qp_time_gcd((int64_t)timebase.numer, (int64_t)timebase.denom);
        numer = (int64_t)timebase.numer / g;
        denom = (int64_t)timebase.denom / g;
    }
    if (info) {
        info->implementation = "mach_absolute_time()";
        info->resolution = (double)numer / (double)denom / 1e9;
        info->monotonic = 1;
        info->adjustable = 0;
    }
    uticks = mach_absolute_time();
    *tp = qp_time_fraction_mul((int64_t)uticks, numer, denom);
    return 0;
#else
#ifdef CLOCK_HIGHRES
    const clockid_t clk_id = CLOCK_HIGHRES;
    const char *implementation = "clock_gettime(CLOCK_HIGHRES)";
#else
    const clockid_t clk_id = CLOCK_MONOTONIC;
    const char *implementation = "clock_gettime(CLOCK_MONOTONIC)";
#endif
    struct timespec ts;
    if (clock_gettime(clk_id, &ts) != 0) {
        if (ctx)
            qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    if (qp_time_from_timespec(ctx, tp, &ts) < 0)
        return -1;
    if (info) {
        struct timespec res;
        info->monotonic = 1;
        info->implementation = implementation;
        info->adjustable = 0;
        if (clock_getres(clk_id, &res) != 0) {
            qp_raise_os_error(ctx, errno, QP_NULL);
            return -1;
        }
        info->resolution = res.tv_sec + res.tv_nsec * 1e-9;
    }
    return 0;
#endif
}

/* process_time_times */
static int qp_time_process_times(int64_t *tp, QPTimeClockInfo *info)
{
    static long ticks_per_second = -1;
    struct tms process;
    if (ticks_per_second == -1)
        ticks_per_second = sysconf(_SC_CLK_TCK);
    if (ticks_per_second < 1)
        return 0;
    if (times(&process) == (clock_t)-1)
        return 0;
    if (info) {
        info->implementation = "times()";
        info->resolution = 1.0 / (double)ticks_per_second;
        info->monotonic = 1;
        info->adjustable = 0;
    }
    *tp = qp_time_fraction_mul((int64_t)process.tms_utime, QP_TIME_SEC_TO_NS, ticks_per_second) +
          qp_time_fraction_mul((int64_t)process.tms_stime, QP_TIME_SEC_TO_NS, ticks_per_second);
    return 1;
}

/* py_process_time */
static int qp_time_process_time(JSContext *ctx, int64_t *tp, QPTimeClockInfo *info)
{
#if defined(CLOCK_PROCESS_CPUTIME_ID) || defined(CLOCK_PROF)
    {
        struct timespec ts;
#ifdef CLOCK_PROF
        const clockid_t clk_id = CLOCK_PROF;
        const char *function = "clock_gettime(CLOCK_PROF)";
#else
        const clockid_t clk_id = CLOCK_PROCESS_CPUTIME_ID;
        const char *function = "clock_gettime(CLOCK_PROCESS_CPUTIME_ID)";
#endif
        if (clock_gettime(clk_id, &ts) == 0) {
            if (info) {
                struct timespec res;
                info->implementation = function;
                info->monotonic = 1;
                info->adjustable = 0;
                if (clock_getres(clk_id, &res)) {
                    qp_raise_os_error(ctx, errno, QP_NULL);
                    return -1;
                }
                info->resolution = res.tv_sec + res.tv_nsec * 1e-9;
            }
            return qp_time_from_timespec(ctx, tp, &ts);
        }
    }
#endif
    {
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            int64_t utime, stime;
            if (info) {
                info->implementation = "getrusage(RUSAGE_SELF)";
                info->monotonic = 1;
                info->adjustable = 0;
                info->resolution = 1e-6;
            }
            if (qp_time_from_timeval(ctx, &utime, &ru.ru_utime) < 0 ||
                qp_time_from_timeval(ctx, &stime, &ru.ru_stime) < 0)
                return -1;
            *tp = utime + stime;
            return 0;
        }
    }
    if (qp_time_process_times(tp, info) == 1)
        return 0;
    {
        /* py_clock */
        clock_t ticks = clock();
        if (ticks == (clock_t)-1) {
            qp_raise_runtime_error(ctx, "the processor time used is not available "
                                        "or its value cannot be represented");
            return -1;
        }
        if (info) {
            info->implementation = "clock()";
            info->resolution = 1.0 / (double)CLOCKS_PER_SEC;
            info->monotonic = 1;
            info->adjustable = 0;
        }
        *tp = qp_time_fraction_mul((int64_t)ticks, QP_TIME_SEC_TO_NS, CLOCKS_PER_SEC);
        return 0;
    }
}

#ifdef QP_TIME_HAVE_THREAD_TIME
/* _PyTime_GetThreadTimeWithInfo */
static int qp_time_thread_time(JSContext *ctx, int64_t *tp, QPTimeClockInfo *info)
{
    struct timespec ts;
    const clockid_t clk_id = CLOCK_THREAD_CPUTIME_ID;
    const char *function = "clock_gettime(CLOCK_THREAD_CPUTIME_ID)";
    if (clock_gettime(clk_id, &ts)) {
        qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    if (info) {
        struct timespec res;
        info->implementation = function;
        info->monotonic = 1;
        info->adjustable = 0;
        if (clock_getres(clk_id, &res)) {
            qp_raise_os_error(ctx, errno, QP_NULL);
            return -1;
        }
        info->resolution = res.tv_sec + res.tv_nsec * 1e-9;
    }
    return qp_time_from_timespec(ctx, tp, &ts);
}
#endif

/* ------------------------------------------------------------------------- */
/* Argument helpers (PyArg_ParseTuple subset used by timemodule.c)           */
/* ------------------------------------------------------------------------- */

/* vgetargs1_impl argument count check; message = the ';' format message */
static int qp_time_check_nargs(JSContext *ctx, const char *fname, int nargs, int min, int max,
                               const char *message)
{
    if (nargs < min || max < nargs) {
        int n = nargs < min ? min : max;
        if (message)
            qp_raise_type_error(ctx, "%s", message);
        else
            qp_raise_type_error(ctx, "%s%s takes %s %d argument%s (%d given)",
                                fname ? fname : "function", fname ? "()" : "",
                                min == max ? "exactly" : nargs < min ? "at least" : "at most",
                                n, n == 1 ? "" : "s", nargs);
        return -1;
    }
    return 0;
}

/* METH_VARARGS entry: no keywords + count check */
static int qp_time_varargs(JSContext *ctx, const char *fname, int nargs, JSValueConst kwnames,
                           int min, int max)
{
    if (qp_arg_no_keywords(ctx, fname, kwnames) < 0)
        return -1;
    return qp_time_check_nargs(ctx, fname, nargs, min, max, NULL);
}

/* "i" format unit (iarg is 1-based); message = ';' message or NULL */
static int qp_time_getarg_int(JSContext *ctx, JSValueConst v, int *out, const char *fname,
                              int iarg, const char *message)
{
    int64_t x;
    JSValue iv;
    if (qp_float_check(ctx, v)) {
        if (message)
            qp_raise_type_error(ctx, "%s", message);
        else
            qp_raise_type_error(ctx, "%s() argument %d must be int, not %T", fname, iarg, v);
        return -1;
    }
    iv = qp_index(ctx, v);
    if (JS_IsException(iv))
        return -1;
    if (qp_long_as_int64(ctx, iv, &x) < 0) {
        JS_FreeValue(ctx, iv);
        return -1;
    }
    JS_FreeValue(ctx, iv);
    if (x > INT_MAX) {
        qp_raise_overflow_error(ctx, "signed integer is greater than maximum");
        return -1;
    }
    if (x < INT_MIN) {
        qp_raise_overflow_error(ctx, "signed integer is less than minimum");
        return -1;
    }
    *out = (int)x;
    return 0;
}

/* PyUnicode_DecodeLocale(s, "surrogateescape") (UTF-8 locale) */
static JSValue qp_time_decode_locale(JSContext *ctx, const char *s, size_t len)
{
    return qp_str_decode(ctx, s, (Py_ssize_t)len, "utf-8", "surrogateescape");
}

static int qp_time_add(JSContext *ctx, JSValueConst module, const char *name, JSValue v)
{
    JSAtom at;
    int r;
    if (JS_IsException(v))
        return -1;
    at = JS_NewAtom(ctx, name);
    r = qp_dict_set_item_atom(ctx, qp_module_get_dict(module), at, v);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, v);
    return r;
}

/* the module's time.struct_time type (state->struct_time_type) */
static QPType *qp_time_struct_time_type(JSContext *ctx, JSValueConst module)
{
    JSValue tv = JS_GetPropertyStr(ctx, module, QP_TIME_STRUCT_TIME_PROP);
    QPType *t;
    if (JS_IsException(tv))
        return NULL;
    t = qp_type_from_value(tv);
    JS_FreeValue(ctx, tv);  /* the module keeps the type alive */
    if (!t)
        qp_raise_system_error(ctx, "time module state is not initialized");
    return t;
}

/* ------------------------------------------------------------------------- */
/* time(), time_ns(), clock_*()                                             */
/* ------------------------------------------------------------------------- */

static JSValue qp_time_time(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    int64_t t;
    if (qp_time_realtime(ctx, &t, NULL) < 0)
        return JS_EXCEPTION;
    return magic ? qp_long_from_int64(ctx, t) : qp_float_new(ctx, qp_time_as_seconds_double(t));
}

/* time_clockid_converter */
static int qp_time_clockid_converter(JSContext *ctx, JSValueConst obj, clockid_t *p)
{
    int64_t x;
    JSValue iv = qp_index(ctx, obj);
    int ok = 0;
    if (!JS_IsException(iv)) {
        if (qp_long_as_int64(ctx, iv, &x) == 0 && x >= INT_MIN && x <= INT_MAX)
            ok = 1;
        JS_FreeValue(ctx, iv);
    }
    if (!ok) {
        QPType *t = qp_type_of(ctx, obj);
        const char *tn;
        JS_FreeValue(ctx, qp_exc_fetch(ctx));
        tn = JS_AtomToCString(ctx, t->name);
        if (!tn)
            return -1;
        qp_raise_type_error(ctx, "clk_id should be integer, not %s", tn);
        JS_FreeCString(ctx, tn);
        return -1;
    }
    *p = (clockid_t)x;
    return 0;
}

static JSValue qp_time_clock_gettime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    clockid_t clk_id;
    struct timespec ts;
    if (qp_time_clockid_converter(ctx, args[0], &clk_id) < 0)
        return JS_EXCEPTION;
    if (clock_gettime(clk_id, &ts) != 0)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    if (magic) {
        int64_t t;
        if (qp_time_from_timespec(ctx, &t, &ts) < 0)
            return JS_EXCEPTION;
        return qp_long_from_int64(ctx, t);
    }
    return qp_float_new(ctx, ts.tv_sec + ts.tv_nsec * 1e-9);
}

/* clock_settime (magic 0) / clock_settime_ns (magic 1); both parse with
   "iO:clock_settime" like CPython. */
static JSValue qp_time_clock_settime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    int clk_id;
    int64_t t;
    struct timespec ts;
    if (qp_time_varargs(ctx, "clock_settime", nargs, kwnames, 2, 2) < 0 ||
        qp_time_getarg_int(ctx, args[0], &clk_id, "clock_settime", 1, NULL) < 0)
        return JS_EXCEPTION;
    if (magic) {
        /* PyLong_AsInt64 */
        int overflow;
        if (qp_time_as_int64(ctx, args[1], &t, &overflow) < 0)
            return JS_EXCEPTION;
        if (overflow)
            return qp_raise_overflow_error(ctx, "Python int too large to convert to C int64_t");
    } else {
        if (qp_time_from_seconds_object(ctx, args[1], &t, QP_TIME_ROUND_FLOOR) < 0)
            return JS_EXCEPTION;
    }
    qp_time_as_timespec(t, &ts);
    if (clock_settime((clockid_t)clk_id, &ts) != 0)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return JS_UNDEFINED;
}

static JSValue qp_time_clock_getres(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    int clk_id;
    struct timespec tp;
    if (qp_time_varargs(ctx, "clock_getres", nargs, kwnames, 1, 1) < 0 ||
        qp_time_getarg_int(ctx, args[0], &clk_id, "clock_getres", 1, NULL) < 0)
        return JS_EXCEPTION;
    if (clock_getres((clockid_t)clk_id, &tp) != 0)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return qp_float_new(ctx, tp.tv_sec + tp.tv_nsec * 1e-9);
}

#ifdef QP_TIME_HAVE_PTHREAD_GETCPUCLOCKID
static JSValue qp_time_pthread_getcpuclockid(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                             int nargs, JSValueConst kwnames, int magic)
{
    uint64_t thread_id;
    int64_t x;
    int overflow = 0, err;
    clockid_t clk_id;
    JSValue v;
    if (qp_time_varargs(ctx, "pthread_getcpuclockid", nargs, kwnames, 1, 1) < 0)
        return JS_EXCEPTION;
    /* "k": PyLong_Check required, PyLong_AsUnsignedLongMask */
    if (!qp_long_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "pthread_getcpuclockid() argument 1 must be int, not %T",
                                   args[0]);
    v = qp_long_value(ctx, args[0]);
    if (JS_IsException(v))
        return JS_EXCEPTION;
    if (qp_long_as_int64_overflow(v, &x, &overflow) == 0 && !overflow) {
        thread_id = (uint64_t)x;
    } else if (JS_ToBigUint64(ctx, &thread_id, v) < 0) {
        JS_FreeValue(ctx, v);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    err = pthread_getcpuclockid((pthread_t)(unsigned long)thread_id, &clk_id);
    if (err)
        return qp_raise_os_error(ctx, err, QP_NULL);
    return qp_long_from_int64(ctx, (int64_t)clk_id);
}
#endif

/* ------------------------------------------------------------------------- */
/* sleep                                                                     */
/* ------------------------------------------------------------------------- */

/* pysleep: the sleep releases the GIL (sleep(0) too, to let other threads
   run); EINTR runs the Python signal handlers (PyErr_CheckSignals) and
   resumes with the remaining time. */
static int qp_time_pysleep(JSContext *ctx, int64_t timeout)
{
    int64_t deadline, monotonic;
    int err = 0;
#ifdef QP_TIME_HAVE_CLOCK_NANOSLEEP
    struct timespec timeout_abs;
#else
    struct timespec timeout_ts;
#endif

    if (qp_time_monotonic_clock(ctx, &monotonic, NULL) < 0)
        return -1;
    if (monotonic > INT64_MAX - timeout)
        deadline = INT64_MAX;
    else
        deadline = monotonic + timeout;
#ifdef QP_TIME_HAVE_CLOCK_NANOSLEEP
    qp_time_as_timespec(deadline, &timeout_abs);
#endif

    do {
        int ret;
#ifdef QP_TIME_HAVE_CLOCK_NANOSLEEP
        QP_BEGIN_ALLOW_THREADS(ctx)
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &timeout_abs, NULL);
        QP_END_ALLOW_THREADS(ctx)
        err = ret;
#else
        qp_time_as_timespec(timeout, &timeout_ts);
        QP_BEGIN_ALLOW_THREADS(ctx)
        ret = nanosleep(&timeout_ts, NULL);
        err = errno;
        QP_END_ALLOW_THREADS(ctx)
#endif
        if (ret == 0)
            break;
        if (err != EINTR) {
            qp_raise_os_error(ctx, err, QP_NULL);
            return -1;
        }
        /* sleep was interrupted by SIGINT */
        if (qp_signal_check(ctx) < 0)
            return -1;
#ifndef QP_TIME_HAVE_CLOCK_NANOSLEEP
        if (qp_time_monotonic_clock(ctx, &monotonic, NULL) < 0)
            return -1;
        timeout = deadline - monotonic;
        if (timeout < 0)
            break;
#endif
    } while (1);
    return 0;
}

/* time.sleep (PySys_Audit("time.sleep") is not raised: this runtime has
   no audit-hook dispatch exposed to native modules). */
static JSValue qp_time_sleep(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    int64_t timeout;
    if (qp_time_from_seconds_object(ctx, args[0], &timeout, QP_TIME_ROUND_TIMEOUT) < 0)
        return JS_EXCEPTION;
    if (timeout < 0)
        return qp_raise_value_error(ctx, "sleep length must be non-negative");
    if (qp_time_pysleep(ctx, timeout) != 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------------- */
/* struct_time                                                               */
/* ------------------------------------------------------------------------- */

static const char *const qp_time_struct_time_fields[] = {
    "tm_year", "tm_mon", "tm_mday", "tm_hour", "tm_min", "tm_sec",
    "tm_wday", "tm_yday", "tm_isdst", "tm_zone", "tm_gmtoff", NULL
};

static const char *const qp_time_struct_time_field_docs[] = {
    "year, for example, 1993",
    "month of year, range [1, 12]",
    "day of month, range [1, 31]",
    "hours, range [0, 23]",
    "minutes, range [0, 59]",
    "seconds, range [0, 61])",
    "day of week, range [0, 6], Monday is 0",
    "day of year, range [1, 366]",
    "1 if summer time is in effect, 0 if not, and -1 if unknown",
    "abbreviation of timezone name",
    "offset from UTC in seconds",
    NULL
};

static const QPStructSeqDesc qp_time_struct_time_desc = {
    "time.struct_time",
    "The time value as returned by gmtime(), localtime(), and strptime(), and\n"
    " accepted by asctime(), mktime() and strftime().  May be considered as a\n"
    " sequence of 9 integers.\n\n"
    " Note that several fields' values are not the same as those defined by\n"
    " the C language standard for struct tm.  For example, the value of the\n"
    " field tm_year is the actual year, not year - 1900.  See individual\n"
    " fields' descriptions for details.",
    qp_time_struct_time_fields,
    qp_time_struct_time_field_docs,
    9,
    0,
};

/* tmtotuple */
static JSValue qp_time_tmtotuple(JSContext *ctx, JSValueConst module, const struct tm *p,
                                 const char *zone, long gmtoff)
{
    QPType *t = qp_time_struct_time_type(ctx, module);
    JSValue items[11];
    int i;
    if (!t)
        return JS_EXCEPTION;
    items[0] = qp_long_from_int64(ctx, (int64_t)p->tm_year + 1900);
    items[1] = JS_NewInt32(ctx, p->tm_mon + 1);         /* Want January == 1 */
    items[2] = JS_NewInt32(ctx, p->tm_mday);
    items[3] = JS_NewInt32(ctx, p->tm_hour);
    items[4] = JS_NewInt32(ctx, p->tm_min);
    items[5] = JS_NewInt32(ctx, p->tm_sec);
    items[6] = JS_NewInt32(ctx, (p->tm_wday + 6) % 7); /* Want Monday == 0 */
    items[7] = JS_NewInt32(ctx, p->tm_yday + 1);        /* Want January, 1 == 1 */
    items[8] = JS_NewInt32(ctx, p->tm_isdst);
#ifdef QP_TIME_HAVE_TM_ZONE
    (void)zone;
    (void)gmtoff;
    if (p->tm_zone)
        items[9] = qp_time_decode_locale(ctx, p->tm_zone, strlen(p->tm_zone));
    else
        items[9] = JS_UNDEFINED;  /* PyUnicode_DecodeLocale(NULL) cannot happen in CPython */
    items[10] = qp_long_from_int64(ctx, (int64_t)p->tm_gmtoff);
#else
    items[9] = qp_time_decode_locale(ctx, zone, strlen(zone));
    items[10] = qp_long_from_int64(ctx, (int64_t)gmtoff);
#endif
    for (i = 0; i < 11; i++) {
        if (JS_IsException(items[i])) {
            int j;
            for (j = 0; j < 11; j++)
                JS_FreeValue(ctx, items[j]);
            return JS_EXCEPTION;
        }
    }
    return qp_structseq_new(ctx, t, items);
}

/* _PyTime_gmtime */
static int qp_time_gmtime_r(JSContext *ctx, time_t t, struct tm *tm)
{
    errno = 0;
    if (gmtime_r(&t, tm) == NULL) {
        if (errno == 0)
            errno = EINVAL;
        qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    return 0;
}

/* _PyTime_localtime */
static int qp_time_localtime_r(JSContext *ctx, time_t t, struct tm *tm)
{
    errno = 0;
    if (localtime_r(&t, tm) == NULL) {
        if (errno == 0)
            errno = EINVAL;
        qp_raise_os_error(ctx, errno, QP_NULL);
        return -1;
    }
    return 0;
}

/* parse_time_t_args ("|O:name") */
static int qp_time_parse_time_t_args(JSContext *ctx, const char *fname, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, time_t *pwhen)
{
    if (qp_time_varargs(ctx, fname, nargs, kwnames, 0, 1) < 0)
        return -1;
    if (nargs == 0 || JS_IsUndefined(args[0])) {
        *pwhen = time(NULL);
        return 0;
    }
    return qp_time_object_to_time_t(ctx, args[0], pwhen, QP_TIME_ROUND_FLOOR);
}

#ifndef QP_TIME_HAVE_TM_ZONE
static long qp_time_gmtoff_of(time_t when, struct tm *p)
{
    struct tm cp = *p;
    return (long)(timegm(&cp) - when);
}
#endif

static JSValue qp_time_gmtime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    time_t when;
    struct tm buf;
    if (qp_time_parse_time_t_args(ctx, "gmtime", args, nargs, kwnames, &when) < 0)
        return JS_EXCEPTION;
    if (qp_time_gmtime_r(ctx, when, &buf) != 0)
        return JS_EXCEPTION;
    return qp_time_tmtotuple(ctx, self, &buf, "UTC", 0);
}

static JSValue qp_time_localtime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    time_t when;
    struct tm buf;
    if (qp_time_parse_time_t_args(ctx, "localtime", args, nargs, kwnames, &when) < 0)
        return JS_EXCEPTION;
    if (qp_time_localtime_r(ctx, when, &buf) != 0)
        return JS_EXCEPTION;
#ifdef QP_TIME_HAVE_TM_ZONE
    return qp_time_tmtotuple(ctx, self, &buf, NULL, 0);
#else
    {
        char zone[100];
        strftime(zone, sizeof(zone), "%Z", &buf);
        return qp_time_tmtotuple(ctx, self, &buf, zone, qp_time_gmtoff_of(when, &buf));
    }
#endif
}

/* gettmarg: convert a 9-item tuple (or struct_time) to struct tm.
   *pzone receives the UTF-8 tm_zone buffer (free with JS_FreeCString after
   the struct tm is no longer used) or NULL. */
static int qp_time_gettmarg(JSContext *ctx, JSValueConst module, JSValueConst args,
                            struct tm *p, const char *message, const char **pzone)
{
    int vals[9];
    int i, y;
    Py_ssize_t n;
    QPType *st;

    memset((void *)p, '\0', sizeof(struct tm));
    *pzone = NULL;
    if (!qp_tuple_check(ctx, args)) {
        qp_raise_type_error(ctx, "Tuple or struct_time argument required");
        return -1;
    }
    n = QP_TUPLE_SIZE(args);
    if (qp_time_check_nargs(ctx, NULL, (int)(n > INT_MAX ? INT_MAX : n), 9, 9, message) < 0)
        return -1;
    for (i = 0; i < 9; i++) {
        if (qp_time_getarg_int(ctx, QP_TUPLE_ITEMS(args)[i], &vals[i], NULL, i + 1, message) < 0)
            return -1;
    }
    y = vals[0];
    if (y < INT_MIN + 1900) {
        qp_raise_overflow_error(ctx, "year out of range");
        return -1;
    }
    p->tm_year = y - 1900;
    p->tm_mon = vals[1] - 1;
    p->tm_mday = vals[2];
    p->tm_hour = vals[3];
    p->tm_min = vals[4];
    p->tm_sec = vals[5];
    p->tm_wday = (vals[6] + 1) % 7;
    p->tm_yday = vals[7] - 1;
    p->tm_isdst = vals[8];
#ifdef QP_TIME_HAVE_TM_ZONE
    st = qp_time_struct_time_type(ctx, module);
    if (!st)
        return -1;
    if (qp_type_of(ctx, args) == st) {
        JSAtom at;
        JSValue item;
        at = JS_NewAtom(ctx, "tm_zone");
        item = qp_getattr(ctx, args, at);
        JS_FreeAtom(ctx, at);
        if (JS_IsException(item))
            return -1;
        if (!JS_IsUndefined(item)) {
            JSValue s;
            const char *z;
            if (!qp_str_check(ctx, item)) {
                JS_FreeValue(ctx, item);
                qp_raise_type_error(ctx, "bad argument type for built-in operation");
                return -1;
            }
            s = qp_str_value(ctx, item);
            JS_FreeValue(ctx, item);
            if (JS_IsException(s))
                return -1;
            z = qp_str_utf8(ctx, s, NULL);
            JS_FreeValue(ctx, s);
            if (!z)
                return -1;
            *pzone = z;
            p->tm_zone = (char *)z;
        } else {
            JS_FreeValue(ctx, item);
        }
        at = JS_NewAtom(ctx, "tm_gmtoff");
        item = qp_getattr(ctx, args, at);
        JS_FreeAtom(ctx, at);
        if (JS_IsException(item))
            return -1;
        if (!JS_IsUndefined(item)) {
            /* PyLong_AsLong */
            int64_t off;
            int overflow;
            int r = qp_time_as_int64(ctx, item, &off, &overflow);
            JS_FreeValue(ctx, item);
            if (r < 0)
                return -1;
            if (overflow || (int64_t)(long)off != off) {
                qp_raise_overflow_error(ctx, "Python int too large to convert to C long");
                return -1;
            }
            p->tm_gmtoff = (long)off;
        }
    }
#else
    (void)st;
#endif
    return 0;
}

static void qp_time_free_zone(JSContext *ctx, const char *zone)
{
    if (zone)
        JS_FreeCString(ctx, zone);
}

/* checktm */
static int qp_time_checktm(JSContext *ctx, struct tm *buf)
{
    if (buf->tm_mon == -1)
        buf->tm_mon = 0;
    else if (buf->tm_mon < 0 || buf->tm_mon > 11) {
        qp_raise_value_error(ctx, "month out of range");
        return -1;
    }
    if (buf->tm_mday == 0)
        buf->tm_mday = 1;
    else if (buf->tm_mday < 0 || buf->tm_mday > 31) {
        qp_raise_value_error(ctx, "day of month out of range");
        return -1;
    }
    if (buf->tm_hour < 0 || buf->tm_hour > 23) {
        qp_raise_value_error(ctx, "hour out of range");
        return -1;
    }
    if (buf->tm_min < 0 || buf->tm_min > 59) {
        qp_raise_value_error(ctx, "minute out of range");
        return -1;
    }
    if (buf->tm_sec < 0 || buf->tm_sec > 61) {
        qp_raise_value_error(ctx, "seconds out of range");
        return -1;
    }
    /* tm_wday does not need checking of its upper-bound since taking
       ``% 7`` in gettmarg() automatically restricts the range. */
    if (buf->tm_wday < 0) {
        qp_raise_value_error(ctx, "day of week out of range");
        return -1;
    }
    if (buf->tm_yday == -1)
        buf->tm_yday = 0;
    else if (buf->tm_yday < 0 || buf->tm_yday > 365) {
        qp_raise_value_error(ctx, "day of year out of range");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* strftime / strptime / asctime / ctime / mktime                            */
/* ------------------------------------------------------------------------- */

/* time_strftime1: format is a NUL-terminated ASCII chunk */
static JSValue qp_time_strftime1(JSContext *ctx, char **outbuf, size_t *bufsize,
                                 const char *format, size_t fmtlen, struct tm *tm)
{
    size_t buflen;
    /* I hate these functions that presume you know how big the output
     * will be ahead of time... */
    while (1) {
        char *tmp;
        if (*bufsize > (size_t)PY_SSIZE_T_MAX)
            return qp_raise_memory_error(ctx);
        tmp = js_realloc(ctx, *outbuf, *bufsize);
        if (!tmp) {
            js_free(ctx, *outbuf);
            *outbuf = NULL;
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            return qp_raise_memory_error(ctx);
        }
        *outbuf = tmp;
        buflen = strftime(*outbuf, *bufsize, format, tm);
        if (buflen == 0 && *bufsize < 256 * fmtlen) {
            *bufsize += *bufsize;
            continue;
        }
        /* If the buffer is 256 times as long as the format,
           it's probably not failing for lack of room!
           More likely, the format yields an empty result,
           e.g. an empty format, or %Z when the timezone
           is unknown. */
        return qp_time_decode_locale(ctx, *outbuf, buflen);
    }
}

#define QP_TIME_READ(view, i)                                              \
    ((view).kind == 1 ? (uint32_t)((const uint8_t *)(view).data)[i] :      \
     (view).kind == 2 ? (uint32_t)((const uint16_t *)(view).data)[i] :     \
                        ((const uint32_t *)(view).data)[i])

/* result = result + piece (consumes both); JS_EXCEPTION on error */
static JSValue qp_time_append(JSContext *ctx, JSValue result, JSValue piece)
{
    JSValue r;
    if (JS_IsException(piece)) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    r = qp_str_concat(ctx, result, piece);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, piece);
    return r;
}

static JSValue qp_time_strftime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    struct tm buf;
    const char *zone = NULL;
    JSValue fmt, result = JS_EXCEPTION;
    qp_strview view;
    char *format = NULL, *outbuf = NULL;
    size_t fmtlen, bufsize = 1024;
    Py_ssize_t i, format_size;

    memset((void *)&buf, '\0', sizeof(buf));
    if (qp_time_varargs(ctx, "strftime", nargs, kwnames, 1, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "strftime() argument 1 must be str, not %T", args[0]);
    if (nargs < 2) {
        time_t tt = time(NULL);
        if (qp_time_localtime_r(ctx, tt, &buf) != 0)
            return JS_EXCEPTION;
    } else if (qp_time_gettmarg(ctx, self, args[1], &buf,
                                "strftime(): illegal time tuple argument", &zone) < 0 ||
               qp_time_checktm(ctx, &buf) < 0) {
        qp_time_free_zone(ctx, zone);
        return JS_EXCEPTION;
    }

    /* Normalize tm_isdst just in case someone foolishly implements %Z
       based on the assumption that tm_isdst falls within the range of
       [-1, 1] */
    if (buf.tm_isdst < -1)
        buf.tm_isdst = -1;
    else if (buf.tm_isdst > 1)
        buf.tm_isdst = 1;

    fmt = qp_str_value(ctx, args[0]);
    if (JS_IsException(fmt)) {
        qp_time_free_zone(ctx, zone);
        return JS_EXCEPTION;
    }
    if (qp_str_view_acquire(ctx, fmt, &view) < 0) {
        JS_FreeValue(ctx, fmt);
        qp_time_free_zone(ctx, zone);
        return JS_EXCEPTION;
    }
    format_size = view.len;
    format = js_malloc(ctx, (size_t)format_size + 1);
    if (!format)
        goto done;
    result = qp_str_from_cstr(ctx, "");
    i = 0;
    while (i < format_size && !JS_IsException(result)) {
        Py_ssize_t start;
        fmtlen = 0;
        for (; i < format_size; i++) {
            uint32_t c = QP_TIME_READ(view, i);
            if (!c || c > 127)
                break;
            format[fmtlen++] = (char)c;
        }
        if (fmtlen) {
            format[fmtlen] = 0;
            result = qp_time_append(ctx, result,
                                    qp_time_strftime1(ctx, &outbuf, &bufsize, format, fmtlen, &buf));
            if (JS_IsException(result))
                break;
        }
        start = i;
        for (; i < format_size; i++) {
            if (QP_TIME_READ(view, i) == '%')
                break;
        }
        if (i > start)
            result = qp_time_append(ctx, result, qp_str_substring(ctx, fmt, start, i));
    }
done:
    qp_str_view_release(ctx, &view);
    JS_FreeValue(ctx, fmt);
    js_free(ctx, outbuf);
    js_free(ctx, format);
    qp_time_free_zone(ctx, zone);
    return result;
}

static JSValue qp_time_strptime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValue mod, func, tup, result;
    JSAtom at;
    if (qp_arg_no_keywords(ctx, "strptime", kwnames) < 0)
        return JS_EXCEPTION;
    /* PyImport_ImportModuleAttrString("_strptime", "_strptime_time") */
    mod = qp_import_module(ctx, "_strptime");
    if (JS_IsException(mod))
        return JS_EXCEPTION;
    at = JS_NewAtom(ctx, "_strptime_time");
    func = qp_getattr(ctx, mod, at);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, mod);
    if (JS_IsException(func))
        return JS_EXCEPTION;
    tup = qp_tuple_pack(ctx, nargs, args);
    if (JS_IsException(tup)) {
        JS_FreeValue(ctx, func);
        return JS_EXCEPTION;
    }
    result = qp_call_tuple(ctx, func, tup, QP_NULL);
    JS_FreeValue(ctx, tup);
    JS_FreeValue(ctx, func);
    return result;
}

/* _asctime */
static JSValue qp_time_asctime_fmt(JSContext *ctx, const struct tm *timeptr)
{
    /* Inspired by Open Group reference implementation available at
     * http://pubs.opengroup.org/onlinepubs/009695399/functions/asctime.html */
    static const char wday_name[7][4] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char mon_name[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char buf[96];
    snprintf(buf, sizeof(buf), "%s %s%3d %.2d:%.2d:%.2d %d",
             wday_name[timeptr->tm_wday], mon_name[timeptr->tm_mon],
             timeptr->tm_mday, timeptr->tm_hour, timeptr->tm_min, timeptr->tm_sec,
             (int)((unsigned)timeptr->tm_year + 1900u));
    return qp_str_from_cstr(ctx, buf);
}

static JSValue qp_time_asctime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    struct tm buf;
    const char *zone = NULL;
    JSValue r;
    /* PyArg_UnpackTuple(args, "asctime", 0, 1, &tup) */
    if (qp_arg_no_keywords(ctx, "asctime", kwnames) < 0 ||
        qp_arg_check_positional(ctx, "asctime", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0) {
        time_t tt = time(NULL);
        if (qp_time_localtime_r(ctx, tt, &buf) != 0)
            return JS_EXCEPTION;
    } else if (qp_time_gettmarg(ctx, self, args[0], &buf,
                                "asctime(): illegal time tuple argument", &zone) < 0 ||
               qp_time_checktm(ctx, &buf) < 0) {
        qp_time_free_zone(ctx, zone);
        return JS_EXCEPTION;
    }
    r = qp_time_asctime_fmt(ctx, &buf);
    qp_time_free_zone(ctx, zone);
    return r;
}

static JSValue qp_time_ctime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    time_t tt;
    struct tm buf;
    if (qp_time_parse_time_t_args(ctx, "ctime", args, nargs, kwnames, &tt) < 0)
        return JS_EXCEPTION;
    if (qp_time_localtime_r(ctx, tt, &buf) != 0)
        return JS_EXCEPTION;
    return qp_time_asctime_fmt(ctx, &buf);
}

static JSValue qp_time_mktime(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    struct tm tm;
    time_t tt;
    const char *zone = NULL;
    if (qp_time_gettmarg(ctx, self, args[0], &tm,
                         "mktime(): illegal time tuple argument", &zone) < 0) {
        qp_time_free_zone(ctx, zone);
        return JS_EXCEPTION;
    }
    tm.tm_wday = -1;  /* sentinel; original value ignored */
    tt = mktime(&tm);
    qp_time_free_zone(ctx, zone);
    /* Return value of -1 does not necessarily mean an error, but
     * tm_wday cannot remain set to -1 if mktime succeeded. */
    if (tt == (time_t)(-1) && tm.tm_wday == -1)
        return qp_raise_overflow_error(ctx, "mktime argument out of range");
    return qp_float_new(ctx, (double)tt);
}

/* ------------------------------------------------------------------------- */
/* timezone                                                                  */
/* ------------------------------------------------------------------------- */

/* init_timezone (HAVE_DECL_TZNAME branch; no HAVE_ALTZONE on these
   platforms) */
static int qp_time_init_timezone(JSContext *ctx, JSValueConst m)
{
    JSValue otz0, otz1, items[2];
    tzset();
    if (qp_time_add(ctx, m, "timezone", qp_long_from_int64(ctx, (int64_t)timezone)) < 0 ||
        qp_time_add(ctx, m, "altzone", qp_long_from_int64(ctx, (int64_t)timezone - 3600)) < 0 ||
        qp_time_add(ctx, m, "daylight", qp_long_from_int64(ctx, (int64_t)daylight)) < 0)
        return -1;
    otz0 = qp_time_decode_locale(ctx, tzname[0], strlen(tzname[0]));
    if (JS_IsException(otz0))
        return -1;
    otz1 = qp_time_decode_locale(ctx, tzname[1], strlen(tzname[1]));
    if (JS_IsException(otz1)) {
        JS_FreeValue(ctx, otz0);
        return -1;
    }
    items[0] = otz0;
    items[1] = otz1;
    return qp_time_add(ctx, m, "tzname", qp_tuple_new_steal(ctx, 2, items));
}

static JSValue qp_time_tzset(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue m = qp_import_module(ctx, "time");
    int r;
    if (JS_IsException(m))
        return JS_EXCEPTION;
    tzset();
    /* Reset timezone, altzone, daylight and tzname */
    r = qp_time_init_timezone(ctx, m);
    JS_FreeValue(ctx, m);
    if (r < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* ------------------------------------------------------------------------- */
/* monotonic / perf_counter / process_time / thread_time / get_clock_info    */
/* ------------------------------------------------------------------------- */

/* magic bit 0: _ns variant */
static JSValue qp_time_ret(JSContext *ctx, int64_t t, int ns)
{
    return ns ? qp_long_from_int64(ctx, t) : qp_float_new(ctx, qp_time_as_seconds_double(t));
}

static JSValue qp_time_monotonic(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    int64_t t;
    if (qp_time_monotonic_clock(ctx, &t, NULL) < 0)
        return JS_EXCEPTION;
    return qp_time_ret(ctx, t, magic);
}

static JSValue qp_time_process_time_fn(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    int64_t t;
    if (qp_time_process_time(ctx, &t, NULL) < 0)
        return JS_EXCEPTION;
    return qp_time_ret(ctx, t, magic);
}

#ifdef QP_TIME_HAVE_THREAD_TIME
static JSValue qp_time_thread_time_fn(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    int64_t t;
    if (qp_time_thread_time(ctx, &t, NULL) < 0)
        return JS_EXCEPTION;
    return qp_time_ret(ctx, t, magic);
}
#endif

static JSValue qp_time_get_clock_info(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      int nargs, JSValueConst kwnames, int magic)
{
    static const char *const names[4] = { "implementation", "monotonic", "adjustable", "resolution" };
    QPTimeClockInfo info;
    int64_t t;
    const char *name;
    size_t len;
    JSValue s, ns, d, vals[4];
    int i, r = 0;

    if (qp_time_varargs(ctx, "get_clock_info", nargs, kwnames, 1, 1) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[0]))
        return qp_raise_type_error(ctx, "get_clock_info() argument 1 must be str, not %T", args[0]);
    s = qp_str_value(ctx, args[0]);
    if (JS_IsException(s))
        return JS_EXCEPTION;
    name = qp_str_utf8(ctx, s, &len);
    JS_FreeValue(ctx, s);
    if (!name)
        return JS_EXCEPTION;
    if (strlen(name) != len) {
        JS_FreeCString(ctx, name);
        return qp_raise_value_error(ctx, "embedded null character");
    }

    info.implementation = "";
    info.monotonic = 0;
    info.adjustable = 0;
    info.resolution = 1.0;

    if (strcmp(name, "time") == 0)
        r = qp_time_realtime(ctx, &t, &info);
    else if (strcmp(name, "monotonic") == 0 || strcmp(name, "perf_counter") == 0)
        r = qp_time_monotonic_clock(ctx, &t, &info);
    else if (strcmp(name, "process_time") == 0)
        r = qp_time_process_time(ctx, &t, &info);
#ifdef QP_TIME_HAVE_THREAD_TIME
    else if (strcmp(name, "thread_time") == 0)
        r = qp_time_thread_time(ctx, &t, &info);
#endif
    else {
        JS_FreeCString(ctx, name);
        return qp_raise_value_error(ctx, "unknown clock");
    }
    JS_FreeCString(ctx, name);
    if (r < 0)
        return JS_EXCEPTION;

    /* _PyNamespace_New(dict) */
    vals[0] = qp_str_from_cstr(ctx, info.implementation);
    vals[1] = qp_bool_new(info.monotonic);
    vals[2] = qp_bool_new(info.adjustable);
    vals[3] = qp_float_new(ctx, info.resolution);
    ns = JS_EXCEPTION;
    d = JS_EXCEPTION;
    if (!JS_IsException(vals[0]))
        ns = qp_object_new_instance(ctx, QP_TYPE(ctx, SimpleNamespace), JS_CLASS_PY_OBJECT);
    if (!JS_IsException(ns))
        d = qp_object_get_dict(ctx, ns);
    for (i = 0; i < 4; i++) {
        if (!JS_IsException(d)) {
            JSAtom at = JS_NewAtom(ctx, names[i]);
            if (qp_dict_set_item_atom(ctx, d, at, vals[i]) < 0) {
                JS_FreeValue(ctx, d);
                d = JS_EXCEPTION;
            }
            JS_FreeAtom(ctx, at);
        }
        JS_FreeValue(ctx, vals[i]);
    }
    if (JS_IsException(d)) {
        JS_FreeValue(ctx, ns);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, d);
    return ns;
}

/* ------------------------------------------------------------------------- */
/* module                                                                    */
/* ------------------------------------------------------------------------- */

static const QPMethodDef qp_time_methods[] = {
    { "time", qp_time_time, QP_METH_NOARGS, 0, qp_time_doc_time },
    { "time_ns", qp_time_time, QP_METH_NOARGS, 1, qp_time_doc_time_ns },
    { "clock_gettime", qp_time_clock_gettime, QP_METH_O, 0, qp_time_doc_clock_gettime },
    { "clock_gettime_ns", qp_time_clock_gettime, QP_METH_O, 1, qp_time_doc_clock_gettime_ns },
    { "clock_settime", qp_time_clock_settime, QP_METH_FASTCALL | QP_METH_VARARGS, 0, qp_time_doc_clock_settime },
    { "clock_settime_ns", qp_time_clock_settime, QP_METH_FASTCALL | QP_METH_VARARGS, 1, qp_time_doc_clock_settime_ns },
    { "clock_getres", qp_time_clock_getres, QP_METH_FASTCALL | QP_METH_VARARGS, 0, qp_time_doc_clock_getres },
#ifdef QP_TIME_HAVE_PTHREAD_GETCPUCLOCKID
    { "pthread_getcpuclockid", qp_time_pthread_getcpuclockid, QP_METH_FASTCALL | QP_METH_VARARGS, 0,
      qp_time_doc_pthread_getcpuclockid },
#endif
    { "sleep", qp_time_sleep, QP_METH_O, 0, qp_time_doc_sleep },
    { "gmtime", qp_time_gmtime, QP_METH_FASTCALL, 0, qp_time_doc_gmtime },
    { "localtime", qp_time_localtime, QP_METH_FASTCALL, 0, qp_time_doc_localtime },
    { "asctime", qp_time_asctime, QP_METH_FASTCALL, 0, qp_time_doc_asctime },
    { "ctime", qp_time_ctime, QP_METH_FASTCALL, 0, qp_time_doc_ctime },
    { "mktime", qp_time_mktime, QP_METH_O, 0, qp_time_doc_mktime },
    { "strftime", qp_time_strftime, QP_METH_FASTCALL | QP_METH_VARARGS, 0, qp_time_doc_strftime },
    { "strptime", qp_time_strptime, QP_METH_FASTCALL, 0, qp_time_doc_strptime },
    { "tzset", qp_time_tzset, QP_METH_NOARGS, 0, qp_time_doc_tzset },
    { "monotonic", qp_time_monotonic, QP_METH_NOARGS, 0, qp_time_doc_monotonic },
    { "monotonic_ns", qp_time_monotonic, QP_METH_NOARGS, 1, qp_time_doc_monotonic_ns },
    { "process_time", qp_time_process_time_fn, QP_METH_NOARGS, 0, qp_time_doc_process_time },
    { "process_time_ns", qp_time_process_time_fn, QP_METH_NOARGS, 1, qp_time_doc_process_time_ns },
#ifdef QP_TIME_HAVE_THREAD_TIME
    { "thread_time", qp_time_thread_time_fn, QP_METH_NOARGS, 0, qp_time_doc_thread_time },
    { "thread_time_ns", qp_time_thread_time_fn, QP_METH_NOARGS, 1, qp_time_doc_thread_time_ns },
#endif
    /* perf_counter shares the monotonic clock on POSIX (PyTime_PerfCounter) */
    { "perf_counter", qp_time_monotonic, QP_METH_NOARGS, 0, qp_time_doc_perf_counter },
    { "perf_counter_ns", qp_time_monotonic, QP_METH_NOARGS, 1, qp_time_doc_perf_counter_ns },
    { "get_clock_info", qp_time_get_clock_info, QP_METH_FASTCALL | QP_METH_VARARGS, 0, qp_time_doc_get_clock_info },
    { NULL }
};

static int qp_time_exec(JSContext *ctx, JSValueConst module)
{
    JSValue st;

    /* Set, or reset, module variables like time.timezone */
    if (qp_time_init_timezone(ctx, module) < 0)
        return -1;

#define QP_TIME_CLOCK(NAME) \
    do { if (qp_time_add(ctx, module, #NAME, qp_long_from_int64(ctx, (int64_t)(NAME))) < 0) return -1; } while (0)
#ifdef CLOCK_REALTIME
    QP_TIME_CLOCK(CLOCK_REALTIME);
#endif
#ifdef CLOCK_MONOTONIC
    QP_TIME_CLOCK(CLOCK_MONOTONIC);
#endif
#ifdef CLOCK_MONOTONIC_RAW
    QP_TIME_CLOCK(CLOCK_MONOTONIC_RAW);
#endif
#ifdef CLOCK_HIGHRES
    QP_TIME_CLOCK(CLOCK_HIGHRES);
#endif
#ifdef CLOCK_PROCESS_CPUTIME_ID
    QP_TIME_CLOCK(CLOCK_PROCESS_CPUTIME_ID);
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    QP_TIME_CLOCK(CLOCK_THREAD_CPUTIME_ID);
#endif
#ifdef CLOCK_PROF
    QP_TIME_CLOCK(CLOCK_PROF);
#endif
#ifdef CLOCK_BOOTTIME
    QP_TIME_CLOCK(CLOCK_BOOTTIME);
#endif
#ifdef CLOCK_TAI
    QP_TIME_CLOCK(CLOCK_TAI);
#endif
#ifdef CLOCK_UPTIME
    QP_TIME_CLOCK(CLOCK_UPTIME);
#endif
#ifdef CLOCK_UPTIME_RAW
    QP_TIME_CLOCK(CLOCK_UPTIME_RAW);
#endif
#ifdef CLOCK_MONOTONIC_RAW_APPROX
    QP_TIME_CLOCK(CLOCK_MONOTONIC_RAW_APPROX);
#endif
#ifdef CLOCK_UPTIME_RAW_APPROX
    QP_TIME_CLOCK(CLOCK_UPTIME_RAW_APPROX);
#endif
#undef QP_TIME_CLOCK

    if (qp_time_add(ctx, module, "_STRUCT_TM_ITEMS", JS_NewInt32(ctx, 11)) < 0)
        return -1;

    /* struct_time type */
    st = qp_structseq_new_type(ctx, &qp_time_struct_time_desc);
    if (JS_IsException(st))
        return -1;
    if (qp_time_add(ctx, module, "struct_time", JS_DupValue(ctx, st)) < 0) {
        JS_FreeValue(ctx, st);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, module, QP_TIME_STRUCT_TIME_PROP, st, 0) < 0)
        return -1;
    return 0;
}

static const QPModuleDef qp_moddef_time = {
    "time",
    qp_time_doc_module,
    qp_time_methods,
    qp_time_exec,
};

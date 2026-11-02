"""L0 golden: datetime with fixed values only (never the current time)."""
from datetime import date, datetime, time, timedelta, timezone

d = date(2024, 2, 28)
print("date", d, repr(d), d.year, d.month, d.day)
print("plus 1 day leap", d + timedelta(days=1), d + timedelta(days=2))
print("weekday", d.weekday(), d.isoweekday(), d.isocalendar(), tuple(d.isocalendar()))
print("isoformat", d.isoformat(), str(date(5, 1, 9)))
print("fromisoformat", date.fromisoformat("2023-12-31"), date.fromisoformat("20231231"))
print("ordinal", d.toordinal(), date.fromordinal(1), date.fromordinal(738000))
print("replace", d.replace(year=2023, day=1))
print("diff", date(2025, 1, 1) - date(2024, 1, 1), (date(2024, 3, 1) - date(2024, 2, 1)).days)
print("compare", date(2024, 1, 1) < date(2024, 1, 2), max(date(2020, 5, 5), date(2019, 9, 9)))
print("min/max", date.min, date.max, date.resolution)
for bad in [(2023, 2, 29), (2024, 13, 1), (0, 1, 1)]:
    try:
        date(*bad)
    except ValueError as e:
        print("bad date", bad, type(e).__name__, e)
try:
    date.max + timedelta(days=1)
except OverflowError as e:
    print("overflow", type(e).__name__, e)

dt = datetime(2024, 3, 9, 14, 5, 7, 123456)
print("datetime", dt, repr(dt))
print("parts", dt.date(), dt.time(), dt.hour, dt.minute, dt.second, dt.microsecond)
print("isoformat", dt.isoformat(), dt.isoformat(sep=" ", timespec="minutes"), dt.isoformat(timespec="milliseconds"))
print("strftime", dt.strftime("%Y-%m-%d %H:%M:%S.%f"), dt.strftime("%a %A %b %B %j %p %I %y %%"))
print("strftime week", dt.strftime("%U %W %w %u"), dt.strftime("%G-W%V-%u"))
print("strptime", datetime.strptime("2021-07-04 09:30", "%Y-%m-%d %H:%M"))
print("strptime month name", datetime.strptime("5 Jan 1999", "%d %b %Y"))
try:
    datetime.strptime("2021-13-01", "%Y-%m-%d")
except ValueError as e:
    print("strptime bad", type(e).__name__, e)
print("fromisoformat", datetime.fromisoformat("2011-11-04T00:05:23.283+00:00"))
print("fromisoformat Z", datetime.fromisoformat("2011-11-04T00:05:23Z"))
print("fromisoformat basic", datetime.fromisoformat("20111104T000523"))
try:
    datetime.fromisoformat("2011-11-04 25:00")
except ValueError as e:
    print("fromisoformat bad", type(e).__name__, e)
print("combine", datetime.combine(date(2000, 1, 1), time(12, 30)))
print("arith", dt + timedelta(hours=12), dt - timedelta(weeks=1))
print("sub datetimes", datetime(2024, 1, 1) - datetime(2023, 12, 31, 18))
print("replace", dt.replace(hour=0, minute=0, second=0, microsecond=0))

# timezone-aware
utc = timezone.utc
aware = datetime(2024, 6, 1, 12, 0, tzinfo=utc)
print("aware", aware, repr(aware), aware.tzname(), aware.utcoffset())
plus = timezone(timedelta(hours=5, minutes=30), "IST")
print("astimezone", aware.astimezone(plus), aware.astimezone(plus).tzname())
minus = timezone(-timedelta(hours=8))
print("negative tz", aware.astimezone(minus), minus, repr(minus))
print("aware eq", aware == aware.astimezone(plus), aware.astimezone(plus) - aware)
print("timestamp", aware.timestamp(), datetime.fromtimestamp(0, utc), datetime.fromtimestamp(1e9, tz=utc))
print("utc repr", repr(utc), str(utc), timezone(timedelta(0)) == utc)
try:
    aware - datetime(2024, 6, 1)
except TypeError as e:
    print("naive vs aware", type(e).__name__, e)
try:
    timezone(timedelta(hours=24))
except ValueError as e:
    print("bad offset", type(e).__name__, e)
print("strftime %z", aware.astimezone(plus).strftime("%z %Z"))

# timedelta
td = timedelta(days=1, hours=2, minutes=3, seconds=4, microseconds=5)
print("timedelta", td, repr(td))
print("total_seconds", td.total_seconds(), timedelta(milliseconds=1500).total_seconds())
print("normalized", repr(timedelta(seconds=-1)), timedelta(seconds=-1), timedelta(hours=-25))
print("components", td.days, td.seconds, td.microseconds)
print("arith", td * 2, td / 2, td // timedelta(hours=1), td % timedelta(hours=1), -td, abs(-td))
print("ratio", timedelta(hours=3) / timedelta(minutes=45))
print("str zero/large", timedelta(0), timedelta(days=400, seconds=1), repr(timedelta(0)))
print("float args", timedelta(days=0.5), timedelta(seconds=1.25))
print("min/max", timedelta.min, timedelta.max, timedelta.resolution)
print("compare", timedelta(hours=1) < timedelta(minutes=61), bool(timedelta(0)))
try:
    timedelta(days=10**9)
except OverflowError as e:
    print("timedelta overflow", type(e).__name__, e)

t = time(23, 59, 59, 999999)
print("time", t, repr(t), t.isoformat(timespec="seconds"), time.fromisoformat("04:23:01.000384"))
print("time min", time.min, time(7), time(7, 5).strftime("%H:%M %p"))

"""Расписание запланированных задач: как в диалоге «Add Scheduled Task» оригинала.

Режимы: every (каждые N минут/часов/дней от первого запуска), weekly (дни недели, каждые N недель),
monthly (число месяца или последний день, каждые N месяцев), once (однократно).
Время локальное, без часовых поясов.
"""
import calendar
from datetime import date, datetime, timedelta

MODES = ("every", "weekly", "monthly", "once")
UNITS = {"minutes": 60, "hours": 3600, "days": 86400}
_ANCHOR_MONDAY = date(2000, 1, 3)
ISO = "%Y-%m-%dT%H:%M"


def parse_time(s):
    try:
        h, m = str(s).split(":")
        h, m = int(h), int(m)
    except ValueError:
        raise ValueError(f"время должно быть в формате ЧЧ:ММ: {s!r}")
    if not (0 <= h < 24 and 0 <= m < 60):
        raise ValueError(f"время вне диапазона: {s!r}")
    return h, m


def parse_dt(s):
    try:
        return datetime.strptime(s[:16], ISO)
    except (TypeError, ValueError):
        raise ValueError(f"дата/время должны быть в формате ГГГГ-ММ-ДДTЧЧ:ММ: {s!r}")


def validate_schedule(sp):
    if not sp.get("enabled"):
        return
    mode = sp.get("mode")
    if mode not in MODES:
        raise ValueError(f"неизвестный режим расписания: {mode!r}")
    if mode in ("weekly", "monthly"):
        parse_time(sp.get("start_time", "12:00"))
    if mode == "every":
        ev = sp.get("every") or {}
        if ev.get("unit") not in UNITS or int(ev.get("value", 0)) < 1:
            raise ValueError("интервал: укажите число >= 1 и единицу minutes/hours/days")
        parse_dt(sp.get("first_time"))
    elif mode == "weekly":
        wd = sp.get("weekdays")
        if not wd or any(d not in range(7) for d in wd):
            raise ValueError("выберите хотя бы один день недели (0=Пн ... 6=Вс)")
        if int(sp.get("week_interval", 1)) < 1:
            raise ValueError("интервал недель должен быть >= 1")
    elif mode == "monthly":
        day = sp.get("day", 1)
        if day != "last" and not (isinstance(day, int) and 1 <= day <= 31):
            raise ValueError("число месяца: 1..31 или 'last'")
        if int(sp.get("month_interval", 1)) < 1:
            raise ValueError("интервал месяцев должен быть >= 1")
    else:
        parse_dt(sp.get("at"))


def next_run(sp, after):
    """Ближайший запуск строго позже after (datetime) или None, если расписание не срабатывает."""
    if not sp.get("enabled"):
        return None
    mode = sp["mode"]
    if mode == "once":
        at = parse_dt(sp["at"])
        return at if at > after else None
    if mode == "every":
        ev = sp["every"]
        step = timedelta(seconds=UNITS[ev["unit"]] * int(ev["value"]))
        first = parse_dt(sp["first_time"])
        if after < first:
            return first
        n = (after - first) // step + 1
        return first + step * n
    h, m = parse_time(sp.get("start_time", "12:00"))
    if mode == "weekly":
        interval = int(sp.get("week_interval", 1))
        anchor = date.fromisoformat(sp["anchor"]) if sp.get("anchor") else _ANCHOR_MONDAY
        anchor -= timedelta(days=anchor.weekday())
        d = after.date()
        for _ in range(7 * interval * 2 + 8):
            if d.weekday() in sp["weekdays"] and ((d - anchor).days // 7) % interval == 0:
                cand = datetime(d.year, d.month, d.day, h, m)
                if cand > after:
                    return cand
            d += timedelta(days=1)
        return None
    interval = int(sp.get("month_interval", 1))
    day = sp.get("day", 1)
    idx = after.year * 12 + after.month - 1
    for k in range(0, interval * 2 + 14):
        i = idx + k
        if (i - (2000 * 12)) % interval:
            continue
        y, mo = divmod(i, 12)
        mo += 1
        last = calendar.monthrange(y, mo)[1]
        dd = last if day == "last" else min(day, last)
        cand = datetime(y, mo, dd, h, m)
        if cand > after:
            return cand
    return None


def describe(sp):
    if not sp.get("enabled"):
        return "disabled"
    mode = sp["mode"]
    if mode == "every":
        ev = sp["every"]
        return f"every {ev['value']} {ev['unit']} from {sp['first_time'].replace('T', ' ')}"
    if mode == "weekly":
        names = [calendar.day_abbr[d] for d in sorted(sp["weekdays"])]
        n = int(sp.get("week_interval", 1))
        return f"{'every week' if n == 1 else f'every {n} weeks'} on {','.join(names)} at {sp.get('start_time', '12:00')}"
    if mode == "monthly":
        n = int(sp.get("month_interval", 1))
        return f"{'every month' if n == 1 else f'every {n} months'} on {sp.get('day', 1)} at {sp.get('start_time', '12:00')}"
    return f"once at {sp['at'].replace('T', ' ')}"

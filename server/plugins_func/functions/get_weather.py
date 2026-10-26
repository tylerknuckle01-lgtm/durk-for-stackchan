import httpx
from config.logger import setup_logging
from plugins_func.register import register_function, ToolType, ActionResponse, Action
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

TAG = __name__
logger = setup_logging()

GET_WEATHER_FUNCTION_DESC = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": (
            "Get current weather and the short-term forecast for a location. "
            "Use when the user asks about weather, temperature, rain, snow, wind, etc. "
            "If the user names a city, pass it as location (e.g. 'Boulder, CO'). "
            "If no location is given, leave location empty to use the default home location. "
            "Results are in Fahrenheit and mph."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "location": {
                    "type": "string",
                    "description": "City name, e.g. 'Austin, Texas'. Optional; defaults to the home location.",
                },
                "lang": {
                    "type": "string",
                    "description": "Response language code, default en_US.",
                },
            },
            "required": [],
        },
    },
}

HEADERS = {"User-Agent": "DurkStackchan/1.0 (home weather plugin)", "Accept": "application/geo+json, application/json"}
import os
DEFAULT_LOC = os.environ.get("DURK_LOCATION", "New York, NY")

WMO = {0: "clear", 1: "mostly clear", 2: "partly cloudy", 3: "overcast", 45: "fog", 48: "freezing fog",
       51: "light drizzle", 53: "drizzle", 55: "heavy drizzle", 56: "freezing drizzle", 57: "freezing drizzle",
       61: "light rain", 63: "rain", 65: "heavy rain", 66: "freezing rain", 67: "freezing rain",
       71: "light snow", 73: "snow", 75: "heavy snow", 77: "snow grains", 80: "rain showers",
       81: "rain showers", 82: "heavy rain showers", 85: "snow showers", 86: "heavy snow showers",
       95: "thunderstorms", 96: "thunderstorms with hail", 99: "thunderstorms with hail"}

US_STATES = {"alabama":"AL","alaska":"AK","arizona":"AZ","arkansas":"AR","california":"CA","colorado":"CO","connecticut":"CT","delaware":"DE","florida":"FL","georgia":"GA","hawaii":"HI","idaho":"ID","illinois":"IL","indiana":"IN","iowa":"IA","kansas":"KS","kentucky":"KY","louisiana":"LA","maine":"ME","maryland":"MD","massachusetts":"MA","michigan":"MI","minnesota":"MN","mississippi":"MS","missouri":"MO","montana":"MT","nebraska":"NE","nevada":"NV","new hampshire":"NH","new jersey":"NJ","new mexico":"NM","new york":"NY","north carolina":"NC","north dakota":"ND","ohio":"OH","oklahoma":"OK","oregon":"OR","pennsylvania":"PA","rhode island":"RI","south carolina":"SC","south dakota":"SD","tennessee":"TN","texas":"TX","utah":"UT","vermont":"VT","virginia":"VA","washington":"WA","west virginia":"WV","wisconsin":"WI","wyoming":"WY"}
ABBR = {v.lower(): k for k, v in US_STATES.items()}


async def geocode(client, location):
    parts = [p.strip() for p in location.split(",")]
    name = parts[0]
    region = parts[1].lower() if len(parts) > 1 else None
    if region in ABBR:
        region = ABBR[region]
    r = await client.get("https://geocoding-api.open-meteo.com/v1/search",
                         params={"name": name, "count": 10, "language": "en", "format": "json"})
    results = r.json().get("results") or []
    if not results:
        return None
    def score(x):
        s = 0
        if x.get("country_code") == "US":
            s += 2
        if region and (x.get("admin1", "").lower() == region or x.get("country", "").lower() == region):
            s += 5
        return s
    best = max(results, key=score)  # max is stable: keeps population order among ties
    label = best["name"] + (", " + best["admin1"] if best.get("admin1") else "")
    if best.get("country_code") != "US":
        label += ", " + best.get("country", "")
    return best["latitude"], best["longitude"], label, best.get("country_code") == "US"


async def build_report(location):
    async with httpx.AsyncClient(timeout=httpx.Timeout(10.0, connect=5.0), headers=HEADERS, follow_redirects=True) as client:
        geo = await geocode(client, location)
        if not geo:
            return None
        lat, lon, label, is_us = geo
        r = await client.get("https://api.open-meteo.com/v1/forecast", params={
            "latitude": lat, "longitude": lon,
            "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,wind_gusts_10m",
            "daily": "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max",
            "temperature_unit": "fahrenheit", "wind_speed_unit": "mph", "timezone": "auto", "forecast_days": 3})
        om = r.json()
        c = om["current"]
        lines = [f"Weather for {label}.",
                 f"Now: {round(c['temperature_2m'])}°F (feels like {round(c['apparent_temperature'])}°F), "
                 f"{WMO.get(c['weather_code'], 'unknown conditions')}, humidity {c['relative_humidity_2m']}%, "
                 f"wind {round(c['wind_speed_10m'])} mph, gusts {round(c['wind_gusts_10m'])} mph."]
        nws_ok = False
        if is_us:
            try:
                p = await client.get(f"https://api.weather.gov/points/{lat:.4f},{lon:.4f}")
                p.raise_for_status()
                f = await client.get(p.json()["properties"]["forecast"])
                f.raise_for_status()
                periods = f.json()["properties"]["periods"][:4]
                lines.append("Forecast (National Weather Service):")
                for per in periods:
                    lines.append(f"{per['name']}: {per['shortForecast']}, {'high' if per['isDaytime'] else 'low'} "
                                 f"{per['temperature']}°F, wind {per['windSpeed']} {per['windDirection']}.")
                nws_ok = True
            except Exception as e:
                logger.bind(tag=TAG).warning(f"NWS forecast failed, using Open-Meteo: {e}")
        if not nws_ok:
            d = om["daily"]
            names = ["Today", "Tomorrow", "Day after"]
            lines.append("Forecast:")
            for i in range(min(3, len(d["time"]))):
                pp = d["precipitation_probability_max"][i]
                lines.append(f"{names[i]}: {WMO.get(d['weather_code'][i], 'mixed')}, high {round(d['temperature_2m_max'][i])}°F, "
                             f"low {round(d['temperature_2m_min'][i])}°F" + (f", {pp}% chance of precipitation." if pp is not None else "."))
        lines.append("Answer the user's actual question briefly and conversationally in English using Fahrenheit; don't read every detail.")
        return "\n".join(lines)


@register_function("get_weather", GET_WEATHER_FUNCTION_DESC, ToolType.SYSTEM_CTL)
async def get_weather(conn: "ConnectionHandler", location: str = None, lang: str = "en_US"):
    from core.utils.cache.manager import cache_manager, CacheType

    cfg = conn.config.get("plugins", {}).get("get_weather", {}) if conn else {}
    default_location = cfg.get("default_location") or DEFAULT_LOC
    if not default_location.isascii():
        default_location = DEFAULT_LOC
    location = (location or "").strip() or default_location

    key = f"us_weather_{location.lower()}"
    cached = cache_manager.get(CacheType.WEATHER, key)
    if cached:
        return ActionResponse(Action.REQLLM, cached, None)
    try:
        report = await build_report(location)
    except Exception as e:
        logger.bind(tag=TAG).error(f"Weather lookup failed: {e}")
        return ActionResponse(Action.REQLLM, "Sorry, the weather service isn't responding right now.", None)
    if not report:
        return ActionResponse(Action.REQLLM, f"I couldn't find a place called {location}. Ask the user to check the city name.", None)
    cache_manager.set(CacheType.WEATHER, key, report)
    return ActionResponse(Action.REQLLM, report, None)

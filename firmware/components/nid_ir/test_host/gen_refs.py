#!/usr/bin/env python3
"""Génère refs_data.h : heures de lever et de coucher du soleil de référence (astral)
pour les tests sur PC de nid_ir (voir Makefile).

Utilisation (le résultat est versionné : ce script n'est à relancer que pour changer
les lieux ou les dates) :

    pip install astral
    python3 gen_refs.py > refs_data.h        # ou : make refs

Deux références par lieu et par date :
  - « astral » : astral.sun.sunrise() / sunset(), modèle de réfraction d'astral
    (zénith effectif 90,789°) ; les tests exigent ±2 min ;
  - « noaa » : même algorithme NOAA avec exactement le zénith 90,833° du firmware
    (time_of_transit(..., with_refraction=False)) ; les tests exigent quelques secondes.

La date est celle du jour solaire local : jour civil en temps solaire moyen du lieu
(UTC + longitude/15 h), comme dans nid_ir_sun_times(). Les heures sont des secondes
Unix UTC. Un jour polaire (soleil toujours levé) vaut +1, une nuit polaire −1.
"""
import datetime as dt
import sys

import astral
from astral import Observer
from astral import sun
from astral.sun import SunDirection, time_of_transit

EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
ZENITH_NOAA = 90.833

# (identifiant C, nom, latitude, longitude)
PLACES = [
    ("CARAVELLE", "Presqu'île de la Caravelle (Martinique)", 14.77, -60.89),
    ("FORT_DE_FRANCE", "Fort-de-France (Martinique)", 14.60, -61.07),
    ("QUITO", "Quito (équateur)", -0.18, -78.47),
    ("PARIS", "Paris", 48.85, 2.35),
    ("SAN_FRANCISCO", "San Francisco : coucher après minuit UTC", 37.77, -122.42),
    ("SYDNEY", "Sydney : lever la veille en UTC", -33.87, 151.21),
    ("AUCKLAND", "Auckland", -36.85, 174.76),
    ("FIDJI", "Fidji, près de la ligne de changement de date", -17.00, 179.90),
    ("KIRITIMATI", "Kiritimati (UTC+14)", 1.87, -157.40),
    ("USHUAIA", "Ushuaia (hémisphère sud, haute latitude)", -54.80, -68.30),
    ("REYKJAVIK", "Reykjavik (64° N)", 64.15, -21.94),
    ("TROMSO", "Tromsø (69,6° N, soleil de minuit)", 69.65, 18.96),
    ("LONGYEARBYEN", "Longyearbyen (78° N)", 78.22, 15.65),
]

D = dt.date


def weekly(year):
    d = D(year, 1, 1)
    out = []
    while d.year == year:
        out.append(d)
        d += dt.timedelta(days=7)
    return out


# Dates par lieu. « polar » : régime attendu (+1 jour polaire, −1 nuit polaire) pour les
# dates choisies loin des transitions (près d'elles, les références divergent entre elles).
DATES = {
    "CARAVELLE": weekly(2026) + [
        D(2026, 3, 20), D(2026, 6, 21), D(2026, 9, 23), D(2026, 12, 21),
        D(2026, 12, 31), D(2027, 1, 1), D(2028, 2, 29),
        D(1969, 12, 31), D(1970, 1, 1), D(2000, 1, 1), D(2035, 6, 21), D(2060, 12, 21), D(2099, 6, 21),
    ],
    "FORT_DE_FRANCE": [D(2026, m, 1) for m in range(1, 13)],
    "QUITO": [D(2026, 3, 20), D(2026, 9, 22)],
    "PARIS": [D(2026, 3, 20), D(2026, 6, 21), D(2026, 9, 23), D(2026, 12, 21)],
    "SAN_FRANCISCO": [D(2026, 3, 20), D(2026, 6, 21), D(2026, 9, 23), D(2026, 12, 21)],
    "SYDNEY": [D(2026, 3, 20), D(2026, 6, 21), D(2026, 9, 23), D(2026, 12, 21)],
    "AUCKLAND": [D(2026, 1, 15), D(2026, 7, 15)],
    "FIDJI": [D(2026, 3, 20), D(2026, 12, 21)],
    "KIRITIMATI": [D(2026, 3, 20), D(2026, 12, 21)],
    "USHUAIA": [D(2026, 6, 21), D(2026, 12, 21)],
    "REYKJAVIK": [D(2026, 3, 20), D(2026, 6, 21), D(2026, 9, 23), D(2026, 12, 21)],
    "TROMSO": [D(2026, 3, 20), D(2026, 9, 23), D(2026, 6, 21), D(2026, 12, 21)],
    "LONGYEARBYEN": [D(2026, 3, 20), D(2026, 9, 22), D(2026, 6, 21), D(2026, 12, 21)],
}


def unix(t):
    return int((t - EPOCH).total_seconds())


def solar_tz(longitude):
    """Temps solaire moyen du lieu : UTC + longitude/15 h (arrondi à la seconde, comme le firmware)."""
    return dt.timezone(dt.timedelta(seconds=round(longitude * 240)))


def refs_for(latitude, longitude, date):
    """(polar, lever_astral, coucher_astral, lever_noaa, coucher_noaa)"""
    tz = solar_tz(longitude)
    obs = Observer(latitude=latitude, longitude=longitude, elevation=0)
    try:
        rise = unix(sun.sunrise(obs, date, tz))
        sset = unix(sun.sunset(obs, date, tz))
    except ValueError as exc:
        text = str(exc)
        if "always above" in text:
            return (1, 0, 0, 0, 0)
        if "always below" in text:
            return (-1, 0, 0, 0, 0)
        raise SystemExit(f"astral ne trouve pas de lever/coucher ({latitude}, {longitude}, {date}) : {text}")

    noaa = []
    for direction in (SunDirection.RISING, SunDirection.SETTING):
        t = time_of_transit(obs, date, ZENITH_NOAA, direction, with_refraction=False).astimezone(tz)
        if t.date() != date:   # même correction de date qu'astral.sun.sunrise()
            shift = dt.timedelta(days=1) if t.date() < date else dt.timedelta(days=-1)
            t = time_of_transit(obs, date + shift, ZENITH_NOAA, direction, with_refraction=False).astimezone(tz)
            if t.date() != date:
                raise SystemExit(f"référence NOAA introuvable ({latitude}, {longitude}, {date}, {direction})")
        noaa.append(unix(t))
    return (0, rise, sset, noaa[0], noaa[1])


# Régime attendu des dates polaires (contrôle du générateur : astral doit être d'accord).
EXPECTED_POLAR = {
    ("TROMSO", D(2026, 6, 21)): 1, ("TROMSO", D(2026, 12, 21)): -1,
    ("LONGYEARBYEN", D(2026, 6, 21)): 1, ("LONGYEARBYEN", D(2026, 12, 21)): -1,
}


def main():
    out = sys.stdout
    out.write("/*\n")
    out.write(" * Données de référence des tests sur PC de nid_ir — FICHIER GÉNÉRÉ par gen_refs.py\n")
    out.write(f" * (astral {astral.__version__}). Ne pas modifier à la main.\n")
    out.write(" *\n")
    out.write(" * Heures en secondes Unix UTC ; date = jour solaire local (temps solaire moyen du lieu).\n")
    out.write(" * polar : 0 normal, +1 jour polaire (soleil toujours levé), −1 nuit polaire.\n")
    out.write(" */\n")
    out.write("#pragma once\n\n")
    out.write("typedef struct {\n    const char *id;\n    const char *name;\n    double latitude;\n    double longitude;\n} ref_place_t;\n\n")
    out.write("typedef struct {\n    int place;\n    int year, month, day;\n    int polar;\n")
    out.write("    long long astral_rise, astral_set;   /* astral.sun.sunrise() / sunset() */\n")
    out.write("    long long noaa_rise, noaa_set;       /* même algorithme, zénith 90,833° exact */\n} ref_entry_t;\n\n")

    out.write("enum {\n")
    for i, (ident, _, _, _) in enumerate(PLACES):
        out.write(f"    PLACE_{ident} = {i},\n")
    out.write("};\n\n")

    out.write("static const ref_place_t REF_PLACES[] = {\n")
    for ident, name, lat, lon in PLACES:
        out.write(f'    {{ "{ident}", "{name}", {lat}, {lon} }},\n')
    out.write("};\n\n")

    out.write("static const ref_entry_t REF_ENTRIES[] = {\n")
    count = 0
    for i, (ident, _, lat, lon) in enumerate(PLACES):
        for date in DATES[ident]:
            polar, ar, as_, nr, ns = refs_for(lat, lon, date)
            expected = EXPECTED_POLAR.get((ident, date))
            if expected is not None and expected != polar:
                raise SystemExit(f"régime polaire inattendu ({ident}, {date}) : {polar} au lieu de {expected}")
            if expected is None and polar != 0:
                raise SystemExit(f"date polaire non prévue ({ident}, {date}) : {polar}")
            out.write(f"    {{ PLACE_{ident}, {date.year}, {date.month}, {date.day}, {polar}, {ar}LL, {as_}LL, {nr}LL, {ns}LL }},\n")
            count += 1
    out.write("};\n")
    sys.stderr.write(f"{count} entrées écrites\n")


if __name__ == "__main__":
    main()

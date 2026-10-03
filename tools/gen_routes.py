"""Build routes.tsv (countries, cities, companies, cargoes) from the user's own extracted ETS2 defs.

Usage: gen_routes.py <extract_root> <out.tsv>
<extract_root> holds one folder per archive (def/, dlc_east/, ...), each extracted with scs_extractor.

Lines (tab-separated):
  N country_token name
  C city_token name country_token
  P company_token company_name city_token     (company present in city)
  O company_token cargo_token                (company ships it)
  I company_token cargo_token                (company receives it)
  G cargo_token name est_mass_kg
"""
import re, sys
from pathlib import Path

root, out = Path(sys.argv[1]), Path(sys.argv[2])
defs = [p for p in root.glob("*/**/def") if (p / "city").is_dir() or (p / "company").is_dir() or (p / "cargo").is_dir() or (p / "country").is_dir()]

def read(p): return p.read_text("utf-8", "replace")
def fields(text, name): return re.findall(rf"^\s*{name}\s*:\s*\"?([^\"\r\n]*?)\"?\s*$", text, re.M)
def num(text, name):
    v = field(text, name)
    try: return float(v.split()[0]) if v else 0.0
    except ValueError: return 0.0
def field(text, name):
    m = re.search(rf"^\s*{name}\s*:\s*\"?([^\"\r\n]*)\"?", text, re.M)
    return m.group(1).strip() if m else None
def pretty(tok): return tok.replace("_", " ").strip().capitalize()

# the game's own Portuguese strings (locale.scs extracted to <root>/locale/locale/pt_br)
loc = {}
for f in root.glob("locale/locale/pt_br/*.sui"):
    loc.update(re.findall(r'key\[\]:\s*"([^"]*)"\s*val\[\]:\s*"([^"]*)"', read(f)))
def tr(text, fallback):
    m = re.fullmatch(r"@@(\w+)@@", text or "")
    out = loc.get(m.group(1), fallback) if m else (text or fallback)
    return out.replace("\\n", " ").strip()  # some UI strings carry a literal \n

countries, cities, companies, cargo = {}, {}, {}, {}
spec, trailers = {}, []  # cargo -> (body types, unit mass, unit volume); single-trailer (body, volume, payload)
place, ship, recv = set(), set(), set()
for d in defs:
    for f in d.glob("country/*.sui"):
        t = read(f)
        m = re.search(r"country\.data\.(\w+)", t)
        if m: countries[m.group(1)] = tr(field(t, "name_localized"), field(t, "name") or pretty(m.group(1)))
    for f in d.glob("city/*.sui"):
        t = read(f)
        m = re.search(r"city\.(\w+)", t)
        if m and field(t, "country"): cities[m.group(1)] = (tr(field(t, "city_name_localized"), field(t, "city_name") or pretty(m.group(1))), field(t, "country"))
    for f in d.glob("company/*.sui"):
        t = read(f)
        co = f.name.split(".")[0]  # "acc.sui" or "sag_tre_pln.dlc_polar.sui"
        companies[co] = field(t, "name") or companies.get(co) or co.upper()
    for f in d.glob("company/*/editor/*.sii"):
        city = field(read(f), "city")
        if city: place.add((f.parent.parent.name, city))
    for kind, bucket in (("out", ship), ("in", recv)):
        for f in d.glob(f"company/*/{kind}/*.sii"):
            m = re.search(r"cargo\.(\w+)", read(f))
            if m: bucket.add((f.parent.parent.name, m.group(1)))
    for f in d.glob("cargo/*.sui"):
        t = read(f)
        m = re.search(r"cargo\.(\w+)", t)
        if m:
            cargo[m.group(1)] = tr(field(t, "name"), pretty(m.group(1)))
            spec[m.group(1)] = (set(fields(t, r"body_types\[\]")), num(t, "mass"), num(t, "volume"))
    for f in d.glob("vehicle/trailer_defs/*.sii"):
        t = read(f)
        if (field(t, "chain_type") or "single") == "single" and field(t, "body_type"):
            payload = num(t, "gross_trailer_weight_limit") - num(t, "chassis_mass") - num(t, "body_mass")
            trailers.append((field(t, "body_type"), num(t, "volume"), payload))

# ponytail: the heaviest load one standard single trailer takes; the game may pick another trailer
def est_mass(c):
    bodies, mass, vol = spec.get(c, (set(), 0.0, 0.0))
    loads = [min(int(tv // vol), int(pl // mass)) * mass for b, tv, pl in trailers if b in bodies and mass > 0 and vol > 0]
    return round(max(loads, default=0) or mass)

lines = [f"N\t{k}\t{v}" for k, v in sorted(countries.items())]
lines += [f"C\t{k}\t{n}\t{c}" for k, (n, c) in sorted(cities.items()) if c in countries]
lines += [f"P\t{co}\t{companies.get(co, co.upper())}\t{ci}" for co, ci in sorted(place) if ci in cities]
lines += [f"O\t{co}\t{cg}" for co, cg in sorted(ship) if cg in cargo]
lines += [f"I\t{co}\t{cg}" for co, cg in sorted(recv) if cg in cargo]
lines += [f"G\t{k}\t{v}\t{est_mass(k)}" for k, v in sorted(cargo.items())]
out.write_text("\n".join(lines) + "\n", "utf-8")
print(f"{len(countries)} países, {len(cities)} cidades, {len(place)} filiais, {len(cargo)} cargas -> {out}")

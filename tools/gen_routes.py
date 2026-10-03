"""Build routes.tsv (countries, cities, companies, cargoes) from the user's own extracted ETS2 defs.

Usage: gen_routes.py <extract_root> <out.tsv>
<extract_root> holds one folder per archive (def/, dlc_east/, ...), each extracted with scs_extractor.

Lines (tab-separated):
  N country_token name
  C city_token name country_token
  P company_token company_name city_token     (company present in city)
  O company_token cargo_token                (company ships it)
  I company_token cargo_token                (company receives it)
  G cargo_token name
"""
import re, sys
from pathlib import Path

root, out = Path(sys.argv[1]), Path(sys.argv[2])
defs = [p for p in root.glob("*/**/def") if (p / "city").is_dir() or (p / "company").is_dir() or (p / "cargo").is_dir() or (p / "country").is_dir()]

def read(p): return p.read_text("utf-8", "replace")
def field(text, name):
    m = re.search(rf"^\s*{name}\s*:\s*\"?([^\"\r\n]*)\"?", text, re.M)
    return m.group(1).strip() if m else None
def pretty(tok): return tok.replace("_", " ").strip().capitalize()

countries, cities, companies, cargo = {}, {}, {}, {}
place, ship, recv = set(), set(), set()
for d in defs:
    for f in d.glob("country/*.sui"):
        t = read(f)
        m = re.search(r"country\.data\.(\w+)", t)
        if m: countries[m.group(1)] = field(t, "name") or pretty(m.group(1))
    for f in d.glob("city/*.sui"):
        t = read(f)
        m = re.search(r"city\.(\w+)", t)
        if m and field(t, "country"): cities[m.group(1)] = (field(t, "city_name") or pretty(m.group(1)), field(t, "country"))
    for f in d.glob("company/*.sui"):
        t = read(f)
        companies[f.stem] = field(t, "name") or companies.get(f.stem) or f.stem.upper()
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
            n = field(t, "name") or ""
            cargo[m.group(1)] = pretty(m.group(1)) if n.startswith("@@") or not n else n

lines = [f"N\t{k}\t{v}" for k, v in sorted(countries.items())]
lines += [f"C\t{k}\t{n}\t{c}" for k, (n, c) in sorted(cities.items()) if c in countries]
lines += [f"P\t{co}\t{companies.get(co, co.upper())}\t{ci}" for co, ci in sorted(place) if ci in cities]
lines += [f"O\t{co}\t{cg}" for co, cg in sorted(ship) if cg in cargo]
lines += [f"I\t{co}\t{cg}" for co, cg in sorted(recv) if cg in cargo]
lines += [f"G\t{k}\t{v}" for k, v in sorted(cargo.items())]
out.write_text("\n".join(lines) + "\n", "utf-8")
print(f"{len(countries)} países, {len(cities)} cidades, {len(place)} filiais, {len(cargo)} cargas -> {out}")

#!/usr/bin/env python3
"""Search JLCPCB's parts library from the command line, with stock and library type.

Why not the website: jlcpcb.com/partdetail renders a part that has zero stock behind
it exactly like one that has thousands, and the parts library is a SPA so fetching
the page gets you the shell. Picking a substitute off a part page is how you end up
specifying something unbuyable -- it happened on pdm14 with KF350-3.5, whose 4P and
6P are both at zero while its 2P/3P are stocked.

componentLibraryType is the other thing worth seeing before choosing: "base" parts
are already on JLC's machines, "expand" ones cost a per-part feeder setup fee, which
on a small run can be a large fraction of the build cost.

Usage:
  python jlc_search.py "DB125-3.5"                    # one family
  python jlc_search.py --batch queries.txt            # one query per line
  python jlc_search.py "100nF 0603 50V X7R" --min-stock 5000 --base-first
"""
import argparse
import json
import sys
import urllib.request

URL = ("https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/"
       "selectSmtComponentList")


def search(keyword, page_size=60):
    body = json.dumps({"currentPage": 1, "pageSize": page_size,
                       "keyword": keyword, "searchSource": "search"}).encode()
    req = urllib.request.Request(
        URL, data=body,
        headers={"Content-Type": "application/json", "User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=45) as r:
        d = json.loads(r.read().decode("utf-8", "replace"))
    return ((d.get("data") or {}).get("componentPageInfo", {}).get("list") or [])


def price_of(c):
    ps = c.get("componentPrices") or []
    return ps[0].get("productPrice") if ps else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keyword", nargs="?")
    ap.add_argument("--batch", help="file with one query per line; '#' comments")
    ap.add_argument("--min-stock", type=int, default=0)
    ap.add_argument("--base-first", action="store_true",
                    help="sort Basic/Preferred parts to the top")
    ap.add_argument("--limit", type=int, default=12)
    a = ap.parse_args()

    queries = []
    if a.batch:
        for line in open(a.batch, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                queries.append(line)
    if a.keyword:
        queries.append(a.keyword)
    if not queries:
        ap.error("give a keyword or --batch")

    for q in queries:
        print(f"##### {q}")
        try:
            rows = search(q)
        except Exception as e:
            print(f"   query failed: {e}")
            continue
        rows = [c for c in rows if (c.get("stockCount") or 0) >= a.min_stock]
        if a.base_first:
            rows.sort(key=lambda c: (c.get("componentLibraryType") != "base",
                                     -(c.get("stockCount") or 0)))
        else:
            rows.sort(key=lambda c: -(c.get("stockCount") or 0))
        if not rows:
            print("   (nothing in stock)")
        for c in rows[:a.limit]:
            lib = "BASIC" if c.get("componentLibraryType") == "base" else "ext"
            desc = str(c.get("describe") or "").encode("ascii", "replace").decode()
            pr = price_of(c)
            print(f"  {c.get('componentCode'):>11} {lib:5} "
                  f"{str(c.get('componentModelEn'))[:30]:30} "
                  f"stock={c.get('stockCount'):<7} "
                  f"${pr if pr is not None else '?'}  {desc[:74]}")
        print()


if __name__ == "__main__":
    sys.exit(main())

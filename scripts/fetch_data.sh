#!/usr/bin/env bash
# Downloads the public market data used by mcb_report into data/raw/ (not
# committed to the repository).
#
#   scripts/fetch_data.sh [output-dir]
#
# Every source is independent: if a download fails, the previously downloaded
# file (if any) is kept and the report marks that source as stale or missing.
#
#   Eris Innovations   SOFR par swap curve + discount factors (end of day)
#   US Treasury        daily par yield curve (FRED as a fallback)
#   Federal Reserve    GSW fitted yield curve history since 1961
#   New York Fed       latest SOFR fixing
#   Moscow Exchange    G-curve, OFZ bond prices, rate futures (ISS API)
set -u

OUT="${1:-data/raw}"
UA="curl (multicurve-bootstrap course project)"
mkdir -p "$OUT/eris" "$OUT/treasury" "$OUT/fed" "$OUT/nyfed" "$OUT/moex"
failures=0

# fetch URL DEST [MIN_BYTES] [TIMEOUT_SECONDS]
fetch() {
  local url="$1" dest="$2" min="${3:-200}" timeout="${4:-90}" tmp="$2.part"
  if curl -fsSL --retry 2 --retry-delay 3 -m "$timeout" -A "$UA" -o "$tmp" "$url" \
     && [ "$(wc -c < "$tmp")" -ge "$min" ]; then
    mv "$tmp" "$dest"
    echo "ok    $(basename "$dest")"
  else
    rm -f "$tmp"
    echo "FAIL  $url"
    failures=$((failures + 1))
    return 1
  fi
}

echo "== Eris SOFR swap curve"
ERIS="https://files.erisfutures.com/ftp"
if fetch "$ERIS/" "$OUT/eris/index.html" 1000; then
  # all par-curve files on the server (about three months of history), newest last
  grep -o 'Eris_[0-9]\{8\}_EOD_ParCouponCurve_SOFR\.csv' "$OUT/eris/index.html" | sort -u > "$OUT/eris/files.txt"
  while read -r f; do
    [ -s "$OUT/eris/$f" ] || fetch "$ERIS/$f" "$OUT/eris/$f" 500 30
  done < "$OUT/eris/files.txt"
  latest="$(tail -n 1 "$OUT/eris/files.txt" | grep -o '[0-9]\{8\}')"
  if [ -n "$latest" ]; then
    df="Eris_${latest}_EOD_DiscountFactors_SOFR.csv"
    [ -s "$OUT/eris/$df" ] || fetch "$ERIS/$df" "$OUT/eris/$df" 10000 120
    # keep only the newest discount-factor file (they are large)
    find "$OUT/eris" -name 'Eris_*_EOD_DiscountFactors_SOFR.csv' ! -name "$df" -delete
  fi
  rm -f "$OUT/eris/index.html" "$OUT/eris/files.txt"
fi

echo "== US Treasury par yield curve"
year="$(date -u +%Y)"
tsy_ok=0
for y in "$year" "$((year - 1))"; do
  url="https://home.treasury.gov/resource-center/data-chart-center/interest-rates/daily-treasury-rates.csv/${y}/all?type=daily_treasury_yield_curve&field_tdr_date_value=${y}&page&_format=csv"
  if fetch "$url" "$OUT/treasury/par_yields_${y}.csv" 300 120; then tsy_ok=1; fi
done
if [ "$tsy_ok" -eq 0 ]; then
  echo "   Treasury site unavailable, trying FRED"
  start="$(date -u -d '400 days ago' +%Y-%m-%d 2>/dev/null || echo "$((year - 1))-01-01")"
  fetch "https://fred.stlouisfed.org/graph/fredgraph.csv?id=DGS1MO,DGS3MO,DGS6MO,DGS1,DGS2,DGS3,DGS5,DGS7,DGS10,DGS20,DGS30&cosd=${start}" \
        "$OUT/treasury/fred_par_yields.csv" 300 60
fi

echo "== Federal Reserve GSW yield curve history"
fetch "https://www.federalreserve.gov/data/yield-curve-tables/feds200628.csv" "$OUT/fed/feds200628.csv" 1000000 600

echo "== New York Fed SOFR"
fetch "https://markets.newyorkfed.org/api/rates/secured/sofr/last/5.json" "$OUT/nyfed/sofr.json" 100 60

echo "== Moscow Exchange"
MOEX="https://iss.moex.com/iss"
fetch "$MOEX/engines/stock/zcyc.json?iss.meta=off" "$OUT/moex/zcyc.json" 500 60
fetch "$MOEX/engines/stock/markets/bonds/boards/TQOB/securities.json?iss.meta=off" "$OUT/moex/ofz.json" 2000 60
fetch "$MOEX/engines/futures/markets/forts/securities.json?iss.meta=off&iss.only=securities" "$OUT/moex/forts.json" 2000 60

echo "== done: $failures download(s) failed"
# Missing sources are handled by the report, so this script never fails the build.
exit 0

# Multicurve bootstrap: curves, prices, risk and forecasts in C++

**Live dashboard: <https://skindori24.github.io/Multicurve-Bootstrap-Stub/>**
(rebuilt automatically from public market data; see [Publishing](#publishing-the-dashboard) for the one-time setup)

A C++20 project for a quantitative-finance engineering course. It turns market
quotes into yield curves, prices swaps and bonds off those curves, measures
their risk, and uses sixty years of yield-curve history to put honest
uncertainty bands around forecasts. Everything shown on the dashboard is
computed by the code in this repository and re-checked on every run.

> **Not investment advice.** The BUY / SELL / HOLD labels are mechanical
> results of stated rules applied to public end-of-day data. They ignore
> transaction costs, liquidity, taxes and your circumstances.

## Two different "bootstraps"

The word means two unrelated things, and this project does both.

| | Curve bootstrapping | Statistical bootstrapping |
|---|---|---|
| Input | today's quotes (deposits, FRAs, futures, swaps, par bonds) | a long history of finished curves |
| Method | solve, one maturity at a time, for the discount factors that reprice every quote exactly | resample the history thousands of times |
| Randomness | none | all of it |
| Output | the curve you price with | uncertainty: p-values, confidence bands, scenario paths |
| Code | `src/curves`, `src/pricing` | `src/stats` |

```
market quotes ──► curve bootstrap ──► discount / projection curves ──► prices, DV01, bucket deltas
                                               │
history of curves (Fed GSW, 1972-today) ───────┴──► CG bootstrap ──► tests, forecasts, signals
```

The statistical part implements Crump & Gospodinov, *Deconstructing the Yield
Curve* (New York Fed Staff Report 884): instead of resampling yields, which
are persistent and mechanically linked across maturities, it resamples
"difference returns" and one long forward rate, then rebuilds every curve from
identities so each simulated history is internally consistent.

## Quick start

Requirements: a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022), CMake 3.24+,
`curl` and `bash` for the data download. Eigen and GoogleTest are downloaded
automatically by CMake if they are not installed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 102 unit tests, about 3 seconds

bash scripts/fetch_data.sh      # download today's public data into data/raw/
./build/mcb_report              # run every engine, write site/data/results.js
# then open site/index.html in a browser

./build/mcb_bench               # latency of curve builds and updates
```

`mcb_report` works even with no downloads at all: the EUR multicurve and the
statistical engine fall back to the reference data in `data/reference/`.

To price your own positions, edit `data/portfolio/usd_portfolio.csv` and rerun.

## What the dashboard shows

| Section | Content |
|---|---|
| Overview | headline rates, the trade-signal table with the reason for each signal, data freshness |
| USD curves | SOFR swap curve and Treasury curve bootstrapped from the day's quotes; zero and forward rates; swap spreads; a sample portfolio with prices, DV01 and bucketed risk |
| Forecast | fan charts of future yields from today's curve, forwards versus history-based expectations, one-year excess-return distributions |
| Paper replication | the expectations-hypothesis tests of Section 4.2 of the paper with bootstrap bands and p-values |
| EUR multicurve | OIS discounting with separate Euribor 3M and 6M projection curves, checked against QuantLib; risk example; latency |
| Moscow Exchange | the official G-curve, every fixed-coupon OFZ priced against it (rich/cheap), rates implied by RUONIA and key-rate futures |
| Validation | every check listed below, recomputed on each run |

## How it is validated

A curve engine is tested by checking that it reprices the market and matches
trusted references. Numbers below are from the run of 2026-11-16 (data of
2026-11-13); the dashboard shows the current values.

| What | Result |
|---|---|
| Holiday calendars (TARGET, US settlement, US government bond, SOFR) vs QuantLib | identical on every day 1990-2080 |
| Date rolling, day counts, schedules vs QuantLib | 2,500 + 6,000 + 800 random cases identical |
| EUR three-curve bootstrap vs QuantLib, same quotes | discount factors within 5e-13 (log-linear) and 3e-12 (log-cubic) |
| Swap and OIS values vs QuantLib | within 0.00001 on a notional of 1,000,000 |
| Bond yield, duration, convexity, Z-spread vs QuantLib | yield within 1e-11 |
| Every bootstrap instrument reprices | about 1e-13 |
| Eris's SOFR swaps repriced from Eris's own discount factors | within 5e-9 bp (a pure test of conventions) |
| Our SOFR zero rates vs Eris's published curve | within 0.13 bp |
| MOEX G-curve formula vs published yields | exact to the published four decimals |
| OFZ yields and spreads vs the exchange's own | yields within 0.15 bp, spreads within the 1 bp rounding |
| Bucket deltas add up to the parallel DV01; Jacobian vs bump-and-rebuild | within 0.2%; within 0.01% |
| Paper, Section 4.2 | the qualitative findings reproduce on the paper's sample (`EhTests.ReplicatesSection42OfThePaper`) |
| Speed (one desktop core) | full 3-curve, 90-instrument rebuild 1.6 ms; one quote tick 0.55 ms; pricing a swap 2 µs |

QuantLib is **not** needed to build or run anything. Its reference values were
generated once by `scripts/quantlib_reference.py` and are stored in
`tests/data/ql/`.

## How the signals are made, and what they are worth

**US Treasuries (duration).** For each maturity the engine simulates 10,000
one-year paths of the whole curve from today's curve and computes the excess
return of the bond over a one-year bill. It also regresses historical excess
returns on the level, slope and curvature of the curve. BUY needs a simulated
probability of a positive excess return of at least 60% *and* a positive
regression forecast; SELL needs at most 40% and a negative forecast.

**Individual OFZ bonds (relative value).** Each bond's Z-spread to the G-curve
is compared with bonds of similar coupon and duration. BUY/SELL needs a gap of
more than 1.25 cross-sectional standard deviations (at least 10 bp) that is
also worth at least 0.25 price points.

Limits you should know about:

- The bands are wide because the uncertainty is wide. A 60% probability is a
  mild tilt, not a prediction. The paper's own message is that textbook
  p-values overstate return predictability; the dashboard shows both.
- A yield is the return you get if you hold a bond to maturity. A forward rate
  is a break-even level, not a forecast: forwards contain a term premium, and
  the tests on the dashboard reject the idea that they predict future rates
  one-for-one.
- Data is end of day. Nothing here reacts intraday.
- No individual US Treasury issues: only the Treasury's par curve is public
  in machine-readable form, so US bonds are priced off that curve.
- The EUR multicurve runs on a frozen 2012 snapshot because free live EUR swap
  quotes do not exist. MOEX swap curves are subscriber-only and are not used.
- Simulated paths can go negative; no lower bound on rates is imposed.

## Project layout

```
include/mcb/, src/     the library (namespace mcb)
  core/                dates, calendars, day counts, schedules
  math/                root finding, curve interpolation
  curves/              yield curves, rate helpers, bootstrap, conventions, real-time engine
  pricing/             swaps, FRAs, bonds, risk (bump-and-rebuild and Jacobian)
  stats/               OLS/HAC, VAR(1) with bias correction, CG bootstrap, EH tests, forecasts
  data/                loaders: Eris SOFR, Treasury, Fed GSW, MOEX, EUR reference market
  report/              assembles the results document
apps/                  mcb_report (main pipeline), mcb_bench (latency)
tests/                 GoogleTest suite and QuantLib reference fixtures
data/reference/        frozen EUR market snapshot, month-end GSW extract
data/portfolio/        positions to price (edit me)
scripts/               fetch_data.sh, quantlib_reference.py
site/                  static dashboard (HTML/CSS/JS, no build step, no dependencies)
.github/workflows/     build, test, fetch, run, publish
```

## Data sources

| Source | Used for | Notes |
|---|---|---|
| [Eris Innovations](https://files.erisfutures.com/ftp/) | SOFR par swap curve, discount factors | downloaded at run time, not stored in this repository (no licence terms are published) |
| [US Treasury](https://home.treasury.gov/resource-center/data-chart-center/interest-rates) | daily par yield curve | public domain; FRED is used as a fallback |
| [Federal Reserve Board](https://www.federalreserve.gov/data/nominal-yield-curve.htm) | Gurkaynak-Sack-Wright yield curve since 1961 | staff research data; a month-end extract is kept in `data/reference/` |
| [New York Fed](https://www.newyorkfed.org/markets/reference-rates/sofr) | SOFR fixing | |
| [Moscow Exchange ISS](https://iss.moex.com/iss/reference/) | G-curve, OFZ prices, rate futures | free delayed data; check the exchange's terms before redistributing it |
| Ametrano & Bianchetti (2013) via the QuantLib cookbook | EUR quotes of 11 Dec 2012 | transcribed test data set used only for validation |

## Publishing the dashboard

The workflow in `.github/workflows/dashboard.yml` builds the project, runs the
tests, downloads the data, runs `mcb_report` and deploys `site/` to GitHub
Pages. It runs on every push to `main`, twice on weekdays after the markets
close, and on demand from the Actions tab.

One-time setup: in the repository open **Settings → Pages → Build and
deployment → Source** and choose **GitHub Actions**. After the next workflow
run the dashboard is at the address at the top of this file.

GitHub pauses scheduled workflows in repositories with no activity for 60
days; re-enable it from the Actions tab if that happens.

## Refreshing reference data

```bash
# QuantLib fixtures (only needed if conventions or the EUR snapshot change)
pip install QuantLib
python3 scripts/quantlib_reference.py

# Month-end extract of the Fed's yield-curve history
bash scripts/fetch_data.sh
./build/mcb_report --write-gsw-extract data/reference/gsw_monthend.csv
```

## Map to the project brief

| Phase | Where | Acceptance check |
|---|---|---|
| 1 Scope | this README | USD live, EUR reference, MOEX added; log-linear, log-cubic, monotone-convex interpolation |
| 2 Foundations | `core/` | `CalendarVsQuantLib.*`, `DayCountVsQuantLib.*`, `ScheduleVsQuantLib.*` |
| 3 Single-curve bootstrap | `curves/bootstrap.cpp` | `Bootstrap.*`: every instrument reprices to 1e-12 |
| 4 Multicurve | `data/eur_reference.cpp`, `curves/curve_engine.cpp` | `EurMulticurveVsQuantLib.*` |
| 5 Pricing and risk | `pricing/` | `SwapPricingVsQuantLib.*`, `BondPricingVsQuantLib.*`, `Risk.*` (buckets sum to the parallel DV01) |
| 6 Real-time layer | `curves/curve_engine.cpp`, `apps/mcb_bench.cpp` | `CurveEngine.*`, latency on the dashboard |
| 7 Validation report | `report/`, dashboard "Validation" | recomputed every run |
| 8 The paper | `stats/` | `EhTests.ReplicatesSection42OfThePaper`, forecasts on the dashboard |

## References

- R. K. Crump and N. Gospodinov, *Deconstructing the Yield Curve*, Federal Reserve Bank of New York Staff Reports 884 (2019, revised 2024).
- F. M. Ametrano and M. Bianchetti, *Everything You Always Wanted to Know About Multiple Interest Rate Curve Bootstrapping but Were Afraid to Ask* (2013).
- P. S. Hagan and G. West, *Interpolation Methods for Curve Construction*, Applied Mathematical Finance (2006).
- R. S. Gurkaynak, B. Sack and J. H. Wright, *The U.S. Treasury Yield Curve: 1961 to the Present*, Journal of Monetary Economics (2007).
- L. Kilian, *Small-Sample Confidence Intervals for Impulse Response Functions*, Review of Economics and Statistics (1998).
- [QuantLib](https://www.quantlib.org), the reference implementation used for validation.

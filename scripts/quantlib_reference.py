#!/usr/bin/env python3
"""Regenerates the QuantLib reference fixtures in tests/data/ql/.

The C++ engine is validated against QuantLib (https://www.quantlib.org), the
open-source reference implementation of Ametrano & Bianchetti's multicurve
algorithms.  QuantLib is NOT needed to build or run this project: this script
was run once and its output is committed, so the unit tests can compare the
engine with QuantLib offline.

Usage:
    pip install QuantLib            # tested with QuantLib 1.44
    python3 scripts/quantlib_reference.py [--ql-path DIR] [--out tests/data/ql]
"""
import argparse
import csv
import os
import random
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ql-path", help="directory containing an unpacked QuantLib wheel")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "tests", "data", "ql"))
    ap.add_argument("--eur-quotes", default=os.path.join(os.path.dirname(__file__), "..", "data", "reference",
                                                         "eur_20121211.csv"))
    args = ap.parse_args()
    if args.ql_path:
        sys.path.insert(0, args.ql_path)
    import QuantLib as ql

    os.makedirs(args.out, exist_ok=True)
    print("QuantLib", ql.__version__)
    Ref(ql, args.out, args.eur_quotes).run()


def iso(d):
    return "%04d-%02d-%02d" % (d.year(), d.month(), d.dayOfMonth())


class Ref:
    def __init__(self, ql, out, eur_quotes):
        self.ql, self.out, self.eur_quotes = ql, out, eur_quotes
        self.rng = random.Random(20261116)

    def write(self, name, header, rows):
        path = os.path.join(self.out, name)
        with open(path, "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(header)
            w.writerows(rows)
        print("  wrote %-28s %6d rows" % (name, len(rows)))

    # ------------------------------------------------------------------ dates
    def calendars(self):
        ql = self.ql
        return {
            "TARGET": ql.TARGET(),
            "US_SETTLEMENT": ql.UnitedStates(ql.UnitedStates.Settlement),
            "US_GOVBOND": ql.UnitedStates(ql.UnitedStates.GovernmentBond),
            "US_SOFR": ql.UnitedStates(ql.UnitedStates.SOFR),
        }

    def conventions(self):
        ql = self.ql
        return {"F": ql.Following, "MF": ql.ModifiedFollowing, "P": ql.Preceding,
                "MP": ql.ModifiedPreceding, "U": ql.Unadjusted}

    def units(self):
        ql = self.ql
        return {"D": ql.Days, "W": ql.Weeks, "M": ql.Months, "Y": ql.Years}

    def random_date(self, y0=1995, y1=2075):
        ql = self.ql
        y = self.rng.randint(y0, y1)
        m = self.rng.randint(1, 12)
        d = self.rng.randint(1, ql.Date.endOfMonth(ql.Date(1, m, y)).dayOfMonth())
        return ql.Date(d, m, y)

    def gen_holidays(self):
        ql = self.ql
        rows = []
        for name, cal in self.calendars().items():
            d, end = ql.Date(1, 1, 1990), ql.Date(31, 12, 2080)
            while d <= end:
                if cal.isHoliday(d) and not cal.isWeekend(d.weekday()):
                    rows.append([name, iso(d)])
                d = d + 1
        self.write("calendar_holidays.csv", ["calendar", "date"], rows)

    def gen_advance(self):
        rows = []
        cals, convs, units = self.calendars(), self.conventions(), self.units()
        for _ in range(2500):
            cn = self.rng.choice(list(cals))
            un = self.rng.choice(list(units))
            cv = self.rng.choice(list(convs))
            n = self.rng.randint(-24, 36) if un != "Y" else self.rng.randint(-5, 30)
            eom = self.rng.random() < 0.5
            d = self.random_date()
            if self.rng.random() < 0.3:  # stress month ends
                d = self.ql.Date.endOfMonth(d) - self.rng.randint(0, 3)
            r = cals[cn].advance(d, n, units[un], convs[cv], eom)
            rows.append([cn, iso(d), n, un, cv, int(eom), iso(r)])
        self.write("calendar_advance.csv", ["calendar", "date", "n", "unit", "convention", "eom", "result"], rows)

    def gen_daycount(self):
        ql = self.ql
        dcs = {
            "ACT/360": ql.Actual360(),
            "ACT/365F": ql.Actual365Fixed(),
            "30/360": ql.Thirty360(ql.Thirty360.BondBasis),
            "30E/360": ql.Thirty360(ql.Thirty360.European),
            "ACT/ACT ISDA": ql.ActualActual(ql.ActualActual.ISDA),
        }
        rows = []
        for _ in range(1200):
            d1 = self.random_date()
            if self.rng.random() < 0.4:
                d1 = ql.Date.endOfMonth(d1) - self.rng.randint(0, 2)
            d2 = d1 + self.rng.randint(0, 4000)
            if self.rng.random() < 0.4:
                d2 = ql.Date.endOfMonth(d2) - self.rng.randint(0, 2)
                if d2 < d1:
                    d2 = d1
            for name, dc in dcs.items():
                rows.append([name, iso(d1), iso(d2), dc.dayCount(d1, d2), repr(dc.yearFraction(d1, d2))])
        self.write("daycount.csv", ["daycount", "d1", "d2", "days", "year_fraction"], rows)

    def gen_schedules(self):
        ql = self.ql
        cals, convs = self.calendars(), self.conventions()
        rules = {"B": ql.DateGeneration.Backward, "F": ql.DateGeneration.Forward}
        tenors = ["1M", "3M", "6M", "1Y", "2W"]
        rows = []
        for _ in range(800):
            cn = self.rng.choice(list(cals))
            cal = cals[cn]
            eff = self.random_date(1999, 2040)
            if self.rng.random() < 0.35:
                eff = ql.Date.endOfMonth(eff) - self.rng.randint(0, 2)
            if self.rng.random() < 0.7:
                eff = cal.adjust(eff)
            months = self.rng.choice([1, 2, 3, 5, 6, 9, 12, 15, 18, 21, 24, 36, 60, 84, 120, 360])
            term = eff + ql.Period(months, ql.Months)
            if self.rng.random() < 0.2:
                term = term + self.rng.randint(-20, 20)
            tn = self.rng.choice(tenors)
            n, u = int(tn[:-1]), self.units()[tn[-1]]
            if tn == "2W" and months > 12:
                tn, n, u = "3M", 3, ql.Months
            cv = self.rng.choice(["MF", "F", "U", "P"])
            tcv = self.rng.choice(["MF", "F", "U"])
            rn = self.rng.choice(list(rules))
            eom = self.rng.random() < 0.5
            if term <= eff:
                continue
            try:
                s = ql.Schedule(eff, term, ql.Period(n, u), cal, convs[cv], convs[tcv], rules[rn], eom)
            except RuntimeError:
                continue
            rows.append([cn, iso(eff), iso(term), tn, cv, tcv, rn, int(eom), "|".join(iso(d) for d in s)])
        self.write("schedules.csv",
                   ["calendar", "effective", "termination", "tenor", "convention", "term_convention", "rule", "eom",
                    "dates"], rows)

    # ------------------------------------------------------------- EUR curves
    def read_eur(self):
        """Parses data/reference/eur_20121211.csv (same file the C++ engine loads)."""
        ql = self.ql
        asof, rows = None, []
        with open(self.eur_quotes) as f:
            for row in csv.DictReader(f):
                c = (row["curve"] or "").strip()
                if not c or c.startswith("#"):
                    continue
                if c == "META":
                    y, m, d = (int(x) for x in row["a"].split("-"))
                    asof = ql.Date(d, m, y)
                    continue
                rows.append(row)
        return asof, rows

    def qdate(self, text):
        y, m, d = (int(x) for x in text.split("-"))
        return self.ql.Date(d, m, y)

    def period(self, text):
        return self.ql.Period(int(text[:-1]), self.units()[text[-1]])

    def build_eur(self, interp, par_coupons, asof, rows, bump=None):
        """Returns {curve name: QuantLib curve} for one interpolation / coupon convention."""
        ql = self.ql
        ql.Settings.instance().evaluationDate = asof
        if par_coupons:
            ql.IborCoupon.createAtParCoupons()
        else:
            ql.IborCoupon.createIndexedCoupons()
        curve_cls = {"loglinear": ql.PiecewiseLogLinearDiscount,
                     "logcubic": ql.PiecewiseNaturalLogCubicDiscount,
                     "monotonelogcubic": ql.PiecewiseLogCubicDiscount}[interp]
        target = ql.TARGET()
        quote = {r["id"]: float(r["quote"]) for r in rows}
        if bump:
            for k, v in bump.items():
                quote[k] += v
        curves = {}
        discount = ql.RelinkableYieldTermStructureHandle()
        for name in ("EUR-OIS", "EUR-6M", "EUR-3M"):
            index = {"EUR-OIS": ql.Eonia(), "EUR-6M": ql.Euribor6M(), "EUR-3M": ql.Euribor3M()}[name]
            helpers = []
            for r in rows:
                if r["curve"] != name:
                    continue
                kind, q = r["kind"], quote[r["id"]]
                h = lambda v: ql.QuoteHandle(ql.SimpleQuote(v))
                if kind == "DEPOSIT":
                    per = self.period(r["a"])
                    overnight = per.units() == ql.Days
                    helpers.append(ql.DepositRateHelper(h(q / 100), per, int(r["b"]), target,
                                                        ql.Following if overnight else ql.ModifiedFollowing,
                                                        not overnight, ql.Actual360()))
                elif kind == "OIS":
                    helpers.append(ql.OISRateHelper(2, self.period(r["a"]), h(q / 100), index))
                elif kind == "OIS_DATED":
                    helpers.append(ql.OISRateHelper.forDates(self.qdate(r["a"]), self.qdate(r["b"]), h(q / 100), index))
                elif kind == "FRA":
                    helpers.append(ql.FraRateHelper(h(q / 100), int(r["a"]), index))
                elif kind == "FUTURE":
                    conv = float(r["b"] or 0.0) / 100
                    helpers.append(ql.FuturesRateHelper(h(q), self.qdate(r["a"]), index, h(conv)))
                elif kind in ("SWAP", "SWAP_BASIS"):
                    rate = q if kind == "SWAP" else quote[r["link"]] - q
                    helpers.append(ql.SwapRateHelper(h(rate / 100), self.period(r["a"]), target, ql.Annual,
                                                     ql.Unadjusted, ql.Thirty360(ql.Thirty360.BondBasis), index,
                                                     ql.QuoteHandle(), ql.Period(0, ql.Days), discount))
                else:
                    raise ValueError(kind)
            curve = curve_cls(asof, helpers, ql.Actual365Fixed())
            curve.enableExtrapolation()
            curve.discount(1.0)  # force the bootstrap now
            curves[name] = curve
            if name == "EUR-OIS":
                discount.linkTo(curve)
        return curves

    def eur_dates(self, asof, curve):
        ql = self.ql
        dates = set(curve.dates())
        d = asof
        while d < curve.maxDate():
            dates.add(d)
            d = d + (17 if d < asof + 800 else 131)
        return sorted(x for x in dates if x <= curve.maxDate())

    def gen_eur_curves(self):
        ql = self.ql
        asof, rows = self.read_eur()
        out, fwd, pillars = [], [], []
        for interp in ("loglinear", "logcubic", "monotonelogcubic"):
            for par in (True, False):
                if interp != "loglinear" and not par:
                    continue  # the coupon convention is independent of the interpolation
                variant = interp + ("|par" if par else "|indexed")
                curves = self.build_eur(interp, par, asof, rows)
                for name, c in curves.items():
                    for d in c.dates()[1:]:
                        pillars.append([variant, name, iso(d)])
                    for d in self.eur_dates(asof, c):
                        out.append([variant, name, iso(d), repr(c.discount(d))])
                # forward index fixings projected on each curve
                for name, idx_cls in (("EUR-6M", ql.Euribor6M), ("EUR-3M", ql.Euribor3M)):
                    index = idx_cls(ql.YieldTermStructureHandle(curves[name]))
                    d = ql.TARGET().adjust(asof + 1)
                    limit = curves[name].maxDate() - 400
                    while d < limit:
                        fwd.append([variant, name, iso(d), repr(index.fixing(d, True))])
                        d = ql.TARGET().adjust(d + (23 if d < asof + 1100 else 211))
        self.write("eur_curves.csv", ["variant", "curve", "date", "discount"], out)
        self.write("eur_pillars.csv", ["variant", "curve", "pillar"], pillars)
        self.write("eur_forwards.csv", ["variant", "index", "fixing_date", "forward"], fwd)

    # ---------------------------------------------------------- EUR pricing
    def eur_swap(self, curves, index_name, start, end, rate, payer, spread, nominal=1e6):
        ql = self.ql
        target = ql.TARGET()
        handle = ql.YieldTermStructureHandle(curves[index_name])
        index = (ql.Euribor6M if index_name == "EUR-6M" else ql.Euribor3M)(handle)
        fixed = ql.Schedule(start, end, ql.Period(1, ql.Years), target, ql.Unadjusted, ql.Unadjusted,
                            ql.DateGeneration.Backward, False)
        flt = ql.Schedule(start, end, index.tenor(), target, ql.ModifiedFollowing, ql.ModifiedFollowing,
                          ql.DateGeneration.Backward, False)
        swap = ql.VanillaSwap(ql.Swap.Payer if payer else ql.Swap.Receiver, nominal, fixed, rate,
                              ql.Thirty360(ql.Thirty360.BondBasis), flt, index, spread, index.dayCounter())
        swap.setPricingEngine(ql.DiscountingSwapEngine(ql.YieldTermStructureHandle(curves["EUR-OIS"])))
        return swap

    def gen_eur_pricing(self):
        ql = self.ql
        asof, rows = self.read_eur()
        target = ql.TARGET()
        spot = target.advance(asof, 2, ql.Days)
        swaps_out, ois_out, risk_out = [], [], []
        cases = [("EUR-6M", 0, 2, 0.005, True, 0.0), ("EUR-6M", 0, 5, 0.01, True, 0.0),
                 ("EUR-6M", 0, 10, 0.02, False, 0.0), ("EUR-6M", 0, 30, 0.025, True, 0.001),
                 ("EUR-6M", 12, 7, 0.015, True, 0.0), ("EUR-6M", 60, 10, 0.03, False, 0.0),
                 ("EUR-3M", 0, 3, 0.004, True, 0.0), ("EUR-3M", 0, 10, 0.015, False, 0.0005),
                 ("EUR-3M", 24, 5, 0.02, True, 0.0), ("EUR-3M", 0, 25, 0.022, True, 0.0)]
        for par in (True, False):
            variant = "loglinear|" + ("par" if par else "indexed")
            curves = self.build_eur("loglinear", par, asof, rows)
            for k, (idx, fwd_months, years, rate, payer, spread) in enumerate(cases):
                start = spot if fwd_months == 0 else target.adjust(spot + ql.Period(fwd_months, ql.Months))
                end = start + ql.Period(years, ql.Years)
                sw = self.eur_swap(curves, idx, start, end, rate, payer, spread)
                swaps_out.append([variant, k, idx, iso(start), iso(end), repr(rate), int(payer), repr(spread),
                                  repr(sw.NPV()), repr(sw.fairRate()), repr(sw.fixedLegNPV()),
                                  repr(sw.floatingLegNPV())])
            if par:
                eonia = ql.Eonia(ql.YieldTermStructureHandle(curves["EUR-OIS"]))
                engine = ql.DiscountingSwapEngine(ql.YieldTermStructureHandle(curves["EUR-OIS"]))
                for k, (tenor, rate, payer) in enumerate([("1M", 0.001, True), ("9M", 0.0005, False),
                                                          ("18M", 0.002, True), ("5Y", 0.006, True),
                                                          ("12Y", 0.02, False), ("30Y", 0.015, True)]):
                    start = spot
                    end = start + self.period(tenor)
                    sch = ql.Schedule(start, end, ql.Period(1, ql.Years), target, ql.ModifiedFollowing,
                                      ql.ModifiedFollowing, ql.DateGeneration.Backward, False)
                    ois = ql.OvernightIndexedSwap(ql.Swap.Payer if payer else ql.Swap.Receiver, 1e6, sch, rate,
                                                  ql.Actual360(), eonia)
                    ois.setPricingEngine(engine)
                    ois_out.append([k, tenor, repr(rate), int(payer), repr(ois.NPV()), repr(ois.fairRate())])
                # bump-and-rebuild deltas for a 10Y payer swap vs Euribor 6M at 2%
                base_swap = self.eur_swap(curves, "EUR-6M", spot, spot + ql.Period(10, ql.Years), 0.02, True, 0.0)
                base = base_swap.NPV()
                for qid in ("EUR6M_SWAP_10Y", "EUR6M_SWAP_5Y", "EONIA_10Y", "EONIA_5Y", "EUR6M_FRA_6x12",
                            "EUR6M_SWAP_12Y"):
                    bumped = self.build_eur("loglinear", True, asof, rows, bump={qid: 0.01})  # +1bp
                    sw = self.eur_swap(bumped, "EUR-6M", spot, spot + ql.Period(10, ql.Years), 0.02, True, 0.0)
                    risk_out.append([qid, repr(sw.NPV() - base)])
        self.write("eur_swaps.csv", ["variant", "case", "index", "start", "end", "fixed_rate", "payer", "spread",
                                     "npv", "fair_rate", "fixed_leg_npv", "float_leg_npv"], swaps_out)
        self.write("eur_ois_swaps.csv", ["case", "tenor", "fixed_rate", "payer", "npv", "fair_rate"], ois_out)
        self.write("eur_deltas.csv", ["quote_id", "delta_npv_1bp"], risk_out)

    # ------------------------------------------------------------------ bonds
    def gen_bonds(self):
        ql = self.ql
        asof, rows = self.read_eur()
        curves = self.build_eur("loglinear", True, asof, rows)
        curve = curves["EUR-OIS"]
        handle = ql.YieldTermStructureHandle(curve)
        target = ql.TARGET()
        settle = ql.Date(12, 12, 2012)
        dc = ql.ActualActual(ql.ActualActual.ISMA)
        out = []
        for k, (mat, cpn, clean) in enumerate([(ql.Date(30, 6, 2014), 0.0075, 100.40),
                                               (ql.Date(15, 8, 2022), 0.01625, 99.10),
                                               (ql.Date(15, 11, 2042), 0.0275, 97.25),
                                               (ql.Date(28, 2, 2013), 0.005, 100.05),
                                               (ql.Date(15, 5, 2030), 0.0625, 148.50),
                                               (ql.Date(31, 12, 2017), 0.0075, 100.90)]):
            eom = ql.Date.isEndOfMonth(mat)
            issue = mat - ql.Period(60 if mat > ql.Date(1, 1, 2020) else 10, ql.Years)
            while issue > settle:
                issue = issue - ql.Period(5, ql.Years)
            sch = ql.Schedule(issue, mat, ql.Period(ql.Semiannual), target, ql.Unadjusted, ql.Unadjusted,
                              ql.DateGeneration.Backward, eom)
            bond = ql.FixedRateBond(1, 100.0, sch, [cpn], dc, ql.Following, 100.0, issue)
            price = ql.BondPrice(clean, ql.BondPrice.Clean)
            accrued = ql.BondFunctions.accruedAmount(bond, settle)
            y = ql.BondFunctions.bondYield(bond, price, dc, ql.Compounded, ql.Semiannual, settle, 1e-14, 500, cpn)
            mod = ql.BondFunctions.duration(bond, y, dc, ql.Compounded, ql.Semiannual, ql.Duration.Modified, settle)
            mac = ql.BondFunctions.duration(bond, y, dc, ql.Compounded, ql.Semiannual, ql.Duration.Macaulay, settle)
            cvx = ql.BondFunctions.convexity(bond, y, dc, ql.Compounded, ql.Semiannual, settle)
            from_curve = ql.BondFunctions.cleanPrice(bond, curve, settle)
            zs = ql.BondFunctions.zSpread(bond, price, curve, ql.Actual365Fixed(), ql.Continuous, ql.NoFrequency,
                                          settle, 1e-14, 500, 0.0)
            out.append([k, iso(mat), repr(cpn), iso(settle), repr(clean), repr(accrued), repr(y), repr(mod),
                        repr(mac), repr(cvx), repr(from_curve), repr(zs)])
        self.write("bonds.csv", ["case", "maturity", "coupon", "settle", "clean_price", "accrued", "yield",
                                 "modified_duration", "macaulay_duration", "convexity", "clean_from_curve",
                                 "z_spread"], out)

    def run(self):
        self.gen_holidays()
        self.gen_advance()
        self.gen_daycount()
        self.gen_schedules()
        self.gen_eur_curves()
        self.gen_eur_pricing()
        self.gen_bonds()


if __name__ == "__main__":
    main()

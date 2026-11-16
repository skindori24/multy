#include "mcb/curves/conventions.hpp"

#include "mcb/core/error.hpp"

namespace mcb {

using BDC = BusinessDayConvention;

IborIndex IborIndex::euribor(Period tenor) {
  IborIndex i;
  i.name = "Euribor" + tenor.str();
  i.tenor = tenor;
  i.fixingDays = 2;
  i.calendar = Calendar(Calendar::Kind::Target);
  i.convention = BDC::ModifiedFollowing;
  // Euribor follows the end-of-month rule for monthly tenors.
  i.endOfMonth = tenor.unit == TimeUnit::Months || tenor.unit == TimeUnit::Years;
  i.dayCount = DayCount::Act360;
  return i;
}

Date spotDate(Date asOf, const Calendar& calendar, int days) {
  return calendar.advance(calendar.adjust(asOf), days, TimeUnit::Days);
}

OisConventions OisConventions::eonia() {
  OisConventions c;
  c.calendar = Calendar(Calendar::Kind::Target);
  return c;
}

OisConventions OisConventions::sofr() {
  OisConventions c;
  c.calendar = Calendar(Calendar::Kind::UsSofr);
  c.paymentLag = 2;
  c.endOfMonthRule = false;
  return c;
}

SwapConventions SwapConventions::euribor(Period indexTenor) {
  SwapConventions c;
  c.calendar = Calendar(Calendar::Kind::Target);
  c.index = IborIndex::euribor(indexTenor);
  return c;
}

// ------------------------------------------------------------ instruments ---

OisSwap makeOisSwap(Date start, Date end, bool endOfMonth, const OisConventions& c, double fixedRate, bool payFixed,
                    double notional) {
  ScheduleSpec spec;
  spec.effective = start;
  spec.termination = end;
  spec.tenor = c.paymentTenor;
  spec.calendar = c.calendar;
  spec.convention = BDC::ModifiedFollowing;
  spec.terminationConvention = BDC::ModifiedFollowing;
  spec.rule = DateGeneration::Backward;
  spec.endOfMonth = endOfMonth;
  const Schedule s = Schedule::make(spec);
  OisSwap swap;
  swap.notional = notional;
  swap.fixedRate = fixedRate;
  swap.payFixed = payFixed;
  for (std::size_t i = 0; i + 1 < s.size(); ++i) {
    OisPeriod p;
    p.start = s[i];
    p.end = s[i + 1];
    p.pay = c.calendar.advance(p.end, c.paymentLag, TimeUnit::Days, c.paymentConvention);
    p.tau = yearFraction(c.dayCount, p.start, p.end);
    swap.periods.push_back(p);
  }
  return swap;
}

OisSwap makeOisSwap(Date asOf, Period tenor, const OisConventions& c, double fixedRate, bool payFixed,
                    double notional) {
  const Date start = spotDate(asOf, c.calendar, c.settlementDays);
  // Swaps starting on the last business day of a month roll on month ends.
  const bool eom = c.endOfMonthRule && c.calendar.isEndOfMonth(start);
  const Date end = eom ? c.calendar.advance(start, tenor, BDC::ModifiedFollowing, true) : start.add(tenor);
  return makeOisSwap(start, end, eom, c, fixedRate, payFixed, notional);
}

VanillaSwap makeVanillaSwap(Date start, Date end, const SwapConventions& c, double fixedRate, bool payFixed,
                            double notional) {
  const IborIndex& idx = c.index;
  ScheduleSpec fixedSpec;
  fixedSpec.effective = start;
  fixedSpec.termination = end;
  fixedSpec.tenor = c.fixedTenor;
  fixedSpec.calendar = c.calendar;
  fixedSpec.convention = c.fixedConvention;
  fixedSpec.terminationConvention = c.fixedConvention;
  const Schedule fixedSchedule = Schedule::make(fixedSpec);

  ScheduleSpec floatSpec = fixedSpec;
  floatSpec.tenor = idx.tenor;
  floatSpec.convention = idx.convention;
  floatSpec.terminationConvention = idx.convention;
  const Schedule floatSchedule = Schedule::make(floatSpec);

  VanillaSwap swap;
  swap.notional = notional;
  swap.fixedRate = fixedRate;
  swap.payFixed = payFixed;
  // Both legs pay on the accrual end date rolled with the floating-leg convention.
  for (std::size_t i = 0; i + 1 < fixedSchedule.size(); ++i) {
    FixedCoupon f;
    f.start = fixedSchedule[i];
    f.end = fixedSchedule[i + 1];
    f.pay = c.calendar.adjust(f.end, idx.convention);
    f.tau = yearFraction(c.fixedDayCount, f.start, f.end);
    swap.fixedLeg.push_back(f);
  }
  for (std::size_t i = 0; i + 1 < floatSchedule.size(); ++i) {
    IborCoupon f;
    f.start = floatSchedule[i];
    f.end = floatSchedule[i + 1];
    f.pay = c.calendar.adjust(f.end, idx.convention);
    f.fixing = idx.calendar.advance(f.start, -idx.fixingDays, TimeUnit::Days, BDC::Preceding);
    f.valueDate = idx.valueDate(f.fixing);
    if (c.parCoupons) {
      // The index period is assumed to end where the next coupon's would start.
      const Date nextFixing = idx.calendar.advance(f.end, -idx.fixingDays, TimeUnit::Days);
      f.indexEnd = idx.calendar.advance(nextFixing, idx.fixingDays, TimeUnit::Days);
    } else {
      f.indexEnd = idx.maturityDate(f.valueDate);
    }
    f.tauIndex = yearFraction(idx.dayCount, f.valueDate, f.indexEnd);
    f.tauAccrual = yearFraction(idx.dayCount, f.start, f.end);
    swap.floatLeg.push_back(f);
  }
  return swap;
}

VanillaSwap makeVanillaSwap(Date asOf, Period tenor, const SwapConventions& c, double fixedRate, bool payFixed,
                            double notional) {
  const Date start = spotDate(asOf, c.calendar, c.settlementDays);
  return makeVanillaSwap(start, start.add(tenor), c, fixedRate, payFixed, notional);
}

Fra makeFra(Date asOf, int monthsToStart, const IborIndex& index, double strike, bool payFixed, double notional) {
  const Date spot = spotDate(asOf, index.calendar, index.fixingDays);
  Fra fra;
  fra.notional = notional;
  fra.strike = strike;
  fra.payFixed = payFixed;
  fra.valueDate =
      index.calendar.advance(spot, Period{monthsToStart, TimeUnit::Months}, index.convention, index.endOfMonth);
  fra.indexEnd = index.maturityDate(fra.valueDate);
  fra.tau = yearFraction(index.dayCount, fra.valueDate, fra.indexEnd);
  return fra;
}

// ---------------------------------------------------------------- helpers ---

OisHelper::Period toHelperPeriod(const OisPeriod& p) { return OisHelper::Period{p.start, p.end, p.pay, p.tau}; }

std::shared_ptr<RateHelper> depositHelper(Date asOf, Period tenor, int fixingDays, const Calendar& calendar,
                                          BDC convention, bool endOfMonth, DayCount dc) {
  const Date start = spotDate(asOf, calendar, fixingDays);
  const Date end = calendar.advance(start, tenor, convention, endOfMonth);
  auto h = std::make_shared<ForwardRateHelper>("deposit", start, end, dc);
  h->label = "Deposit " + tenor.str();
  return h;
}

std::shared_ptr<RateHelper> fraHelper(Date asOf, int monthsToStart, const IborIndex& index) {
  const Fra fra = makeFra(asOf, monthsToStart, index);
  auto h = std::make_shared<ForwardRateHelper>("fra", fra.valueDate, fra.indexEnd, index.dayCount);
  h->label = "FRA " + std::to_string(monthsToStart) + "x" +
             std::to_string(monthsToStart + (index.tenor.unit == TimeUnit::Years ? 12 : 1) * index.tenor.length);
  return h;
}

std::shared_ptr<RateHelper> futuresHelper(Date immStart, const IborIndex& index, double convexityAdjustment) {
  // The futures period runs from the IMM date for one index tenor (no end-of-month rule).
  const Date end = index.calendar.advance(immStart, index.tenor, index.convention);
  auto h = std::make_shared<FuturesHelper>(immStart, end, index.dayCount, convexityAdjustment);
  h->label = "Future " + immStart.iso();
  return h;
}

std::shared_ptr<RateHelper> oisHelper(Date asOf, Period tenor, const OisConventions& c) {
  const OisSwap swap = makeOisSwap(asOf, tenor, c);
  std::vector<OisHelper::Period> periods;
  for (const auto& p : swap.periods) periods.push_back(toHelperPeriod(p));
  auto h = std::make_shared<OisHelper>(std::move(periods));
  h->label = "OIS " + tenor.str();
  return h;
}

std::shared_ptr<RateHelper> datedOisHelper(Date start, Date end, const OisConventions& c) {
  const OisSwap swap = makeOisSwap(start, end, false, c);
  std::vector<OisHelper::Period> periods;
  for (const auto& p : swap.periods) periods.push_back(toHelperPeriod(p));
  auto h = std::make_shared<OisHelper>(std::move(periods));
  h->label = "OIS " + start.iso() + "/" + end.iso();
  return h;
}

std::shared_ptr<RateHelper> swapHelper(Date asOf, Period tenor, const SwapConventions& c) {
  const VanillaSwap swap = makeVanillaSwap(asOf, tenor, c);
  std::vector<SwapHelper::FixedCoupon> fixed;
  std::vector<SwapHelper::FloatCoupon> floating;
  for (const auto& f : swap.fixedLeg) fixed.push_back({f.pay, f.tau});
  for (const auto& f : swap.floatLeg) floating.push_back({f.valueDate, f.indexEnd, f.pay, f.tauIndex, f.tauAccrual});
  auto h = std::make_shared<SwapHelper>(std::move(fixed), std::move(floating), swap.floatLeg.front().start,
                                        swap.floatLeg.back().end);
  h->label = "Swap " + tenor.str() + " vs " + c.index.name;
  return h;
}

std::shared_ptr<RateHelper> parBondHelper(Date settle, Period tenor, int frequency) {
  MCB_REQUIRE(frequency >= 1 && 12 % frequency == 0, "par bond: unsupported coupon frequency " << frequency);
  const Date maturity = settle.add(tenor);
  ScheduleSpec spec;
  spec.effective = settle;
  spec.termination = maturity;
  spec.tenor = Period{12 / frequency, TimeUnit::Months};
  spec.calendar = Calendar(Calendar::Kind::Null);
  spec.convention = BDC::Unadjusted;
  spec.terminationConvention = BDC::Unadjusted;
  const Schedule s = Schedule::make(spec);
  std::vector<ParBondHelper::Coupon> coupons;
  for (std::size_t i = 1; i < s.size(); ++i) coupons.push_back({s[i], 1.0 / frequency});
  auto h = std::make_shared<ParBondHelper>(settle, std::move(coupons));
  h->label = "Par bond " + tenor.str();
  return h;
}

std::shared_ptr<RateHelper> billHelper(Date settle, Period tenor) {
  auto h = std::make_shared<ForwardRateHelper>("bill", settle, settle.add(tenor), DayCount::Act365Fixed);
  h->label = "Bill " + tenor.str();
  return h;
}

}  // namespace mcb

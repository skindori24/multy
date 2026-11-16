#include "mcb/report/report.hpp"

#include <ostream>

#include "sections.hpp"

namespace mcb {

Json buildReport(const ReportOptions& options, std::ostream& log) {
  report::Context ctx;
  ctx.opt = options;
  ctx.log = &log;

  Json doc = Json::object();
  // Order matters: the statistics section starts its forecasts from the
  // Treasury curve bootstrapped in the USD section.
  const Json usd = report::usdSection(ctx);
  const Json stats = report::statsSection(ctx);
  const Json eur = report::eurSection(ctx);
  const Json moex = report::moexSection(ctx);

  int passed = 0, failed = 0;
  for (const auto& row : ctx.validation.items()) {
    if (row["ok"].isNull()) continue;
    (row["ok"].boolean() ? passed : failed) += 1;
  }

  Json meta = Json::object()
                  .set("generatedAt", options.generatedAt)
                  .set("engine", "multicurve-bootstrap 1.0.0")
                  .set("sources", ctx.sources)
                  .set("checksPassed", passed)
                  .set("checksFailed", failed)
                  .set("disclaimer",
                       "Educational output of a C++ course project. Signals are mechanical results of the stated "
                       "rules applied to public end-of-day data; they ignore transaction costs, liquidity, taxes and "
                       "your circumstances, and are not investment advice.");
  doc.set("meta", meta);
  doc.set("signals", ctx.signals);
  doc.set("usd", usd);
  doc.set("stats", stats);
  doc.set("eur", eur);
  doc.set("moex", moex);
  doc.set("validation", ctx.validation);
  log << "Validation: " << passed << " checks passed, " << failed << " failed\n";
  return doc;
}

}  // namespace mcb

#!/usr/bin/env python3
"""streaming-ratio independent rational reference; run on the verification host.

JSON stdin: {"points":[[time_ms,equity,open_profit],...]}.
Equity/open-profit tokens may be Python float.hex strings (lossless).
--equities/--first/--last supplies the small generated C++ rows without files.
--tsv emits scalar key/value lines for the C++ harness; default is JSON.

Decode the EMITTED binary64 returns, not ideal unrounded equity returns.
All moments and sqrt enclosures use fractions.Fraction; no numpy, Decimal,
long double, production library, or assumed platform extended precision.
The legacy emulation below is a frozen copy of feae3369's operation order.
"""
import argparse
from fractions import Fraction
import json
import math
import struct
import subprocess
import sys

YEAR = 31557600000
TAU = Fraction(1, 10**12)


def binary64(token):
    if isinstance(token, str):
        return float.fromhex(token) if "0x" in token.lower() else float(token)
    return float(token)


def bits(value):
    return struct.pack(">d", value).hex()


def rational(value):
    return Fraction.from_float(value)


def wire_fraction(value):
    return str(value.numerator) + "/" + str(value.denominator)


def sqrt_interval(value, precision=256):
    """Enclose sqrt(value) by adjacent dyadics, with no float conversion."""
    if value < 0:
        raise ValueError("negative squared deviation")
    if not value:
        return Fraction(0), Fraction(0)
    scale = 1 << precision
    floor = math.isqrt((value.numerator << (2 * precision)) // value.denominator)
    lower = Fraction(floor, scale)
    if lower * lower == value:
        return lower, lower
    return lower, Fraction(floor + 1, scale)


def divide_by_sqrt(numerator, squared, precision=256):
    if squared == 0:
        return None
    # Absolute 2^-precision spacing also covers tiny/subnormal deviations:
    # refine until lower>0 and both ratio endpoints round to the same double.
    while precision <= 16384:
        lower, upper = sqrt_interval(squared, precision)
        if lower:
            ends = sorted((numerator / upper, numerator / lower))
            rounded = []
            for endpoint in ends:
                try:
                    rounded.append(float(endpoint))
                except OverflowError:
                    rounded.append(math.inf if endpoint >= 0 else -math.inf)
            if bits(rounded[0]) == bits(rounded[1]):
                return {"lower": wire_fraction(ends[0]),
                        "upper": wire_fraction(ends[1]),
                        "rounded": rounded[0], "precision": precision}
        precision *= 2
    raise ArithmeticError("ratio rounding still ambiguous at 16384 bits")


def density(count, first, last):
    # The integer magnitude difference precedes conversion, including the
    # int64 endpoint extremes. Do not cast each endpoint before subtraction.
    span_ms = float(last - first)
    span_years = span_ms / (365.25 * 86400.0 * 1000.0)
    if count < 3 or span_years <= 0:
        return span_ms, None, None, None
    per_year = float(count - 1) / span_years
    return span_ms, per_year, .02 / per_year, math.sqrt(per_year)


def emitted_returns(points):
    return [points[i][1] / points[i - 1][1] - 1.0
            for i in range(1, len(points)) if points[i - 1][1] > 0.0]


def legacy_generated(generate, rf, ann, enabled, trace_enabled=True):
    result = {"sharpe": math.nan, "sortino": math.nan, "trace": [],
              "failure": None, "mean": math.nan, "variance_sum": 0.,
              "downside_sum": 0.}
    trace = result["trace"]

    def check(value, index, operation, field):
        failed = not math.isfinite(value)
        event = ["bar", index, operation,
                 "report_result_invalid" if failed else "", field,
                 "nonfinite" if failed else ""]
        if trace_enabled:
            trace.append(event)
        if failed:
            result["failure"] = event
            raise ArithmeticError("frozen legacy nonfinite")
        return value

    sf = "metrics.equity.sharpe_bar"
    df = "metrics.equity.sortino_bar"
    total = 0.
    deferred = None
    count = 0
    for i, value in enumerate(generate()):
        for op, x in (("return.first_pass", value), ("sum", total + value)):
            if trace_enabled:
                trace.append(["bar.first_pass", i, op, "", sf, ""])
            if not math.isfinite(x) and deferred is None:
                deferred = i, op
        total += value
        count += 1
    if not enabled or count < 2:
        return result
    try:
        check(rf, -1, "rf", sf)
        check(ann, -1, "annualizer", sf)
        if deferred is not None:
            check(math.nan, deferred[0], "promote." + deferred[1], sf)
        check(total, -1, "sum.final", sf)
        mean = check(total / float(count), -1, "mean", sf)
        result["mean"] = mean
        var = down = 0.
        for i, value in enumerate(generate()):
            check(value, i, "return.second_pass", sf)
            delta = check(value - mean, i, "delta", sf)
            var = check(var + delta * delta, i, "variance.add_square", sf)
            d = min(0., check(value - rf, i, "downside.subtract", df))
            down = check(down + d * d, i, "downside.add_square", df)
        result["variance_sum"], result["downside_sum"] = var, down
        sd = check(math.sqrt(var / float(count - 1)), -1, "sample.sd", sf)
        dd = check(math.sqrt(down / float(count)), -1, "downside.sd", df)
        if sd > 0:
            result["sharpe"] = check((mean - rf) / sd * ann, -1, "sharpe", sf)
        if dd > 0:
            result["sortino"] = check((mean - rf) / dd * ann, -1, "sortino", df)
    except ArithmeticError:
        # This records an EXPECTED legacy failure, never a passing candidate.
        pass
    return result


def reference_source(count, first, last, generate_points):
    span, per_year, rf, ann = density(count, first, last)

    def generate_returns():
        previous = None
        for point in generate_points():
            if previous is not None and previous > 0:
                yield point[1] / previous - 1.
            previous = point[1]

    old = legacy_generated(generate_returns, rf, ann, per_year is not None, count <= 64)
    m = 0
    total = squared = absolute = exact_downside = Fraction(0)
    finite = True
    rounded_rf = rational(rf) if rf is not None else Fraction(0)
    for value in generate_returns():
        m += 1
        if not math.isfinite(value):
            finite = False
            continue
        r = rational(value)
        total += r
        squared += r*r
        absolute += abs(r)
        exact_downside += min(Fraction(0), r - rounded_rf)**2
    result = {"count": count, "m": m, "first": first, "last": last,
              "span_ms": span, "rf": rf, "annualizer": ann, "legacy": old,
              "legacy_sharpe_bits": bits(old["sharpe"]),
              "legacy_sortino_bits": bits(old["sortino"]),
              "constant_series": False}
    if not m or not finite:
        result["exact_domain"] = "nonfinite_or_empty_returns"
        return result
    mean = total / m
    # Raw moments here are EXACT RATIONAL operations. No cancellation or
    # rounding occurs; this is not the binary64 raw-moment mutation.
    centered = squared - total*total/m
    result["exact_mean"] = wire_fraction(mean)
    result["exact_centered_sum"] = wire_fraction(centered)
    result["constant_series"] = centered == 0
    if m < 2 or rf is None:
        result["exact_domain"] = "insufficient_count_or_span"
        return result
    variance = centered / (m - 1)
    result["exact_sample_variance"] = wire_fraction(variance)
    if centered:
        result["kappa"] = divide_by_sqrt(abs(mean), variance)
    else:
        result["kappa"] = None  # A distinct constant-series domain.
    rounded_ann = rational(ann)
    unit = Fraction(1, 2**53)
    gamma = m*unit/(1-m*unit) if m*unit < 1 else None
    mean_budget = (1 + TAU/2)**2 - 1
    bound = Fraction(m, m-1) * (gamma * absolute/m)**2 / variance if gamma and variance else None
    result["mean_screen_pass"] = int(bound is not None and bound <= mean_budget)
    result["exact_downside_sum"] = wire_fraction(exact_downside)
    exact_numerator = (mean - rounded_rf) * rounded_ann
    result["exact_mean_sharpe"] = divide_by_sqrt(exact_numerator, variance)
    result["exact_mean_sortino"] = divide_by_sqrt(
        exact_numerator, exact_downside / m)
    if math.isfinite(old["mean"]):
        # Isolate the variance change with the legacy rounded mean AND the
        # legacy rounded numerator subtraction. Final ann operand is rounded.
        numerator = rational(old["mean"] - rf) * rounded_ann
        result["legacy_numerator_exact_variance_sharpe"] = divide_by_sqrt(
            numerator, variance)
        result["legacy_numerator_exact_downside_sortino"] = divide_by_sqrt(
            numerator, exact_downside / m)
    return result


def reference(points):
    count = len(points)
    first, last = (points[0][0], points[-1][0]) if points else (0, 0)
    return reference_source(count, first, last, lambda: iter(points))


def scalar_rows(result):
    for key in ("count", "m", "first", "last", "span_ms", "rf", "annualizer",
                "legacy_sharpe_bits", "legacy_sortino_bits", "exact_mean",
                "exact_centered_sum", "exact_sample_variance", "exact_downside_sum",
                "mean_screen_pass"):
        if key in result:
            yield key, result[key]
    for key in ("sharpe", "sortino", "mean", "variance_sum", "downside_sum"):
        yield "legacy_" + key, repr(result["legacy"][key])
    for key in ("kappa", "exact_mean_sharpe", "exact_mean_sortino",
                "legacy_numerator_exact_variance_sharpe",
                "legacy_numerator_exact_downside_sortino"):
        value = result.get(key)
        if value is not None:
            for subkey in ("lower", "upper", "rounded", "precision"):
                yield key + "_" + subkey, value[subkey]
    yield "constant_series", int(result["constant_series"])
    yield "legacy_failure", json.dumps(result["legacy"]["failure"], separators=(",", ":"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--equities")
    parser.add_argument("--first", type=int, default=0)
    parser.add_argument("--last", type=int, default=YEAR)
    parser.add_argument("--tsv", action="store_true")
    parser.add_argument("--points-file")
    # CR-150 lane files permit script rows. Forward the exact child status;
    # no expected-failure translation, timeout pass, skip or local lab call.
    parser.add_argument("--run-row", nargs=2, metavar=("EXECUTABLE", "ROW"))
    args = parser.parse_args()
    if args.run_row:
        return subprocess.call([args.run_row[0], "--row", args.run_row[1]])
    if args.points_file:
        with open(args.points_file, encoding="ascii") as stream:
            count, first, last = map(int, stream.readline().split())

        def generate():
            with open(args.points_file, encoding="ascii") as stream:
                next(stream)
                for line in stream:
                    t, e, o = line.split()
                    yield [int(t), binary64(e), binary64(o)]

        result = reference_source(count, first, last, generate)
    elif args.equities is not None:
        equities = [binary64(x) for x in args.equities.split(",")]
        divisor = max(1, len(equities) - 1)
        points = [[args.first + (args.last - args.first) * i // divisor, eq, 0.]
                  for i, eq in enumerate(equities)]
        result = reference(points)
    else:
        source = json.load(sys.stdin)
        points = [[int(p[0]), binary64(p[1]), binary64(p[2])] for p in source["points"]]
        result = reference(points)
    if args.tsv:
        for key, value in scalar_rows(result):
            print(key + "\t" + str(value))
    else:
        # Reference artifacts explicitly spell NaN/Infinity; these are not
        # application report JSON or a newly published ratio domain.
        json.dump(result, sys.stdout, indent=2, allow_nan=True)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

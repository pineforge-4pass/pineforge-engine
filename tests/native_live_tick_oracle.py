"""Independent print and continuous-OHLC fill oracles for first divergences.

Only immutable accepted order terms are read from the observer. No engine fill
price, activation decision, or execution sequence is an oracle input. Unsupported
terms and any disagreement are findings, not comparator exceptions.
"""

from decimal import Decimal
import math
import re


def directional_price(value, is_buy, tick=0.01):
    scaled = float(value) / tick
    return (math.ceil(scaled - 1e-9) if is_buy else math.floor(scaled + 1e-9)) * tick


def print_price(value, is_buy, slippage=0, tick=0.01, multiply=False):
    value = float(value)
    index = math.floor(value / tick + 0.5)
    rounded = index * tick if multiply or index * tick == value else index / round(1 / tick)
    return directional_price(rounded + (1 if is_buy else -1) * slippage * tick, is_buy, tick)


def script_bars(minute_bars, duration=900000):
    combined = {}
    for row in minute_bars:
        timestamp = int(row["timestamp"])
        bucket = timestamp - timestamp % duration
        prices = {field: Decimal(str(row[field])) for field in ("open", "high", "low", "close")}
        if bucket not in combined:
            combined[bucket] = {"timestamp": bucket, **prices}
        else:
            bar = combined[bucket]
            bar["high"] = max(bar["high"], prices["high"])
            bar["low"] = min(bar["low"], prices["low"])
            bar["close"] = prices["close"]
    return list(combined.values())


def modeled_points(bars, not_before, duration=900000):
    for bar in bars:
        timestamp = bar["timestamp"]
        if timestamp < not_before:
            continue
        high_first = abs(bar["high"] - bar["open"]) < abs(bar["open"] - bar["low"])
        fields = ("open", "high", "low", "close") if high_first else ("open", "low", "high", "close")
        previous = None
        for field in fields:
            yield {"ts": timestamp + (duration if field == "close" else 0),
                "price": bar[field], "phase": {"open": 1, "high": 2, "low": 3, "close": 4}[field],
                "previous": previous, "seq": 0}
            previous = bar[field]


def first_fill(request, points, is_buy, modeled=False, slippage=0, tick=0.01):
    terms = {key: Decimal(str(value)) for key, value in request["levels"].items() if value is not None}
    order_type = request["type"]
    if order_type not in ("limit", "stop", "stop_limit", "trail", "market"):
        raise ValueError("unsupported independent oracle trigger " + order_type)
    if order_type == "trail" and ("arm" not in terms or terms.get("offset", Decimal(0)) <= 0):
        raise ValueError("oracle requires an explicit positive-offset trail")
    active = False
    best = None
    activation = None
    considered = 0

    def reaches(price, level, upward):
        return price >= level if upward else price <= level

    for point in points:
        if point["ts"] < request["not_before"]:
            continue
        considered += 1
        price = Decimal(str(point["price"]))
        previous = point.get("previous") if modeled else None
        gap = not modeled or previous is None
        raw = price
        fill = False
        if order_type == "market":
            fill = True
        elif order_type == "limit":
            fill = reaches(price, terms["limit"], not is_buy)
            if fill and not gap:
                raw = terms["limit"] if not reaches(previous, terms["limit"], not is_buy) else previous
        elif order_type in ("stop", "stop_limit"):
            if not active and reaches(price, terms["stop"], is_buy):
                active = True
                activation = {"timestamp": point["ts"], "sequence": point["seq"], "price": str(price)}
                if not gap and not reaches(previous, terms["stop"], is_buy):
                    raw = terms["stop"]
            if active:
                fill = order_type == "stop" or reaches(raw, terms["limit"], not is_buy)
                if not fill and modeled and previous is not None and reaches(price, terms["limit"], not is_buy):
                    raw = terms["limit"]
                    fill = True
        else:
            if not active and reaches(price, terms["arm"], not is_buy):
                active = True
                best = terms.get("best_seed", terms["arm"])
                activation = {"timestamp": point["ts"], "sequence": point["seq"], "price": str(price)}
            if active:
                best = min(best, price) if is_buy else max(best, price)
                level = best + (terms["offset"] if is_buy else -terms["offset"])
                fill = reaches(price, level, is_buy)
                if fill and not gap:
                    raw = level
        if not fill:
            continue
        if order_type == "limit":
            resolved = print_price(raw, is_buy, tick=tick, multiply=True) if modeled and gap else directional_price(terms["limit"], not is_buy, tick)
        elif order_type == "stop_limit":
            resolved = directional_price(raw, not is_buy, tick)
        elif modeled and not gap:
            resolved = directional_price(float(raw) + (1 if is_buy else -1) * slippage * tick, is_buy, tick)
        else:
            resolved = print_price(raw, is_buy, slippage, tick)
        return {"timestamp": point["ts"], "sequence": point["seq"], "raw_price": float(raw),
            "resolved_price": resolved, "path_phase": point.get("phase", 0),
            "activation": activation, "running_best": None if best is None else str(best),
            "considered_points": considered, "type": order_type}
    return None


def execution_receipt(receipts, action):
    order = action["order"]
    matches = [row for row in receipts if row["kind"] == "executed" and row["id"] == order["id"]
        and row["timestamp"] == action["timestamp"] and row["resolved_price"] == float(order["price"])]
    if len(matches) != 1:
        raise ValueError("first-divergence execution receipt is not unique")
    execution = matches[0]
    requests = [row for row in receipts if row["kind"] in ("accepted", "replaced")
        and row["request_incarnation"] == execution["request_incarnation"]]
    if len(requests) != 1:
        raise ValueError("first-divergence immutable request is not unique")
    return requests[0], execution


def classify_first_divergence(batch_actions, tick_actions, batch_receipts, tick_receipts,
                              packets, minute_bars, difference, slippage=0):
    if difference is None:
        return {"status": "NO_DIVERGENCE", "actions_compared": len(tick_actions)}
    result = {"status": "UNEXPLAINED", "difference": difference, "slippage_ticks": slippage}
    try:
        match = re.match(r"^\[(\d+)\]", difference["path"])
        if match is None:
            raise ValueError("report-only divergence has no independent fill explanation")
        index = int(match.group(1))
        batch_action = batch_actions[index]
        tick_action = tick_actions[index]
        result.update(action_index=index, batch_action=batch_action, tick_action=tick_action)
        for field in ("action", "leg", "contracts", "id", "entry_incarnation", "reduce_only"):
            if batch_action["order"][field] != tick_action["order"][field]:
                raise ValueError("different order terms at first divergence: " + field)
        batch_request, batch_execution = execution_receipt(batch_receipts, batch_action)
        tick_request, tick_execution = execution_receipt(tick_receipts, tick_action)
        result.update(batch_request=batch_request, tick_request=tick_request,
            batch_execution=batch_execution, tick_execution=tick_execution)
        if (batch_request["type"], batch_request["levels"], batch_request["not_before"]) != (
                tick_request["type"], tick_request["levels"], tick_request["not_before"]):
            raise ValueError("order terms already differ before the fill")
        is_buy = tick_action["order"]["action"] == "buy"
        tick_oracle = first_fill(tick_request,
            (packet for packet in packets if packet["type"] == "tick"), is_buy, slippage=slippage)
        batch_oracle = first_fill(batch_request,
            modeled_points(script_bars(minute_bars), batch_request["not_before"]), is_buy,
            modeled=True, slippage=slippage)
        result.update(tick_oracle=tick_oracle, batch_oracle=batch_oracle)
        for name, oracle, execution in (("tick", tick_oracle, tick_execution), ("batch", batch_oracle, batch_execution)):
            if oracle is None:
                raise ValueError(name + " independent oracle found no fill")
            for field in ("timestamp", "sequence", "raw_price", "resolved_price", "path_phase"):
                if oracle[field] != execution[field]:
                    raise ValueError(name + " independent oracle disagrees on " + field)
        result["status"] = "EXPLAINED"
        result["rule"] = {"limit": "Observed non-open limit receives its level; modeled opening gap receives the better open.",
            "stop_limit": "Observed stop-limit activates at the first print; modeled continuous crossing activates at the stop level; the limit leg is unslipped.",
            "trail": "Chronological print best and first adverse print versus modeled auto-OHLC best and continuous stop crossing; observed prints gap to the quote.",
            "stop": "Observed stop gaps to the first qualifying print; modeled continuous stop crossing receives the level.",
            "market": "Observed next print versus modeled next opening waypoint."}[tick_request["type"]]
    except (ValueError, KeyError, IndexError, TypeError) as error:
        result["proof_gap"] = str(error)
    return result

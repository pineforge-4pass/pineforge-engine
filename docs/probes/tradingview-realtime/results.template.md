# TradingView realtime probe results

| Field | Value |
|---|---|
| Probe / mode | |
| Run ID | |
| Webhook / alert-log / trade counts | |
| Repeat run | |

## Pre-registered decision rule

| Item | Signature |
|---|---|
| Tested statement | |
| Confirming sequence | |
| Disconfirming sequence | |
| Invalidating/contaminating sequence | |

## Intended event sequence

Write the command and price sequence before examining the result.

## Observed event sequence

List trace and fill events in TradingView alert-log order. Include bar open time,
command bar, order tag, order ID, fill-bar open time, server time, fill price,
quantity, and resulting position.

## Cross-check

- Webhook versus TradingView alert log:
- Alert log versus Strategy Tester List of Trades:
- Intended price milestones present:
- Missing or duplicated events:

## Conclusion

Choose one: `confirmed`, `disconfirmed`, or `inconclusive`. State only the
semantic rule established by the evidence and list every remaining ambiguity.

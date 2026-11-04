# SMA oracle history

The active SMA checks use an independent correctly rounded binary64 window-sum
reference, followed by one ordinary division by the length.

the earlier pins disagree with TradingView's current values in the last binary digit; whether TradingView changed since 2026-09-05 is not known; the earlier pins are kept as history

The byte-preserved earlier test sources are under
history/pin-2026-09-05-capture-unconfirmed/ and are not compiled.
The existing exact-order and window-residual targets remain active.
The four retained tables check math.sum / n against the earlier values;
current TradingView math.sum parity has not been established.

// Generated with schedules.inc: each tape the engine does not yet reproduce, the rule it
// needs, its row counts (TradingView, engine) and its first departing field (the pin
// validator's ten exact fields: row, field, TradingView, engine) at the change's head.
// Recorded, not asserted. Never edit by hand.
const KnownDivergence kKnownDivergences[] = {
    {"siblings-02", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"siblings-03", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 2, "exit_id", "T2", "T1"},
    {"siblings-05", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "qty", "1.0", "2.0"},
    {"siblings-06", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 2, "exit_id", "T2", "T1"},
    {"siblings-07", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"siblings-08", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "Close position order"},
    {"siblings-09", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"siblings-10", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"siblings-11", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"siblings-12", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"reversal-30", "reversal: a close before a reversal leaves the requested opening leg", 4, 2, 0, "", "", ""},
    {"reversal-31", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"reversal-32", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"reversal-33", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"reversal-34", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"siblings-49", "rule 1: child identity per (exit id, entry incarnation)", 4, 3, 3, "exit_id", "T1", "T2"},
    {"reversal-51", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"reversal-52", "reversal: a close before a reversal leaves the requested opening leg", 5, 3, 0, "", "", ""},
    {"reversal-90", "reversal: a close before a reversal leaves the requested opening leg", 4, 3, 0, "", "", ""},
    {"reversal-95", "rule 6: a range-end row reports the entry commission only", 1, 1, 1, "commission", "40.0", "80.0"},
    {"reversal-96", "reversal: a close before a reversal leaves the requested opening leg", 3, 2, 0, "", "", ""},
    {"terminal-pct", "rule 6: a range-end row reports the entry commission only", 2, 2, 1, "commission", "1.250007486", "2.50275355785"},
    {"terminal-part", "rule 6: a range-end row reports the entry commission only", 3, 3, 1, "commission", "2.498023273", "2.4980232732"},
    {"hist13-same", "rule 7: a same-bar close(id) and entry(id) run the entry first", 15, 15, 2, "exit_time", "1753857000000", "1753857900000"}
};

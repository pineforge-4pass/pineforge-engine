# s1a_text_invalid_ordinary

Ordinary run, with no selected-window flags. This probe is ready for P0's ordinary driver. Pinned before implementation/execution: strategy_value_invalid, field=entry.comment, reason=invalid_unicode.

The source literal é has UTF-8 bytes C3 A9. visit_call.py:3743-3759 lowers str.substring to std::string::substr with byte offsets, so [0,1) returns the truncated C3 byte. No out-of-range or invalid source encoding is needed.

Forty synthetic one-minute bars: timestamp=1704067200000+i*60000, O=100 H=101 L=99 C=100 V=1000. The invalid command executes at index 2. Paired native row: entry.comment/ordinary/truncated2.

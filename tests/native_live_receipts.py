"""Sequence indexes for complete live-delivery receipt phases."""


def receipts_by_sequence(receipts, sequence_of=lambda receipt: receipt["sequence"]):
    """Keep each complete receipt and reject duplicates instead of overwriting them."""
    indexed = {}
    for receipt in receipts:
        sequence = sequence_of(receipt)
        if sequence in indexed:
            raise RuntimeError(f"duplicate receipt sequence: {sequence}")
        indexed[sequence] = receipt
    return indexed

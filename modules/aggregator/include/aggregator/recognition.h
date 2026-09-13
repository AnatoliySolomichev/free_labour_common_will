#pragma once

// Which acceptances count (records.md §9.5, ИР-023): one work — one recognized
// acceptance. Every view that reads acceptances asks here, so that no two views
// disagree about which deal happened. The rule itself is records' — the same
// one the client applies (records/acceptance.h); this only gathers the claims.

#include "aggregator.h"

#include <records/acceptance.h>

namespace aggregator {

// One pass over the storage. A claim is an acceptance written by its receiver
// (the spoof guard every view applies) that is not a self-acceptance; it is
// settled when the transfers naming it in `reason`, each from its own sender,
// add up to labor + carried exactly.
records::AcceptanceRecognition recognize_acceptances(const AggregatorStorage& storage);

} // namespace aggregator

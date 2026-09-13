#include "records/acceptance.h"

#include <cmath>
#include <map>

namespace records {

AcceptanceRecognition recognize_acceptances(const std::vector<AcceptanceClaim>& claims) {
    using Hash32 = std::array<uint8_t, 32>;
    struct Tally { std::set<Hash32> all, settled; };
    std::map<Hash32, Tally> by_work;
    for (const auto& c : claims) {
        auto& t = by_work[c.work];
        t.all.insert(c.acceptance);          // the same block seen twice is one claim
        if (c.settled) t.settled.insert(c.acceptance);
    }

    AcceptanceRecognition out;
    for (const auto& [work, t] : by_work) {
        if (t.settled.size() == 1)     out.recognized.insert(*t.settled.begin());
        else if (t.settled.size() > 1) out.oversold.insert(work);
        else if (t.all.size() == 1)    out.recognized.insert(*t.all.begin());
    }
    return out;
}

bool settled_exactly(double paid, double payable) noexcept {
    return paid > 0.0 && std::abs(paid - payable) <= 1e-6;
}

} // namespace records

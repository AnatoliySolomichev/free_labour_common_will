#include "records/acceptance.h"

#include <cmath>
#include <map>

namespace records {

DealRecognition recognize_deals(const std::vector<DealClaim>& claims) {
    using Hash32 = std::array<uint8_t, 32>;
    std::map<Hash32, std::set<Hash32>> by_work;   // the same block seen twice is one deal
    for (const auto& c : claims) by_work[c.work].insert(c.deal);

    DealRecognition out;
    for (const auto& [work, deals] : by_work) {
        if (deals.size() == 1) out.recognized.insert(*deals.begin());
        else                   out.oversold.insert(work);
    }
    return out;
}

bool settled_exactly(double paid, double payable) noexcept {
    return paid > 0.0 && std::abs(paid - payable) <= 1e-6;
}

} // namespace records

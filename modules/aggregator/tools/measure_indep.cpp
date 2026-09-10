// Замер ИР-021 на журнале сделок прогона года — самим build_daily_rates.
//
// Что меряется:
//   1. какую долю ЧЕСТНОГО объёма задевает дисконт независимости — по разрядам
//      и в сумме;
//   2. какая доля объёма внедрённого СГОВОРА уцелевает. Сговор — пара, каждый
//      месяц платящая друг другу по 6 ч в самой ходовой специальности по цене
//      k × средняя её самого ходового разряда: «на k дороже, чем получает их
//      разряд».
//
// Входные данные одни и те же при любой методике — меняется только то, что
// считает агрегатор, поэтому числа до и после правки сравнимы напрямую. Каждый
// вид сделок кладётся в свои сутки (сговор — отдельно, честные — по разряду), и
// агрегат этих суток читается целиком: окно независимости при этом видит все
// сделки, а свёртка дня — только нужный вид.
//
// Прототип docs/pilot/pair-independence.py отстал от методики (медиана и
// постоянный порог против средней по сделкам и самонастраиваемого порога),
// поэтому замер идёт кодом агрегатора.
//
// Запуск: aggregator_measure_indep docs/pilot/sim-out/deals.tsv [k …]
// Журнал пишет docs/pilot/sim-year.sh. Блоки подписываются случайными ключами;
// в расчёт подписи не входят, результат воспроизводим с точностью до округления.

#include "aggregator/rates_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace aggregator;
using namespace blockchain;

namespace {

struct Row {
    int         month = 0, payer = 0, worker = 0, level = 0;
    std::string spec;
    double      hours = 0.0, coef = 0.0;
};

constexpr int64_t kDay  = 86'400;
constexpr int64_t kBase = kDay * 1000;
constexpr int     kAttackA = 1'000'001, kAttackB = 1'000'002;

int64_t month_day(int m)           { return kBase + m * 30LL * kDay; }
int64_t attack_day(int m)          { return month_day(m) + kDay; }
int64_t honest_day(int m, int lvl) { return month_day(m) + (2 + lvl) * kDay; }

UserId person(int id) {
    UserId u{};
    u.bytes.fill(0x5A);
    for (int i = 0; i < 4; ++i) u.bytes[i] = static_cast<uint8_t>(id >> (8 * i));
    return u;
}

records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

class World {
public:
    explicit World(std::filesystem::path dir) : dir_(std::move(dir)) {
        std::filesystem::remove_all(dir_);
        storage_ = std::make_unique<AggregatorStorage>(dir_);
    }
    ~World() {
        storage_.reset();
        std::filesystem::remove_all(dir_);
    }
    const AggregatorStorage& storage() const { return *storage_; }

    void deal(int payer, int worker, const std::string& spec, int level,
              double hours, double coef, int64_t ts) {
        const UserId p = person(payer), w = person(worker);
        records::WorkRecord wr{};
        wr.agent = grade(w, spec, level);
        wr.hours = hours;
        const Block work = add(w, wr);

        const double units = hours * coef;
        records::BlockPath axis{};
        axis.chain.fill(0x79);
        axis.hash.fill(0x01);
        records::Acceptance a{};
        a.work        = ref_to(w, work);
        a.receiver    = p.bytes;
        a.hours_raw   = hours;
        a.labor_units = units;
        a.axes        = {{axis, units}};
        a.timestamp   = ts;
        const Block acc = add(p, a);

        records::Transfer t{};
        t.from    = p.bytes;
        t.to      = w.bytes;
        t.origins = { {p.bytes, units} };
        t.reason  = ref_to(p, acc);
        add(p, t);
    }

private:
    std::filesystem::path              dir_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_ = 0;
    std::map<std::tuple<std::array<uint8_t, 32>, std::string, int>, records::Ref> grades_;

    Block add(const UserId& owner, const records::Record& rec) {
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 0;
        b.type              = BlockType::DATA;
        b.payload           = records::Codec::encode(rec);
        b.signature         = Signature::null();
        const auto bytes    = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        if (!storage_->add_block(b)) throw std::runtime_error("add_block refused");
        return b;
    }

    records::Ref grade(const UserId& w, const std::string& spec, int level) {
        const auto key = std::make_tuple(w.bytes, spec, level);
        if (const auto it = grades_.find(key); it != grades_.end()) return it->second;
        const Block s = add(w, records::Specialty{spec});
        records::Grade g{};
        g.specialty = ref_to(w, s);
        g.level     = static_cast<uint8_t>(level);
        return grades_[key] = ref_to(w, add(w, g));
    }
};

std::vector<Row> read_journal(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("нет журнала: " + path);
    std::vector<Row> rows;
    std::string line;
    std::getline(f, line);                                   // заголовок
    while (std::getline(f, line)) {
        std::istringstream in(line);
        Row r;
        std::string field;
        std::vector<std::string> p;
        while (std::getline(in, field, '\t')) p.push_back(field);
        if (p.size() < 7) continue;
        r.month  = std::stoi(p[0]);
        r.payer  = std::stoi(p[1]);
        r.worker = std::stoi(p[2]);
        r.spec   = p[3];
        r.level  = std::stoi(p[4]);
        r.hours  = std::stod(p[5]);
        r.coef   = std::stod(p[6]);
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: aggregator_measure_indep deals.tsv [k …]\n";
        return 1;
    }
    const auto rows = read_journal(argv[1]);
    std::vector<double> ks;
    for (int i = 2; i < argc; ++i) ks.push_back(std::stod(argv[i]));
    if (ks.empty()) ks = {1.2, 1.3, 1.5};

    std::set<int> months, levels;
    std::map<std::string, int> by_spec;
    for (const auto& r : rows) {
        months.insert(r.month);
        levels.insert(r.level);
        ++by_spec[r.spec];
    }
    std::string vspec;
    for (const auto& [s, n] : by_spec)
        if (vspec.empty() || n > by_spec[vspec]) vspec = s;
    std::map<int, int> by_level;
    for (const auto& r : rows) if (r.spec == vspec) ++by_level[r.level];
    int vlevel = 0;
    for (const auto& [l, n] : by_level)
        if (!vlevel || n > by_level[vlevel]) vlevel = l;
    double su = 0.0, sh = 0.0;
    for (const auto& r : rows)
        if (r.spec == vspec && r.level == vlevel) { su += r.hours * r.coef; sh += r.hours; }
    const double ref = su / sh;

    const IndependenceParams indep{};
    std::printf("ИР-021 на журнале прогона года: %zu сделок, %zu мес.\n",
                rows.size(), months.size());
    std::printf("методика: window=%lld max_cycle=%zu kappa=%.2f self_calibrate=%d "
                "calib_min_deals=%zu min_basket_deals=%zu kappa∈[%.2f, %.2f]\n\n",
                static_cast<long long>(indep.window_days), indep.max_cycle, indep.kappa,
                indep.self_calibrate ? 1 : 0, indep.calib_min_deals,
                indep.min_basket_deals, indep.kappa_min, indep.kappa_max);

    const auto tmp = std::filesystem::temp_directory_path() /
                     ("bc_measure_indep_" + std::to_string(::getpid()));
    // `one_day`: all honest deals of a month in ONE day, as a real aggregate sees
    // them — for the normalizer. Otherwise each grade gets its own day, so the
    // discount can be read per grade.
    auto build = [&](World& w, std::optional<double> k, bool one_day = false) {
        std::map<std::pair<int, int>, int> seq;
        for (const auto& r : rows) {
            const int     n   = seq[{r.month, one_day ? 0 : r.level}]++;
            const int64_t day = one_day ? month_day(r.month) + 2 * kDay
                                        : honest_day(r.month, r.level);
            w.deal(r.payer, r.worker, r.spec, r.level, r.hours, r.coef,
                   day + 60 + (n * 37) % 80'000);
        }
        if (k)
            for (int m : months) {
                w.deal(kAttackA, kAttackB, vspec, vlevel, 6.0, *k * ref, attack_day(m) + 100);
                w.deal(kAttackB, kAttackA, vspec, vlevel, 6.0, *k * ref, attack_day(m) + 200);
            }
    };
    // min_hours = 0: ни одна корзина не пропускается, даже целиком ушедшая в дисконт.
    auto fold = [&](const World& w, int64_t day, double& wh, double& h) {
        for (const auto& e : build_daily_rates(w.storage(), day, {}, 0.3, 0.0, nullptr, &indep)) {
            wh += e.weighted_hours;
            h  += e.hours;
        }
    };

    {
        World w(tmp / "honest");
        build(w, std::nullopt);
        std::printf("честный объём, задетый дисконтом:\n");
        double twh = 0.0, th = 0.0;
        for (int l : levels) {
            double wh = 0.0, h = 0.0;
            for (int m : months) fold(w, honest_day(m, l), wh, h);
            std::printf("  разряд %d: %5.2f%%  (из %.0f ч)\n", l, 100.0 * (1.0 - wh / h), h);
            twh += wh;
            th  += h;
        }
        std::printf("  всего:    %5.2f%%  (из %.0f ч)\n\n", 100.0 * (1.0 - twh / th), th);
    }

    {
        // Нормировщик W (economy.md §2б): насколько дисконт сдвигает средний
        // трудочас сети — то, во что уценка веса сделок превращается для денег.
        World w(tmp / "oneday");
        build(w, std::nullopt, true);
        double swr = 0.0, sw = 0.0;
        for (int m : months)
            for (const auto& e : build_daily_rates(w.storage(), month_day(m) + 2 * kDay,
                                                   {}, 0.3, 0.0, nullptr, &indep))
                if (std::isfinite(e.rate)) { swr += e.rate * e.hours; sw += e.hours; }
        double tu = 0.0, tt = 0.0;
        for (const auto& r : rows) { tu += r.hours * r.coef; tt += r.hours; }
        const double raw = tu / tt, weighted = swr / sw;
        std::printf("нормировщик W: по сделкам как есть %.4f, после взвешивания %.4f "
                    "(сдвиг %+.2f%%)\n\n", raw, weighted, 100.0 * (weighted / raw - 1.0));
    }

    std::printf("сговор: пара в %s р.%d (средняя разряда %.3f), по 6 ч в обе стороны "
                "каждый месяц\n", vspec.c_str(), vlevel, ref);
    for (double k : ks) {
        World w(tmp / ("k" + std::to_string(k)));
        build(w, k);
        double wh = 0.0, h = 0.0;
        for (int m : months) fold(w, attack_day(m), wh, h);
        std::printf("  k=%.2f: уцелело %5.1f%% объёма сговора\n", k, 100.0 * wh / h);
    }
    return 0;
}

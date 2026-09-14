// Замер ИР-021 на журнале сделок прогона года — самим build_daily_prices.
//
// Мир строится из журнала: у каждой сделки — разбивка по осям той же таблицей,
// что axis_shares() в sim-year.sh (шесть групп профессий). Надбавка за разряд в
// прогоне — 0.10 стч/ч на ступень от 4.5 (6-й разряд — +0.15). Два мира:
//
//   «не названа» — надбавка размазана по осям группы, как делает sim-year;
//   «одной осью» — мастера 5–6-го разрядов кладут её на отдельную ось
//                  «Уровень решений» (decision из seed-axes.sh), остальное —
//                  по осям группы. Так мастер «аргументирует надбавку осью».
//   «по ступеням» — каждая ступень выше 3-й называет свою ось («мастерство-4»,
//                  «мастерство-5», …) на всю разницу с 3-й: разные умения —
//                  разные оси, как «работа под напряжением 400 В» и «220 В».
//
// Что меряется в каждом мире:
//   1. доля ЧЕСТНОГО объёма под дисконтом независимости — по разрядам и в сумме;
//   2. сдвиг нормировщика W (economy.md §2б) — во что уценка превращается для
//      денег;
//   3. доля уцелевшего объёма СГОВОРА: пара, каждый месяц платящая друг другу
//      по 6 ч в самой ходовой специальности по цене k × средняя её самого
//      ходового разряда. Три способа спрятать завышение: в общих осях группы,
//      на оси мастерства, на своей оси, которую не называет больше никто.
//
// Входные данные одни и те же при любой методике — меняется только то, что
// считает агрегатор, поэтому числа до и после правки сравнимы напрямую. Каждый
// вид сделок кладётся в свои сутки (сговор — отдельно, честные — по разряду), и
// агрегат этих суток читается целиком: окно независимости видит все сделки, а
// свёртка дня — только нужный вид. Для W все честные сделки месяца лежат в одних
// сутках, как их видит настоящий агрегатор.
//
// Прежний прототип на Python (pair-independence.py) удалён 2026-09-11: он отстал
// от методики, и замер теперь идёт кодом агрегатора.
//
// Запуск: aggregator_measure_indep docs/pilot/sim-out/deals.tsv [k …]
// Журнал пишет docs/pilot/sim-year.sh. Блоки подписываются случайными ключами;
// в расчёт подписи не входят, результат воспроизводим с точностью до округления.

#include "aggregator/rates_view.h"
#include "../tests/labor_thread_support.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
#include <utility>
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
int64_t oneday(int m)              { return month_day(m) + 2 * kDay; }

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

// Ось — по слагу: определения осей замеру не нужны, нужна лишь их личность.
records::BlockPath axis_path(const std::string& slug) {
    records::BlockPath p{};
    p.chain.fill(0x79);
    std::memcpy(p.hash.data(), slug.data(), std::min<std::size_t>(slug.size(), 32));
    return p;
}

using Lines = std::vector<std::pair<std::string, double>>;   // ось → стч

// Та же таблица, что axis_shares() в sim-year.sh.
Lines group_shares(const std::string& spec) {
    auto in = [&](std::initializer_list<const char*> xs) {
        for (const char* x : xs) if (spec == x) return true;
        return false;
    };
    if (in({"prof.programmer", "prof.designer", "prof.accountant", "prof.scribe",
            "prof.sysadmin"}))
        return {{"knowledge", 0.5}, {"precision", 0.3}, {"creativity", 0.2}};
    if (in({"prof.doctor", "prof.nurse"}))
        return {{"responsibility", 0.4}, {"knowledge", 0.35}, {"emotional", 0.25}};
    if (in({"prof.teacher", "prof.lawyer"}))
        return {{"knowledge", 0.45}, {"emotional", 0.3}, {"responsibility", 0.25}};
    if (in({"prof.welder", "prof.builder", "prof.farmer"}))
        return {{"physical", 0.45}, {"danger", 0.3}, {"endurance", 0.25}};
    if (in({"prof.electrician", "prof.plumber", "prof.auto-mechanic",
            "prof.appliance-repair"}))
        return {{"knowledge", 0.35}, {"precision", 0.35}, {"danger", 0.3}};
    return {{"physical", 0.4}, {"precision", 0.35}, {"pace", 0.25}};
}

// Надбавка за разряд в прогоне года (coef в sim-year.sh): 0.10 стч/ч на ступень
// от 4.5. Её есть что называть только у 5-го и 6-го разрядов.
double grade_premium(int level) { return level >= 5 ? 0.10 * (level - 4.5) : 0.0; }

const char* const kMasterAxis  = "decision";   // «Уровень решений», seed-axes.sh
const char* const kPrivateAxis = "our-own";    // ось, которую называет одна пара

// `base` — по долям группы, `extra` — отдельной строкой на ось `extra_axis`.
Lines split(const std::string& spec, double base, const char* extra_axis, double extra) {
    Lines out;
    for (const auto& [axis, share] : group_shares(spec)) out.emplace_back(axis, base * share);
    if (extra_axis && extra > 0.0) out.emplace_back(extra_axis, extra);
    return out;
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
              double hours, const Lines& lines, int64_t ts) {
        const UserId p = person(payer), w = person(worker);
        records::WorkRecord wr{};
        wr.hours = hours;
        wr.title = spec + ", " + std::to_string(level) + "-й разряд";   // для людей
        wr.customer = p.bytes;   // на заказ плательщика (records.md §9.4)
        const Block work = add(w, wr);

        records::Acceptance a{};
        double units = 0.0;
        for (const auto& [axis, u] : lines) {
            a.axes.push_back({axis_path(axis), u});
            units += u;
        }
        a.work        = ref_to(w, work);
        a.hours_raw   = hours;
        a.labor_units = units;

        // Приёмка — внутри своей оплаты (records.md §9.5, ИР-023).
        records::Transfer t{};
        t.from      = p.bytes;
        t.to        = w.bytes;
        t.origins   = { {p.bytes, units} };
        t.accept    = a;
        t.timestamp = ts;
        add(p, t);
    }

private:
    std::filesystem::path              dir_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_ = 0;
    LaborThreads threads_;   // records.md §9.4 v4: works go into the thread
    Block add(const UserId& owner, const records::Record& written) {
        const records::Record rec = threads_.stamp(owner.bytes, written);
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
        threads_.placed(owner.bytes, rec, Crypto::hash_block(b).bytes);
        return b;
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
        std::string field;
        std::vector<std::string> p;
        while (std::getline(in, field, '\t')) p.push_back(field);
        if (p.size() < 7) continue;
        Row r;
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

enum class Attack { Hidden, Master, Private };
enum class Mode   { Hidden, OneAxis, PerStep };

// Ось, на которую мастер кладёт надбавку в данном мире, и сама надбавка, стч/ч.
std::pair<std::string, double> named_premium(Mode mode, int level) {
    if (mode == Mode::OneAxis) return {kMasterAxis, grade_premium(level)};
    if (mode == Mode::PerStep && level > 3)
        return {"mastery-" + std::to_string(level), 0.10 * (level - 3)};
    return {"", 0.0};
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
    if (ks.empty()) ks = {1.3};

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
                "calib_min_deals=%zu min_axis_payers=%zu kappa∈[%.2f, %.2f]\n",
                static_cast<long long>(indep.window_days), indep.max_cycle, indep.kappa,
                indep.self_calibrate ? 1 : 0, indep.calib_min_deals,
                indep.min_axis_payers, indep.kappa_min, indep.kappa_max);
    std::printf("сговор: пара в %s р.%d (средняя разряда %.3f), по 6 ч в обе стороны "
                "каждый месяц\n", vspec.c_str(), vlevel, ref);

    const auto tmp = std::filesystem::temp_directory_path() /
                     ("bc_measure_indep_" + std::to_string(::getpid()));

    auto lines_for = [&](const std::string& spec, int level, double hours, double coef,
                         Mode mode) {
        const auto [axis, per_hour] = named_premium(mode, level);
        const double prem = hours * per_hour;
        return split(spec, hours * coef - prem, axis.empty() ? nullptr : axis.c_str(), prem);
    };
    auto build = [&](World& w, Mode mode, bool one_day,
                     std::optional<std::pair<Attack, double>> attack) {
        std::map<std::pair<int, int>, int> seq;
        for (const auto& r : rows) {
            const int     n   = seq[{r.month, one_day ? 0 : r.level}]++;
            const int64_t day = one_day ? oneday(r.month) : honest_day(r.month, r.level);
            w.deal(r.payer, r.worker, r.spec, r.level, r.hours,
                   lines_for(r.spec, r.level, r.hours, r.coef, mode),
                   day + 60 + (n * 37) % 80'000);
        }
        if (!attack) return;
        // Честная часть сговора устроена как настоящая сделка того же разряда в
        // этом мире; завышение ложится туда, куда его прячут.
        const auto [how, k] = *attack;
        const double over = 6.0 * (k - 1.0) * ref;
        Lines lines = lines_for(vspec, vlevel, 6.0, ref, mode);
        const auto master = named_premium(mode, vlevel).first;
        const Lines extra =
            how == Attack::Hidden ? split(vspec, over, nullptr, 0.0)
          : how == Attack::Master ? Lines{{master.empty() ? kMasterAxis : master, over}}
                                  : Lines{{kPrivateAxis, over}};
        lines.insert(lines.end(), extra.begin(), extra.end());
        for (int m : months) {
            w.deal(kAttackA, kAttackB, vspec, vlevel, 6.0, lines, attack_day(m) + 100);
            w.deal(kAttackB, kAttackA, vspec, vlevel, 6.0, lines, attack_day(m) + 200);
        }
    };
    // min_hours = 0: ни одна ось не пропускается, даже целиком ушедшая в дисконт.
    // Часы сделки входят в каждую её ось, но вес у всех осей сделки один, поэтому
    // доля уцелевшего объёма по осям та же, что по сделкам.
    auto fold = [&](const World& w, int64_t day, double& wh, double& h, double* paid = nullptr) {
        for (const auto& e : build_daily_prices(w.storage(), day, {}, 0.3, 0.0, &indep).axes) {
            wh += e.weighted_hours;
            h  += e.hours;
            if (paid && std::isfinite(e.price)) *paid += e.price * e.hours;
        }
    };

    const std::pair<Mode, const char*> modes[] = {
        {Mode::Hidden,  "НЕ названа (размазана по осям группы, как в sim-year)"},
        {Mode::OneAxis, "названа ОДНОЙ осью «Уровень решений» (разряды 5–6)"},
        {Mode::PerStep, "каждая ступень названа СВОЕЙ осью (мастерство-4, -5, -6)"}};
    for (const auto& [mode, title] : modes) {
        std::printf("\nнадбавка мастеров %s:\n", title);
        {
            World w(tmp / "honest");
            build(w, mode, false, std::nullopt);
            std::printf("  честный объём под дисконтом:");
            double lost = 0.0, all = 0.0;               // по часам сделок, не осей
            for (int l : levels) {
                double wh = 0.0, h = 0.0, hl = 0.0;
                for (int m : months) fold(w, honest_day(m, l), wh, h);
                for (const auto& r : rows) if (r.level == l) hl += r.hours;
                const double share = 1.0 - wh / h;
                std::printf("  р.%d %.2f%%", l, 100.0 * share);
                lost += share * hl;
                all  += hl;
            }
            std::printf("  · всего %.2f%%\n", 100.0 * lost / all);
        }
        {
            World w(tmp / "oneday");
            build(w, mode, true, std::nullopt);
            double wh = 0.0, h = 0.0, paid = 0.0;
            for (int m : months) fold(w, oneday(m), wh, h, &paid);
            double tu = 0.0, tt = 0.0;
            for (const auto& r : rows) { tu += r.hours * r.coef; tt += r.hours; }
            const double raw = tu / tt, weighted = paid / tt;
            std::printf("  нормировщик W: как есть %.4f, после взвешивания %.4f (сдвиг %+.2f%%)\n",
                        raw, weighted, 100.0 * (weighted / raw - 1.0));
        }
        for (double k : ks) {
            std::printf("  сговор ×%.2f уцелел:", k);
            const std::pair<Attack, const char*> hows[] = {
                {Attack::Hidden,  "в общих осях"},
                {Attack::Master,  "на оси мастерства"},
                {Attack::Private, "на своей оси"}};
            for (const auto& [how, label] : hows) {
                World w(tmp / "attack");
                build(w, mode, false, std::make_pair(how, k));
                double wh = 0.0, h = 0.0;
                for (int m : months) fold(w, attack_day(m), wh, h);
                std::printf("  %s %.1f%%", label, 100.0 * wh / h);
            }
            std::printf("\n");
        }
    }
    return 0;
}

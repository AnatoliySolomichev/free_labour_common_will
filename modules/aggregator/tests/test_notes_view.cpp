#include "aggregator/notes_view.h"
#include "aggregator/server.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/social.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace aggregator;
using namespace blockchain;

// ── Заметки о предметах: индекс агрегатора (social.md §7) ────────────────────

namespace {

const std::string kVideo = "yt:dQw4w9WgXcQ";

UserId make_owner(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::Note note(records::Subject about, std::string kind, std::optional<int64_t> v = std::nullopt,
                   std::string text = {}, std::optional<records::Ref> re = std::nullopt) {
    records::Note n;
    n.about = std::move(about);
    n.kind  = std::move(kind);
    n.v     = v;
    n.text  = std::move(text);
    n.re    = re;
    return n;
}

records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

class NotesViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 1;
    const UserId anna_  = make_owner(0xA1);
    const UserId boris_ = make_owner(0xB2);
    const UserId vera_  = make_owner(0xC3);

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_notesview_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // Время блока растёт с номером: порядок записи — порядок времени.
    Block add(const UserId& owner, const records::Record& rec) {
        return add_at(owner, rec, static_cast<Timestamp>(next_index_) * 1000LL);
    }

    Block add_at(const UserId& owner, const records::Record& rec, Timestamp at) {
        return add_payload(owner, records::Codec::encode(rec), at);
    }

    // Блок со списком записей (records.md §2) — суточный пакет просмотров.
    Block add_list(const UserId& owner, const std::vector<records::Record>& recs) {
        return add_payload(owner, records::Codec::encode_list(recs),
                           static_cast<Timestamp>(next_index_) * 1000LL);
    }

    Block add_payload(const UserId& owner, std::vector<uint8_t> payload, Timestamp at) {
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = at;
        b.type              = BlockType::DATA;
        b.payload           = std::move(payload);
        b.signature         = Signature::null();
        const auto    bytes = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }
};

} // namespace

TEST_F(NotesViewTest, IndexesBySubjectAndByAuthor) {
    add(anna_,  note(kVideo, "react", 1));
    add(boris_, note(kVideo, "comment", std::nullopt, "хороший разбор"));
    add(vera_,  note(records::ChainId{anna_.bytes}, "follow", 1));
    records::Concept idea;
    idea.text = "не заметка";
    add(anna_, idea);                                   // прочие записи индекс не видит

    const auto board = read_notes(*storage_);
    EXPECT_EQ(board.notes.size(), 3u);
    const auto about = board.about(kVideo);
    ASSERT_EQ(about.size(), 2u);
    EXPECT_EQ(about[0]->author, anna_);                 // по времени
    EXPECT_EQ(about[1]->note.text, "хороший разбор");
    EXPECT_EQ(board.by(anna_).size(), 1u);
    EXPECT_EQ(board.by(vera_, "follow").size(), 1u);
    EXPECT_TRUE(board.by(vera_, "react").empty());
    // Кто подписан на цепь — заметки о ней как о предмете.
    const auto followers = board.about(records::subject_id(records::ChainId{anna_.bytes}));
    ASSERT_EQ(followers.size(), 1u);
    EXPECT_EQ(followers[0]->author, vera_);
}

// Обсуждение — одним ответом: комментарии, ответы и заметки о каждом комментарии.
TEST_F(NotesViewTest, ThreadCarriesNotesAboutEveryComment) {
    const Block c = add(boris_, note(kVideo, "comment", std::nullopt, "первый"));
    const auto  cref = ref_to(boris_, c);
    add(anna_,  note(cref, "react", 1));                                  // лайк комментарию
    add(vera_,  note(kVideo, "comment", std::nullopt, "ответ", cref));   // ответ: предмет — ролик
    add(boris_, note(cref, "retract"));                                   // автор снял
    add(anna_,  note(std::string("yt:otherVideo1"), "react", -1));        // другое обсуждение

    const auto board = read_notes(*storage_);
    EXPECT_EQ(board.about(kVideo).size(), 2u);          // комментарий и ответ
    const auto thread = board.thread(kVideo);
    ASSERT_EQ(thread.size(), 4u);
    EXPECT_EQ(thread[0]->note.text, "первый");
    EXPECT_EQ(thread[1]->note.kind, "react");
    EXPECT_EQ(thread[2]->note.text, "ответ");
    EXPECT_EQ(thread[3]->note.kind, "retract");
}

// В одну секунду порядок решает номер блока в ветке, а не хеш: иначе «последнее
// слово» выбирал бы случай (records.md §8.11).
TEST_F(NotesViewTest, SameSecondNotesKeepTheirBranchOrder) {
    for (int i = 0; i < 6; ++i) add_at(anna_, note(kVideo, "react", i % 2 ? -1 : 1), 5000);
    const auto about = read_notes(*storage_).about(kVideo);
    ASSERT_EQ(about.size(), 6u);
    for (size_t i = 1; i < about.size(); ++i) EXPECT_LT(about[i - 1]->index, about[i]->index);
    EXPECT_EQ(about.back()->note.v, -1);                // последнее слово — последний блок
}

// Суточный пакет: каждая заметка списка — своя запись индекса, со своим местом.
TEST_F(NotesViewTest, ListBlockNotesAreIndexedOneByOne) {
    auto seg = note(kVideo, "view", 300);   // 3x
    seg.from = 0;
    seg.to   = 1800;
    const Block day = add_list(vera_, {seg, note(std::string("tg:durov/42"), "view")});
    add(anna_, note(kVideo, "react", 1));

    const auto board = read_notes(*storage_);
    EXPECT_EQ(board.notes.size(), 3u);
    const auto views = board.by(vera_, "view");
    ASSERT_EQ(views.size(), 2u);
    EXPECT_EQ(views[0]->pos, 0u);
    EXPECT_EQ(views[1]->pos, 1u);
    EXPECT_EQ(views[0]->block, views[1]->block);        // один блок на двоих
    EXPECT_EQ(views[0]->block, Crypto::hash_block(day));
    const auto about = board.about(kVideo);
    ASSERT_EQ(about.size(), 2u);                        // просмотр Веры и реакция Анны
    EXPECT_EQ(about[0]->note.to, 1800);
}

TEST_F(NotesViewTest, BoardIsRecomputedOnlyWhenBlocksArrive) {
    add(anna_, note(kVideo, "react", 1));
    const auto first = notes_board(*storage_);
    EXPECT_EQ(notes_board(*storage_), first);           // блоков не прибавилось
    add(boris_, note(kVideo, "react", -1));
    const auto second = notes_board(*storage_);
    EXPECT_NE(second, first);
    EXPECT_EQ(second->about(kVideo).size(), 2u);
}

TEST_F(NotesViewTest, NotesEndpointServesThreadAndAuthor) {
    const Block c = add(boris_, note(kVideo, "comment", std::nullopt, "кавычки \" и\nперевод"));
    add(anna_, note(ref_to(boris_, c), "react", 1));
    add(vera_, note(records::ChainId{anna_.bytes}, "follow", 1));
    auto seg = note(kVideo, "view", 150);
    seg.from = 60;
    seg.to   = 360;
    add_list(vera_, {seg, note(std::string("tg:durov/42"), "view")});

    const uint16_t port = 18796;
    AggregatorServer server(*storage_, port, {}, std::chrono::seconds(3600));
    std::thread th([&] { server.run(); });
    httplib::Client cli("127.0.0.1:" + std::to_string(port));
    for (int i = 0; i < 100; ++i) {
        if (auto r = cli.Get("/stats"); r && r->status == 200) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto thread  = cli.Get("/notes?about=yt%3AdQw4w9WgXcQ");
    const auto follows = cli.Get(("/notes?by=" + records::subject_id(records::ChainId{vera_.bytes}) +
                                  "&kind=follow").c_str());
    const auto bad     = cli.Get("/notes?about=no-such-thing");
    const auto none    = cli.Get("/notes");
    server.stop();
    th.join();

    ASSERT_TRUE(thread && thread->status == 200);
    EXPECT_NE(thread->body.find("\"kind\":\"comment\""), std::string::npos);
    EXPECT_NE(thread->body.find("\"text\":\"кавычки \\\" и\\nперевод\""), std::string::npos)
        << thread->body;
    EXPECT_NE(thread->body.find("\"about\":\"" + records::subject_id(ref_to(boris_, c)) + "\""),
              std::string::npos);                       // реакция на комментарий — в обсуждении
    EXPECT_NE(thread->body.find("\"v\":1"), std::string::npos);
    EXPECT_NE(thread->body.find("\"index\":" + std::to_string(c.address.block_index)),
              std::string::npos);
    EXPECT_NE(thread->body.find("\"kind\":\"view\",\"v\":150,\"from\":60,\"to\":360"),
              std::string::npos) << thread->body;
    EXPECT_NE(thread->body.find("\"pos\":0"), std::string::npos);
    ASSERT_TRUE(follows && follows->status == 200);
    EXPECT_NE(follows->body.find("\"kind\":\"follow\""), std::string::npos);
    EXPECT_EQ(follows->body.find("\"kind\":\"comment\""), std::string::npos);
    ASSERT_TRUE(bad && none);
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(none->status, 400);
}

#include "aggregator/notes_view.h"

#include "aggregator/recognition.h"

#include <records/social.h>

#include <algorithm>
#include <mutex>
#include <tuple>

namespace aggregator {

namespace {

void by_time(std::vector<const NoteEntry*>& out) {
    std::sort(out.begin(), out.end(), [](const NoteEntry* a, const NoteEntry* b) {
        return std::tie(a->at, a->index, a->block.bytes) < std::tie(b->at, b->index, b->block.bytes);
    });
}

std::string comment_id(const NoteEntry& e) {
    records::Ref r{};
    r.chain = e.author.bytes;
    r.hash  = e.block.bytes;
    return records::subject_id(records::Subject{r});
}

} // namespace

std::vector<const NoteEntry*> NotesBoard::about(const std::string& subject) const {
    std::vector<const NoteEntry*> out;
    if (const auto it = by_subject.find(subject); it != by_subject.end())
        for (const size_t i : it->second) out.push_back(&notes[i]);
    by_time(out);
    return out;
}

std::vector<const NoteEntry*> NotesBoard::thread(const std::string& subject) const {
    auto out = about(subject);
    const size_t root = out.size();
    for (size_t i = 0; i < root; ++i)
        if (out[i]->note.kind == "comment")
            for (const NoteEntry* e : about(comment_id(*out[i]))) out.push_back(e);
    by_time(out);
    return out;
}

std::vector<const NoteEntry*> NotesBoard::by(const UserId& author, const std::string& kind) const {
    std::vector<const NoteEntry*> out;
    if (const auto it = by_author.find(author); it != by_author.end())
        for (const size_t i : it->second)
            if (kind.empty() || notes[i].note.kind == kind) out.push_back(&notes[i]);
    by_time(out);
    return out;
}

NotesBoard read_notes(const AggregatorStorage& storage) {
    NotesBoard out;
    for (const Hash& bh : storage.all_block_hashes()) {
        const SeenBlock& block = seen_block(storage, bh);
        if (!block.data) continue;
        const auto* note = std::get_if<records::Note>(&block.record);
        if (!note) continue;
        const size_t i = out.notes.size();
        out.notes.push_back(NoteEntry{bh, block.address.user_id, block.timestamp_claimed,
                                      block.address.block_index, *note});
        out.by_subject[records::subject_id(note->about)].push_back(i);
        out.by_author[block.address.user_id].push_back(i);
    }
    return out;
}

std::shared_ptr<const NotesBoard> notes_board(const AggregatorStorage& storage) {
    static std::mutex                        mu;
    static uint64_t                          cached_id    = 0;
    static std::size_t                       cached_count = 0;
    static std::shared_ptr<const NotesBoard> cached;
    std::lock_guard<std::mutex> lock(mu);   // один считает, остальные ждут
    const std::size_t count = storage.block_count();
    if (cached && cached_id == storage.instance_id() && cached_count == count) return cached;
    cached       = std::make_shared<const NotesBoard>(read_notes(storage));
    cached_id    = storage.instance_id();
    cached_count = count;
    return cached;
}

} // namespace aggregator

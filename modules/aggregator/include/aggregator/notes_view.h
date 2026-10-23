#pragma once

#include "aggregator.h"

#include <records/types.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aggregator {

// ── Заметки о предметах (records.md §8.11, social.md §7) ─────────────────────
//
// Индекс по предмету и по автору — для соцслоя, а потом для соревнований и
// отзывов о продукции (ИР-026). Заметки сырые: последнее слово и круг
// применяет клиент — агрегатор не знает, кто спрашивает, и не должен (ИР-010).
// Как всякая витрина — кеш и индекс, а не источник истины: каждая заметка —
// блок, подписанный веткой автора (sync.md §10.10).

struct NoteEntry {
    Hash          block{};    // Ref заметки = (author, block)
    UserId        author{};   // чья заметка — говорит адрес блока
    Timestamp     at = 0;     // время блока
    BlockIndex    index = 0;  // номер блока в ветке: порядок при равном времени
    uint32_t      pos   = 0;  // место в списке записей блока (records.md §2); одна — 0
    records::Note note;
};

struct NotesBoard {
    std::vector<NoteEntry>                     notes;        // в порядке хранилища
    std::map<std::string, std::vector<size_t>> by_subject;   // records::subject_id → индексы
    std::map<UserId, std::vector<size_t>>      by_author;

    // Заметки о предмете, по времени; при равном — по номеру блока в ветке
    // (в одной ветке время не убывает, blockchain.md §9.4).
    std::vector<const NoteEntry*> about(const std::string& subject) const;

    // Обсуждение (social.md §7): заметки о предмете и о каждом комментарии под
    // ним — реакции на комментарии, снятия. По времени.
    std::vector<const NoteEntry*> thread(const std::string& subject) const;

    // Заметки цепи, по времени; kind пустой — всех видов.
    std::vector<const NoteEntry*> by(const UserId& author, const std::string& kind = {}) const;
};

// Один проход по хранилищу.
NotesBoard read_notes(const AggregatorStorage& storage);

// То же, посчитанное один раз на состояние хранилища: блоки только
// добавляются, и пока их число стоит, стоит и индекс (как стол сделок,
// records.md §13).
std::shared_ptr<const NotesBoard> notes_board(const AggregatorStorage& storage);

} // namespace aggregator

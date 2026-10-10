#pragma once

#include "types.h"
#include "storage.h"
#include <vector>

namespace blockchain {

// Self-verifying attribution of a signing key to a chain (blockchain.md §7.5):
// the node path from the root down to the node whose working key signed a seal
// (blockchain.md §7). Built by the signer, carried by the aggregator without
// inspection (sync.md §7.5), checked by the reader on its own — nobody has to
// be trusted for the answer "this key belongs to that chain".
struct KeyPath {
    std::vector<Node> path;   // nodes root → signer, inclusive
    // Звенья смены рабочего ключа ветки (blockchain.md §6.6), по порядку:
    // каждый блок подписан ПРЕЖНИМ ключом и называет новый. Без них путь
    // заканчивается первоначальным ключом узла, и подпись ветки, сменившей
    // ключ, к цепи не привести (ограничение снято 2026-10-10).
    std::vector<Block> rotations;
};

// Shared link check for node paths (KeyPathCert and RevocationCert):
// the path starts at a self-signed root (index 0, zero parent_hash), every next
// node is a child of the previous one, parent_hash and parent signatures link
// up. Returns the chain identity (root structural key).
// Throws: SignatureError, ChainIntegrityError.
UserId verify_node_path(const std::vector<Node>& path);

class KeyPathCert {
public:
    KeyPathCert() = delete;

    // Assemble the path for a node present in local storage.
    // Throws: NodeNotFoundError.
    static KeyPath build(const IStorage& storage, const UserId& chain, NodeIndex node);

    // Autonomous verification — no storage access. Returns the chain identity.
    // Проверяет и звенья смены ключа: каждое — блок типа KEY_ROTATION в ветке
    // того же узла, подписанный ПРЕЖНИМ ключом, с новым ключом в payload, и
    // блоки идут по возрастанию индекса. Чего проверка не доказывает: что
    // ветка не сменила ключ ПОСЛЕ последнего приложенного звена — такая ветка
    // публикует путь заново.
    // Throws: SignatureError, ChainIntegrityError.
    static UserId verify(const KeyPath& kp);

    // Ключ, которым ветка подписывает СЕЙЧАС: рабочий ключ последнего узла,
    // проведённый через все звенья смены.
    // Throws: ChainIntegrityError (empty path).
    static PublicKey signer_key(const KeyPath& kp);
};

} // namespace blockchain

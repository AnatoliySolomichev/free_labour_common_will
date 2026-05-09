# API модуля блокчейна

**Статус:** черновик v0.1  
**Основание:** [docs/blockchain.md](blockchain.md)  
**Язык реализации:** C++17  
**Внешние зависимости:** libsodium, LMDB (CBOR реализован вручную, без внешней библиотеки)

---

## 1. Обзор архитектуры

Модуль состоит из шести слоёв:

```
┌─────────────────────────────────┐
│         Blockchain              │  ← главный фасад
├──────────────┬──────────────────┤
│ MergeSession │   SealManager    │  ← протоколы
├──────────────┴──────────────────┤
│           Validator             │  ← проверка инвариантов
├─────────────────────────────────┤
│     IStorage / LmdbStorage      │  ← персистентность
├──────────────┬──────────────────┤
│    Crypto    │   Serializer     │  ← примитивы
└──────────────┴──────────────────┘
```

Все публичные классы находятся в пространстве имён `blockchain`.

---

## 2. Пространства имён

```
blockchain          — все публичные типы и классы
blockchain::detail  — вспомогательные типы (не часть публичного API)
```

---

## 3. Примитивные типы

```cpp
namespace blockchain {

// Ed25519 публичный ключ (32 байта)
struct PublicKey {
    std::array<uint8_t, 32> bytes;

    bool operator==(const PublicKey&) const noexcept;
    bool operator< (const PublicKey&) const noexcept;  // для использования в std::map
};

// Ed25519 приватный ключ (64 байта: seed || pubkey, формат libsodium)
struct SecretKey {
    std::array<uint8_t, 64> bytes;
};

// Пара ключей
struct KeyPair {
    PublicKey pub;
    SecretKey sec;
};

// BLAKE2b-256 хеш (32 байта)
struct Hash {
    std::array<uint8_t, 32> bytes;

    bool operator==(const Hash&) const noexcept;
    static Hash zero() noexcept;  // нулевой хеш (для корневого узла)
};

// Ed25519 подпись (64 байта)
struct Signature {
    std::array<uint8_t, 64> bytes;

    bool operator==(const Signature&) const noexcept;
    static Signature null() noexcept;  // нулевая подпись (заглушка до получения co-sign)
};

// Идентификатор пользователя = публичный ключ корневого узла
using UserId = PublicKey;

// Heap-индекс узла в дереве (0 = корень; max = 2^32 − 2)
using NodeIndex = uint32_t;

// Порядковый индекс блока в ветке (0, 1, 2, ...)
using BlockIndex = uint32_t;

// Unix-время в секундах (UTC)
using Timestamp = int64_t;

} // namespace blockchain
```

---

## 4. Перечисления

```cpp
namespace blockchain {

enum class BlockType : uint8_t {
    DATA,          // пользовательский блок с произвольными данными
    MERGE,         // блок слияния двух веток (§6.4)
    KEY_ROTATION,  // ротация рабочего ключа (§6.6)
    REVOCATION,    // отзыв скомпрометированного узла (§6.7)
};

// Тип пломбы (§7.1)
enum class SealMode : uint8_t {
    BLIND,   // подписан только хеш блока (содержимое не раскрывалось)
    OPEN,    // подписаны и хеш, и содержимое
};

} // namespace blockchain
```

---

## 5. Структуры данных

### 5.1. Адрес блока

```cpp
struct BlockAddress {
    UserId     user_id;      // публичный ключ корневого узла владельца
    NodeIndex  node_index;   // heap-индекс листового узла ветки
    BlockIndex block_index;  // порядковый индекс блока в ветке

    bool operator==(const BlockAddress&) const noexcept;
};
```

### 5.2. Внешняя ссылка

Используется в поле `external_refs` блока для ссылки на блоки других пользователей.

```cpp
struct ExternalRef {
    BlockAddress address;
    Hash         block_hash;
};
```

### 5.3. Узел дерева

Служебная сущность; пользовательских данных не содержит (§3.3).

```cpp
struct Node {
    NodeIndex  index;               // heap-индекс в дереве
    PublicKey  structural_pubkey;   // структурный ключ (в MVP = working_pubkey)
    PublicKey  working_pubkey;      // рабочий ключ ветки

    Hash       parent_hash;         // хеш родительского узла (Hash::zero() для корня)
    Signature  parent_sig;          // подпись родительским ключом (самоподпись для корня)
    Timestamp  created_at;          // UTC, секунды

    // Вычисляемые свойства (не хранятся отдельно)
    uint32_t  depth()             const noexcept;  // ⌊log₂(index + 1)⌋
    bool      is_root()           const noexcept;  // index == 0
    bool      is_leaf()           const noexcept;  // depth() == 31
    bool      is_left_child()     const noexcept;
    NodeIndex parent_index()      const noexcept;  // (index − 1) / 2
    NodeIndex left_child_index()  const noexcept;  // 2*index + 1
    NodeIndex right_child_index() const noexcept;  // 2*index + 2
};
```

### 5.4. Блок ветки

```cpp
struct Block {
    BlockAddress             address;
    Hash                     prev_hash;          // хеш пред. блока или хеш листового узла (для index 0)
    Timestamp                timestamp_claimed;  // время, заявленное автором
    BlockType                type;
    std::vector<uint8_t>     payload;            // CBOR-payload (структура зависит от type)
    std::vector<ExternalRef> external_refs;      // опциональные ссылки на чужие блоки

    Signature  signature;                        // подпись рабочим ключом ветки
    // Для MERGE-блоков — дополнительная co-подпись партнёра.
    // Подписывается hash(block без co_signature), а не hash(block целиком).
    std::optional<Signature> co_signature;
};
```

**Примечание по co_signature:** поле отсутствует в CBOR-сериализации canonical блока.
Хеш блока вычисляется по CBOR без co_signature, что позволяет партнёру подписать хеш
до того, как co_signature известен. co_signature хранится как расширение рядом с блоком.

### 5.5. Payload MERGE-блока

Кодируется в CBOR и помещается в `Block::payload` (type == MERGE).

```cpp
struct MergePayload {
    BlockAddress partner_last_address;  // адрес последнего блока партнёра
    Hash         partner_last_hash;     // хеш последнего блока партнёра
    Timestamp    merge_timestamp;       // UTC, согласованное время слияния
};
```

### 5.6. Payload KEY_ROTATION-блока

```cpp
struct KeyRotationPayload {
    PublicKey new_working_pubkey;  // новый рабочий ключ, вступает в силу
                                   // начиная со следующего блока
};
```

### 5.7. Payload REVOCATION-блока

```cpp
struct RevocationPayload {
    NodeIndex revoked_node_index;    // отзываемый узел
    Timestamp compromised_since;    // с какого момента считать блоки недействительными
    PublicKey replacement_pubkey;   // новый публичный ключ (для информирования)
    // [OPEN §11.1] размещение этого блока в дереве — открытый вопрос
};
```

### 5.8. Пломба

```cpp
struct Seal {
    UserId    signer_id;    // идентификатор подписывающего
    Hash      block_hash;   // хеш заверяемого блока
    Signature signature;    // подпись block_hash ключом signer_id
    SealMode  mode;
    Timestamp sealed_at;   // UTC, время создания пломбы
};
```

### 5.9. Информация о верхушке ветки

Передаётся при инициации слияния (§6.4, шаг 1).

```cpp
struct BranchTipInfo {
    BlockAddress      tip_address;  // адрес последнего блока (или NodeIndex с block_index=−1 если ветка пуста)
    Hash              tip_hash;     // hash(последнего блока) или hash(листового узла) если ветка пуста
    std::vector<Node> path;         // узлы от корня до листа включительно (для верификации)
};
```

---

## 6. Иерархия исключений

```cpp
namespace blockchain {

// Базовый класс всех исключений модуля
class BlockchainError : public std::runtime_error {
public:
    explicit BlockchainError(std::string_view msg);
};

// Ошибка криптографической операции (подпись, верификация, генерация ключей)
class CryptoError : public BlockchainError { /* ... */ };

// Нарушение одного из инвариантов (§9)
class InvariantError : public BlockchainError {
public:
    // Номер нарушенного инварианта (1–7)
    int invariant_number() const noexcept;
};

// Некорректная подпись
class SignatureError : public InvariantError { /* ... */ };

// Нарушение целостности цепи (prev_hash не совпадает)
class ChainIntegrityError : public InvariantError { /* ... */ };

// Нарушение монотонности времени
class TimestampError : public InvariantError { /* ... */ };

// Запрашиваемый узел не найден в хранилище
class NodeNotFoundError : public BlockchainError {
public:
    NodeIndex missing_index() const noexcept;
};

// Запрашиваемый блок не найден в хранилище
class BlockNotFoundError : public BlockchainError {
public:
    BlockAddress missing_address() const noexcept;
};

// Ошибка операции с хранилищем (I/O, LMDB)
class StorageError : public BlockchainError { /* ... */ };

// Ошибка сериализации / десериализации CBOR
class SerializationError : public BlockchainError { /* ... */ };

// Некорректный аргумент (например, is_leaf() == false для листовой операции)
class InvalidArgumentError : public BlockchainError { /* ... */ };

} // namespace blockchain
```

---

## 7. `Crypto` — криптографические примитивы

Обёртка над libsodium. Все методы — статические. Не содержит состояния.

```cpp
class Crypto {
public:
    Crypto() = delete;

    // Генерация новой пары Ed25519-ключей
    // Бросает: CryptoError
    static KeyPair generate_keypair();

    // BLAKE2b-256 хеш произвольных данных
    static Hash hash(std::span<const uint8_t> data) noexcept;

    // Ed25519 подпись данных приватным ключом
    // Бросает: CryptoError
    static Signature sign(std::span<const uint8_t> data, const SecretKey& key);

    // Верификация Ed25519 подписи
    // Возвращает false при невалидной подписи (не бросает)
    static bool verify(std::span<const uint8_t> data,
                       const Signature& sig,
                       const PublicKey& key) noexcept;

    // Хеш узла (по канонической CBOR-сериализации без полей, зависящих от родителя)
    // Используется как parent_hash в дочерних узлах
    static Hash hash_node(const Node& node);

    // Хеш блока (по канонической CBOR-сериализации без поля co_signature)
    // Используется как prev_hash, для пломб и co-подписи при слиянии
    static Hash hash_block(const Block& block);
};
```

---

## 8. `Serializer` — CBOR сериализация

Детерминированная кодировка согласно RFC 8949 §4.2.1. Используется внутри `Crypto::hash_*`
и `Storage`. Часть публичного API — для обмена данными по сети при синхронизации.

```cpp
class Serializer {
public:
    Serializer() = delete;

    // Сериализация в CBOR
    // Бросает: SerializationError
    static std::vector<uint8_t> encode(const Node& node);
    static std::vector<uint8_t> encode(const Block& block);
    static std::vector<uint8_t> encode(const Seal& seal);
    static std::vector<uint8_t> encode(const BranchTipInfo& tip);

    // Десериализация из CBOR
    // Бросает: SerializationError при неверном формате
    static Node          decode_node (std::span<const uint8_t> data);
    static Block         decode_block(std::span<const uint8_t> data);
    static Seal          decode_seal (std::span<const uint8_t> data);
    static BranchTipInfo decode_tip  (std::span<const uint8_t> data);
};
```

---

## 9. `IStorage` / `LmdbStorage` — хранилище

### 9.1. Интерфейс `IStorage`

Абстракция, позволяющая подменять реализацию (в первую очередь для тестов).

```cpp
class IStorage {
public:
    virtual ~IStorage() = default;

    // --- Узлы ---

    // Сохранить узел. Если узел с таким индексом уже существует — перезапись запрещена.
    // Бросает: StorageError, InvalidArgumentError (дубликат)
    virtual void put_node(const UserId& user_id, const Node& node) = 0;

    // Получить узел. Бросает: NodeNotFoundError
    virtual Node get_node(const UserId& user_id, NodeIndex index) const = 0;

    // Проверить наличие узла (не бросает)
    virtual bool has_node(const UserId& user_id, NodeIndex index) const noexcept = 0;

    // --- Блоки ---

    // Сохранить блок. Перезапись запрещена.
    // Бросает: StorageError, InvalidArgumentError (дубликат)
    virtual void put_block(const Block& block) = 0;

    // Получить блок. Бросает: BlockNotFoundError
    virtual Block get_block(const BlockAddress& address) const = 0;

    virtual bool has_block(const BlockAddress& address) const noexcept = 0;

    // Индекс последнего блока ветки. Возвращает std::nullopt если ветка пуста.
    virtual std::optional<BlockIndex> branch_tip_index(
        const UserId& user_id, NodeIndex leaf_index) const noexcept = 0;

    // --- Пломбы ---

    // Добавить пломбу. Несколько пломб на один блок допустимы.
    // Бросает: StorageError
    virtual void put_seal(const Seal& seal) = 0;

    // Все пломбы для заданного хеша блока
    virtual std::vector<Seal> get_seals(const Hash& block_hash) const = 0;

    // --- Внешние блоки (чужих пользователей) ---

    virtual void  put_external_block(const Block& block) = 0;
    virtual Block get_external_block(const BlockAddress& address) const = 0;
    virtual bool  has_external_block(const BlockAddress& address) const noexcept = 0;

    // --- Транзакции ---

    // RAII-транзакция. Фиксирует при commit(), откатывает при уничтожении без commit().
    class Transaction {
    public:
        virtual ~Transaction() = default;
        virtual void commit() = 0;
    };

    // Начать транзакцию записи (только один одновременный writer — требование LMDB)
    // Бросает: StorageError
    virtual std::unique_ptr<Transaction> begin_write() = 0;
};
```

### 9.2. `LmdbStorage`

Конкретная реализация поверх LMDB.

```cpp
class LmdbStorage : public IStorage {
public:
    // Открыть или создать хранилище по пути к директории.
    // Бросает: StorageError
    explicit LmdbStorage(std::filesystem::path db_path);

    ~LmdbStorage() override;

    // Некопируем, перемещаем
    LmdbStorage(const LmdbStorage&) = delete;
    LmdbStorage& operator=(const LmdbStorage&) = delete;
    LmdbStorage(LmdbStorage&&) noexcept;
    LmdbStorage& operator=(LmdbStorage&&) noexcept;

    // Реализует все методы IStorage
    // ...
};
```

**Структура таблиц LMDB:**

| Таблица | Ключ | Значение |
|---|---|---|
| `nodes` | `(user_id, node_index)` — CBOR | сериализованный `Node` — CBOR |
| `blocks` | `(user_id, node_index, block_index)` — CBOR | сериализованный `Block` — CBOR |
| `seals` | `block_hash` (32 байта) | список сериализованных `Seal` — CBOR |
| `external_blocks` | `(user_id, node_index, block_index)` — CBOR | сериализованный `Block` — CBOR |

---

## 10. `Validator` — проверка инвариантов

Все методы читают только из переданного хранилища; не изменяют состояния.

```cpp
class Validator {
public:
    explicit Validator(const IStorage& storage);

    // --- Проверка узлов (инварианты 1, 3, 6) ---

    // Проверить подпись узла родительским ключом.
    // Для корневого узла (index == 0) проверяет самоподпись.
    // Бросает: SignatureError, NodeNotFoundError (если родитель не найден)
    void validate_node(const Node& node, const UserId& user_id) const;

    // Проверить полный путь от корня до узла leaf_index.
    // Читает все узлы пути из хранилища.
    // Бросает: SignatureError, NodeNotFoundError, InvariantError
    void validate_path(const UserId& user_id, NodeIndex leaf_index) const;

    // --- Проверка ветки (инварианты 2, 3, 4, 5, 7) ---

    // Проверить один блок:
    //   - prev_hash соответствует предыдущему блоку (или хешу листового узла для index 0);
    //   - подпись блока соответствует рабочему ключу (с учётом KEY_ROTATION);
    //   - timestamp_claimed >= предыдущего.
    // Бросает: SignatureError, ChainIntegrityError, TimestampError
    void validate_block(const Block& block,
                        const Hash& expected_prev_hash,
                        const PublicKey& expected_working_pubkey) const;

    // Проверить всю ветку от блока 0 до конца.
    // Читает блоки из хранилища; отслеживает KEY_ROTATION.
    // Бросает: SignatureError, ChainIntegrityError, TimestampError,
    //          BlockNotFoundError, NodeNotFoundError
    void validate_branch(const UserId& user_id, NodeIndex leaf_index) const;

    // --- Проверка пломбы (инвариант 3) ---

    // Бросает: SignatureError
    void validate_seal(const Seal& seal) const;

    // --- Проверка co-подписи MERGE-блока ---

    // Проверяет, что co_signature в block действительно является подписью
    // partner_pubkey на Crypto::hash_block(block).
    // Бросает: SignatureError, InvalidArgumentError (если block.type != MERGE)
    void validate_co_signature(const Block& block, const PublicKey& partner_pubkey) const;
};
```

---

## 11. `SealManager` — управление пломбами

```cpp
class SealManager {
public:
    explicit SealManager(IStorage& storage);

    // Создать пломбу: подписать block_hash ключом signer_keypair.
    // Сохраняет в хранилище.
    // mode == BLIND: подписывается только block_hash.
    // mode == OPEN:  перед подписью верифицируется block_hash == Crypto::hash_block(block).
    // Бросает: CryptoError, StorageError, InvalidArgumentError (несовпадение хеша при OPEN)
    Seal create_seal(const Hash& block_hash,
                     const KeyPair& signer_keypair,
                     SealMode mode);

    Seal create_open_seal(const Block& block, const KeyPair& signer_keypair);

    // Получить все пломбы для блока
    std::vector<Seal> get_seals(const Hash& block_hash) const;

    // Вычислить witnessed_time: самую позднюю верхнюю границу времени существования блока.
    // Анализирует граф MERGE-блоков: если блок B ссылается на наш блок A,
    // то A точно существовал не позже B.timestamp_claimed.
    // Возвращает std::nullopt, если внешних свидетелей нет.
    // [OPEN §11.3] распространение знания об отзывах при gossip не реализовано в MVP
    std::optional<Timestamp> compute_witnessed_time(const BlockAddress& address) const;
};
```

---

## 12. `MergeSession` — протокол двустороннего слияния

Инкапсулирует состояние двухраундового протокола (§6.4).

```cpp
// Промежуточное состояние после первого раунда
struct PendingMergeBlock {
    Block draft;       // block с signature (своей), co_signature == nullopt
    Hash  draft_hash;  // Crypto::hash_block(draft) — отправляется партнёру для co-sign
};

class MergeSession {
public:
    MergeSession(IStorage& storage, Validator& validator);

    // Шаг 1а: подготовить BranchTipInfo своей ветки для отправки партнёру.
    // Бросает: NodeNotFoundError, BlockNotFoundError
    BranchTipInfo prepare_tip(const UserId& user_id, NodeIndex leaf_index) const;

    // Шаг 1б: верифицировать полученный BranchTipInfo партнёра (путь + подписи).
    // Бросает: SignatureError, ChainIntegrityError, NodeNotFoundError
    void verify_partner_tip(const BranchTipInfo& partner_tip) const;

    // Шаг 2: создать черновик MERGE-блока (своя подпись; co_signature пуста).
    // Добавляет черновик в хранилище как финальный блок ветки (см. примечание ниже).
    // Возвращает pending для отправки draft_hash партнёру.
    // Бросает: CryptoError, StorageError, NodeNotFoundError
    //
    // Примечание: блок сохраняется сразу, но без co_signature (поле == nullopt).
    // После получения co-подписи партнёра вызвать finalize().
    PendingMergeBlock create_pending(
        const UserId&       user_id,
        NodeIndex           leaf_index,
        const BranchTipInfo& partner_tip,
        const KeyPair&      own_working_keypair,
        Timestamp           merge_timestamp
    );

    // Шаг 3: co-подписать черновик MERGE-блока партнёра.
    // Возвращает Signature, которую надо передать партнёру.
    // Бросает: CryptoError
    Signature co_sign(const Hash& partner_draft_hash, const KeyPair& own_working_keypair);

    // Шаг 4: добавить co_signature партнёра к своему pending-блоку и сохранить.
    // После этого блок считается финализированным.
    // Бросает: SignatureError (если co_sig невалидна), StorageError
    Block finalize(const PendingMergeBlock& pending,
                   const Signature&         partner_co_sig,
                   const PublicKey&         partner_pubkey);
};
```

**Типичный порядок вызовов:**

```
Alice                               Bob
prepare_tip() ─────────────────►  verify_partner_tip()
verify_partner_tip()  ◄─────────  prepare_tip()
create_pending() ──── draft_hash ►  co_sign()
co_sign()        ◄── draft_hash ──  create_pending()
finalize() ◄──────── co_sig ──────  finalize()
           ─────────── co_sig ────►
```

---

## 13. `Blockchain` — основной фасад

Точка входа для всех операций прикладного кода.

```cpp
class Blockchain {
public:
    // storage и validator должны жить не меньше Blockchain
    Blockchain(IStorage& storage, Validator& validator);

    // ─── Идентичность (§6.1) ──────────────────────────────────────────────

    // Создать корневой узел (index 0) с самоподписью.
    // Сохраняет узел в хранилище.
    // Бросает: CryptoError, StorageError, InvalidArgumentError (уже существует)
    Node create_identity(const KeyPair& root_keypair);

    // ─── Дерево узлов (§6.2) ──────────────────────────────────────────────

    // Убедиться, что все узлы от корня до leaf_index существуют.
    // Для отсутствующих узлов вызывает key_for(node_index) → KeyPair, создаёт и сохраняет узел.
    // Возвращает только вновь созданные узлы в порядке корень→лист.
    // Бросает: CryptoError, StorageError, NodeNotFoundError (корень не найден),
    //          InvalidArgumentError (leaf_index не является листом, т.е. depth != 31)
    std::vector<Node> ensure_path(
        const UserId&                          user_id,
        NodeIndex                              leaf_index,
        std::function<KeyPair(NodeIndex)>      key_for
    );

    // Получить узел. Бросает: NodeNotFoundError
    Node get_node(const UserId& user_id, NodeIndex index) const;

    // Получить путь (список узлов) от корня до leaf_index из хранилища.
    // Бросает: NodeNotFoundError (если хотя бы один узел пути отсутствует)
    std::vector<Node> get_path(const UserId& user_id, NodeIndex leaf_index) const;

    // ─── Операции с веткой ────────────────────────────────────────────────

    // Хеш верхушки ветки: hash(последнего блока) или hash(листового узла) если ветка пуста.
    // Бросает: NodeNotFoundError
    Hash branch_tip_hash(const UserId& user_id, NodeIndex leaf_index) const;

    // Добавить DATA-блок в ветку (§6.3).
    // Определяет prev_hash автоматически через branch_tip_hash().
    // Подписывает рабочим ключом; отслеживает KEY_ROTATION в ветке.
    // Бросает: CryptoError, StorageError, NodeNotFoundError,
    //          InvalidArgumentError (leaf_index не листовой)
    Block append_data_block(
        const UserId&           user_id,
        NodeIndex               leaf_index,
        std::vector<uint8_t>    payload,
        const KeyPair&          working_keypair,
        Timestamp               timestamp
    );

    // Ротация рабочего ключа (§6.6): добавляет KEY_ROTATION-блок.
    // Подписывается old_working_keypair; все следующие блоки должны использовать new_keypair.
    // Бросает: CryptoError, StorageError, NodeNotFoundError
    Block rotate_key(
        const UserId&  user_id,
        NodeIndex      leaf_index,
        const KeyPair& old_working_keypair,
        const KeyPair& new_keypair,
        Timestamp      timestamp
    );

    // Отзыв скомпрометированного узла (§6.7).
    // parent_keypair — ключ родительского узла (node_index−1)/2.
    // [OPEN §11.1] размещение REVOCATION-блока в дереве не определено в MVP;
    // текущая реализация добавляет его в служебную ветку родительского узла.
    // Бросает: CryptoError, StorageError, NodeNotFoundError
    Block revoke_node(
        const UserId&  user_id,
        NodeIndex      revoked_node_index,
        Timestamp      compromised_since,
        const PublicKey& replacement_pubkey,
        const KeyPair& parent_keypair,
        Timestamp      timestamp
    );

    // ─── Чтение ───────────────────────────────────────────────────────────

    // Бросает: BlockNotFoundError
    Block get_block(const BlockAddress& address) const;

    // Все блоки ветки от индекса 0 до верхушки в порядке возрастания.
    // Бросает: NodeNotFoundError, BlockNotFoundError
    std::vector<Block> get_branch(const UserId& user_id, NodeIndex leaf_index) const;

    // ─── Валидация ────────────────────────────────────────────────────────

    // Проверить путь узлов от корня до leaf_index.
    // Делегирует Validator::validate_path().
    // Бросает: SignatureError, NodeNotFoundError
    void validate_path(const UserId& user_id, NodeIndex leaf_index) const;

    // Проверить всю ветку (путь + все блоки).
    // Делегирует Validator::validate_branch().
    // Бросает: SignatureError, ChainIntegrityError, TimestampError,
    //          BlockNotFoundError, NodeNotFoundError
    void validate_branch(const UserId& user_id, NodeIndex leaf_index) const;

private:
    IStorage&  storage_;
    Validator& validator_;
};
```

---

## 14. Вспомогательные свободные функции

```cpp
namespace blockchain {

// Вычислить последовательность heap-индексов узлов от корня до target_index включительно.
// Результат: [0, ..., target_index], длина == depth(target_index) + 1.
std::vector<NodeIndex> path_indices(NodeIndex target_index) noexcept;

// Глубина узла: ⌊log₂(index + 1)⌋
uint32_t node_depth(NodeIndex index) noexcept;

// Является ли узел листовым (depth == 31)
bool is_leaf_node(NodeIndex index) noexcept;

} // namespace blockchain
```

---

## 15. Сценарии использования (псевдокод)

### Создание идентичности и первого блока

```cpp
LmdbStorage storage("/data/my_blockchain");
Validator   validator(storage);
Blockchain  bc(storage, validator);

KeyPair root_kp = Crypto::generate_keypair();
bc.create_identity(root_kp);                     // создать корневой узел

NodeIndex leaf = /* выбранный heap-индекс */;
bc.ensure_path(root_kp.pub, leaf, [](NodeIndex n) {
    return Crypto::generate_keypair();           // генерировать ключ для каждого нового узла
});

bc.append_data_block(root_kp.pub, leaf, payload, working_kp, now());
```

### Слияние двух пользователей

```cpp
MergeSession session_alice(storage_alice, validator_alice);
MergeSession session_bob  (storage_bob,  validator_bob);

// Шаг 1: обмен информацией о верхушках
BranchTipInfo tip_alice = session_alice.prepare_tip(alice_id, leaf_alice);
BranchTipInfo tip_bob   = session_bob.prepare_tip  (bob_id,   leaf_bob);

session_alice.verify_partner_tip(tip_bob);
session_bob.verify_partner_tip  (tip_alice);

// Шаг 2: черновики MERGE-блоков
PendingMergeBlock pending_alice = session_alice.create_pending(
    alice_id, leaf_alice, tip_bob, alice_working_kp, now());
PendingMergeBlock pending_bob   = session_bob.create_pending(
    bob_id, leaf_bob, tip_alice, bob_working_kp, now());

// Шаг 3: обмен co-подписями
Signature co_alice = session_alice.co_sign(pending_bob.draft_hash,   alice_working_kp);
Signature co_bob   = session_bob.co_sign  (pending_alice.draft_hash, bob_working_kp);

// Шаг 4: финализация
Block merge_alice = session_alice.finalize(pending_alice, co_bob,   bob_working_kp.pub);
Block merge_bob   = session_bob.finalize  (pending_bob,   co_alice, alice_working_kp.pub);
```

---

## 16. Открытые вопросы API

Следующие вопросы унаследованы из `blockchain.md §11` и влияют на API:

| # | Вопрос | Влияние на API |
|---|---|---|
| 11.1 | Где размещается REVOCATION-блок | Сигнатура `Blockchain::revoke_node()` может измениться |
| 11.2 | Защита от фальшивых поддеревьев | Возможно добавление `Blockchain::finalize_node()` (свидетельствование) |
| 11.3 | Gossip об отзывах | Возможно добавление `SealManager::invalidate_stale_seals()` |
| 11.4 | Отказоустойчивость группового слияния | `MergeSession` потребует timeout/retry параметры |
| 11.5 | Восстановление мастер-идентичности | Поле `master_key_version` в `Node` зарезервировано |
| 11.7 | Ротация структурного ключа | Не покрывается MVP API; `Node::structural_pubkey` зарезервировано |

---

## 17. История изменений

- **v0.1** — первая версия API. Покрывает MVP-операции: идентичность, дерево узлов, ветки, слияние, ротация ключа, отзыв, валидация, пломбы.

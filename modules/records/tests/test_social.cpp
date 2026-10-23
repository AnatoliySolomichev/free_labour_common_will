#include "records/social.h"

#include <gtest/gtest.h>

using namespace records;

// ── Ключ внешнего объекта: YouTube (social.md §3.2) ──────────────────────────

// Один ролик — один ключ, из какой бы формы ссылки его ни вычисляли.
TEST(ExternalKey, YouTubeVideoFromEveryFormOfLink) {
    for (const char* link : {
             "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
             "https://youtube.com/watch?feature=share&v=dQw4w9WgXcQ&t=42s",
             "http://m.youtube.com/watch?v=dQw4w9WgXcQ&list=PL123&index=2",
             "youtu.be/dQw4w9WgXcQ",
             "https://youtu.be/dQw4w9WgXcQ?si=AbCdEf&t=30",
             "https://www.youtube.com/shorts/dQw4w9WgXcQ",
             "https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ?start=10",
             "https://www.youtube.com/live/dQw4w9WgXcQ?feature=shared",
             "https://music.youtube.com/watch?v=dQw4w9WgXcQ",
             "  HTTPS://WWW.YOUTUBE.COM/watch?v=dQw4w9WgXcQ#comments  ",
         })
        EXPECT_EQ(external_key(link), "yt:dQw4w9WgXcQ") << link;
}

TEST(ExternalKey, YouTubeIdIsCaseSensitive) {
    EXPECT_EQ(external_key("youtu.be/DQW4W9WGXCQ"), "yt:DQW4W9WGXCQ");
}

// Не ролик — канал, плейлист, кривой id: правило url: с единым хостом youtube.com.
TEST(ExternalKey, YouTubePagesThatAreNotVideosGoByUrlRule) {
    EXPECT_EQ(external_key("https://m.youtube.com/@channel?si=xyz"), "url:youtube.com/@channel");
    EXPECT_EQ(external_key("https://www.youtube.com/playlist?list=PL123&si=x"),
              "url:youtube.com/playlist?list=PL123");
    EXPECT_EQ(external_key("https://www.youtube.com/watch?v=short"),
              "url:youtube.com/watch?v=short");
}

// ── Telegram (social.md §3.3) ────────────────────────────────────────────────

TEST(ExternalKey, TelegramPostFromEveryFormOfLink) {
    for (const char* link : {
             "https://t.me/durov/42",
             "t.me/Durov/42?single",
             "https://t.me/s/durov/42",
             "https://telegram.me/DUROV/042?comment=7",
         })
        EXPECT_EQ(external_key(link), "tg:durov/42") << link;
}

TEST(ExternalKey, TelegramPrivateChannelAndChannelItself) {
    EXPECT_EQ(external_key("https://t.me/c/1234567890/15"), "tg:c/1234567890/15");
    EXPECT_EQ(external_key("https://t.me/Durov"), "tg:durov");
}

// Служебные пути и приглашения — не каналы.
TEST(ExternalKey, TelegramServicePathsGoByUrlRule) {
    EXPECT_EQ(external_key("https://t.me/joinchat/AbCd"), "url:t.me/joinchat/AbCd");
    EXPECT_EQ(external_key("https://t.me/+AbCdEf"), "url:t.me/+AbCdEf");
    EXPECT_EQ(external_key("https://telegram.me/addstickers/pack"), "url:t.me/addstickers/pack");
}

// ── Любая другая ссылка (social.md §3.4) ─────────────────────────────────────

TEST(ExternalKey, UrlRuleExampleFromTheSpec) {
    EXPECT_EQ(external_key("https://WWW.Example.com/news/42/?utm_source=tg&b=2&a=1#top"),
              "url:example.com/news/42?a=1&b=2");
}

TEST(ExternalKey, UrlRuleDropsWhatDoesNotChangeTheObject) {
    // схема, www, порт по умолчанию, имя пользователя, завершающий «/», фрагмент
    for (const char* link : {
             "http://example.com/a",
             "https://example.com/a/",
             "https://www.example.com:443/a",
             "http://user:pw@example.com:80/a#x",
             "example.com/a",
             "//example.com/a",
         })
        EXPECT_EQ(external_key(link), "url:example.com/a") << link;
    EXPECT_EQ(external_key("https://example.com/"), "url:example.com");
    EXPECT_EQ(external_key("https://example.com:8080/a"), "url:example.com:8080/a");
}

TEST(ExternalKey, UrlRuleCleansQueryAndPercentEscapes) {
    EXPECT_EQ(external_key("https://shop.example/item?fbclid=1&id=7&gclid=2&UTM_medium=x&ysclid=3"),
              "url:shop.example/item?id=7");
    EXPECT_EQ(external_key("https://example.com/%7euser/%2fa%41"), "url:example.com/~user/%2FaA");
    // одинаковые имена сохраняют порядок между собой
    EXPECT_EQ(external_key("https://example.com/s?b=1&a=2&a=1"), "url:example.com/s?a=2&a=1&b=1");
    // путь различает регистр
    EXPECT_NE(external_key("https://example.com/A"), external_key("https://example.com/a"));
}

TEST(ExternalKey, NotALinkIsNoKey) {
    for (const char* bad : {"", "   ", "ftp://example.com/a", "https://", "two words",
                            "https://exa mple.com/a"})
        EXPECT_FALSE(external_key(bad)) << bad;
}

// ── Готовый ключ (social.md §3.1) ────────────────────────────────────────────

TEST(ExternalKey, KnownKeysAreBroughtToCanonicalForm) {
    EXPECT_EQ(external_key("yt:dQw4w9WgXcQ"), "yt:dQw4w9WgXcQ");
    EXPECT_FALSE(external_key("yt:bad"));
    EXPECT_EQ(external_key("tg:Durov/042"), "tg:durov/42");
    EXPECT_EQ(external_key("url:https://www.Example.com/a/"), "url:example.com/a");
    // url:, который на деле ролик, — тот же ключ, что у ролика
    EXPECT_EQ(external_key("url:youtube.com/watch?v=dQw4w9WgXcQ"), "yt:dQw4w9WgXcQ");
}

// Объект по хешу содержимого (social.md §3.5): 64 hex, строчными.
TEST(ExternalKey, ContentKeyIsSixtyFourHexLowercase) {
    EXPECT_EQ(external_key("b2:" + std::string(64, 'A')), "b2:" + std::string(64, 'a'));
    EXPECT_FALSE(external_key("b2:" + std::string(63, 'a')));
    EXPECT_FALSE(external_key("b2:" + std::string(64, 'g')));
}

// ── Товары (social.md §3.6) ──────────────────────────────────────────────────

// Штрихкод любой длины — к 14 цифрам; контрольная цифра проверяется.
TEST(ExternalKey, BarcodeToFourteenDigitsWithCheckDigit) {
    EXPECT_EQ(external_key("gtin:4006381333931"), "gtin:04006381333931");   // EAN-13
    EXPECT_EQ(external_key("gtin:036000291452"), "gtin:00036000291452");    // UPC-A
    EXPECT_EQ(external_key("gtin:73513537"), "gtin:00000073513537");        // EAN-8
    EXPECT_EQ(external_key("4600000000008"), "gtin:04600000000008");        // голый штрихкод
    EXPECT_FALSE(external_key("gtin:4600000000009"));                       // не та контрольная
    EXPECT_FALSE(external_key("4600000000009"));
    EXPECT_FALSE(external_key("gtin:12345"));
}

TEST(ExternalKey, ModelNameIsFolded) {
    EXPECT_EQ(external_key("model:Apple iPhone 15 Pro (256 GB)"), "model:apple-iphone-15-pro-256-gb");
    EXPECT_EQ(external_key("model:  Xiaomi  Power-Bank 20000 "), "model:xiaomi-power-bank-20000");
    EXPECT_EQ(external_key("model:Табурет «Ёжик»"), "model:табурет-ежик");
    EXPECT_EQ(external_key("model:ТАБУРЕТ ЁЖИК"), external_key("model:табурет ежик"));
    EXPECT_FALSE(external_key("model:  — !"));
}

TEST(ExternalKey, ShopLinksToProductKeys) {
    EXPECT_EQ(external_key("https://www.ozon.ru/product/power-bank-xiaomi-20000-123456789/?sh=x"),
              "ozon:123456789");
    EXPECT_EQ(external_key("ozon.ru/product/123456789/"), "ozon:123456789");
    EXPECT_EQ(external_key("https://www.wildberries.ru/catalog/654321/detail.aspx?targetUrl=GP"),
              "wb:654321");
    EXPECT_EQ(external_key("https://market.yandex.ru/product--power-bank/1779337045?sku=1"),
              "ymarket:1779337045");
    EXPECT_EQ(external_key("https://market.yandex.ru/card/power-bank/103276459821"),
              "ymarket:card/103276459821");
    EXPECT_EQ(external_key("https://www.amazon.com/Anker-PowerCore/dp/b0194wdvhi/ref=sr_1_3"),
              "amazon:B0194WDVHI");
    EXPECT_EQ(external_key("https://www.amazon.de/gp/product/B0194WDVHI"), "amazon:B0194WDVHI");
    EXPECT_EQ(external_key("https://www.amazon.co.uk/gp/aw/d/B0194WDVHI"), "amazon:B0194WDVHI");
    EXPECT_EQ(external_key("https://aliexpress.ru/item/1005001234567890.html?spm=a2g0o"),
              "ali:1005001234567890");
    EXPECT_EQ(external_key("https://m.aliexpress.com/item/1005001234567890.html"),
              "ali:1005001234567890");
    // не страница товара — общим правилом
    EXPECT_EQ(external_key("https://www.ozon.ru/category/powerbanks/"), "url:ozon.ru/category/powerbanks");
}

TEST(ExternalKey, ProductKeysGivenAsKeysAreChecked) {
    EXPECT_EQ(external_key("ozon:123456789"), "ozon:123456789");
    EXPECT_FALSE(external_key("ozon:12a"));
    EXPECT_EQ(external_key("amazon:b0194wdvhi"), "amazon:B0194WDVHI");
    EXPECT_FALSE(external_key("amazon:B0194"));
    EXPECT_EQ(external_key("ymarket:card/103276459821"), "ymarket:card/103276459821");
}

// Незнакомые пространства — законны: следующие сценарии добавят свои без правки кода.
TEST(ExternalKey, UnknownNamespacesAreTakenAsTheyAre) {
    EXPECT_EQ(external_key("isbn:9785171182606"), "isbn:9785171182606");
    EXPECT_FALSE(external_key("isbn:978 5"));
}

TEST(ExternalKey, KeyFormIsChecked) {
    EXPECT_TRUE(is_external_key("yt:dQw4w9WgXcQ"));
    EXPECT_TRUE(is_external_key("url:example.com/a?b=c"));
    EXPECT_TRUE(is_external_key("isbn9:978-5"));
    for (const char* bad : {"", "yt", "yt:", ":x", "YT:x", "yt:a b", "https://x", "http:x",
                            "a1234567890123456:x", "t.me:x", "url://x"})
        EXPECT_FALSE(is_external_key(bad)) << bad;
}

// ── Предмет строкой (social.md §7) ───────────────────────────────────────────

TEST(Subject, IdRoundTripForAllThreeKinds) {
    Ref r{};
    r.chain.fill(0xab);
    r.hash.fill(0x01);
    ChainId c{};
    c.fill(0x0f);
    for (const Subject& s : {Subject{r}, Subject{std::string("tg:durov/42")}, Subject{c}}) {
        const auto id   = subject_id(s);
        const auto back = parse_subject(id);
        ASSERT_TRUE(back) << id;
        EXPECT_EQ(subject_id(*back), id);
        EXPECT_EQ(back->index(), s.index());
    }
    EXPECT_EQ(subject_id(Subject{r}),   // hex строчными, «ЦЕПЬ/ХЕШ»
              "abababababababababababababababababababababababababababababababab/"
              "0101010101010101010101010101010101010101010101010101010101010101");
}

TEST(Subject, ParseAcceptsUpperHexAndRejectsJunk) {
    EXPECT_TRUE(parse_subject(std::string(64, 'A')));
    for (const char* bad : {"", "abc", "yt:", "zz", "https://x"})
        EXPECT_FALSE(parse_subject(bad)) << bad;
    EXPECT_FALSE(parse_subject(std::string(64, 'a') + "/" + std::string(63, 'b')));
    EXPECT_FALSE(parse_subject(std::string(64, 'g')));
}

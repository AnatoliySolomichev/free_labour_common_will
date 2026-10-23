#!/usr/bin/env python3
"""Хост native messaging расширения (extension.md §3): кадры, белый список, ответы.

Говорит с `bc native-host` так же, как браузер: 4 байта длины в порядке байтов
машины и JSON. Проверяет чтение картин, запись заметок в ветку браузера и
отказ во всём, чего нет в белом списке.

Запуск: host_test.py BC КАТАЛОГ_ДАННЫХ URL_АГРЕГАТОРА
"""
import json
import struct
import subprocess
import sys

VIDEO = "https://youtu.be/dQw4w9WgXcQ?t=30"
SHOP = "https://www.ozon.ru/product/power-bank-xiaomi-20000-123456789/"
BROWSER_LEAF = 2147483648

failed = 0


def check(what, fact, want):
    global failed
    if fact == want:
        print(f"   ✓ {what}")
    else:
        print(f"   ✗ {what}: получили «{fact}», ждали «{want}»")
        failed = 1


def main():
    bc, data, via = sys.argv[1:4]
    host = subprocess.Popen(
        [bc, "--data-dir", data, "native-host", "--via", via, "chrome-extension://test/"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    next_id = iter(range(1, 1000))

    def frame(body):
        host.stdin.write(struct.pack("=I", len(body)) + body)
        host.stdin.flush()
        size = struct.unpack("=I", host.stdout.read(4))[0]
        return json.loads(host.stdout.read(size))

    def ask(cmd, **fields):
        return frame(json.dumps({"id": next(next_id), "cmd": cmd, **fields}).encode())

    me = ask("whoami")
    check("кто я — цепь и ветка браузера", (me["ok"], len(me["data"]["chain"]), me["data"]["leaf"]),
          (True, 64, BROWSER_LEAF))
    check("ключ страницы", ask("key", subject=VIDEO)["data"]["key"], "yt:dQw4w9WgXcQ")
    check("лайк", ask("like", subject=VIDEO)["ok"], True)
    check("комментарий", ask("comment", subject=VIDEO, text="из расширения \"с кавычками\"")["ok"], True)
    picture = ask("show", subject=VIDEO)
    check("картина: свой лайк", picture["data"]["reactions"]["me"], {"plus": 1, "minus": 0})
    check("картина: свой комментарий", picture["data"]["comments"][0]["text"], "из расширения \"с кавычками\"")
    check("отзыв о товаре", ask("review", subject=SHOP, stars=4, text="держит заряд")["ok"], True)
    reviews = ask("reviews", subject=SHOP)
    check("картина товара", (reviews["data"]["product"], reviews["data"]["reviews"][0]["stars"]),
          ("ozon:123456789", 4))
    denied = ask("transfer", subject="x")
    check("перевод расширению нельзя", (denied["ok"], "нельзя" in denied["error"]), (False, True))
    check("не JSON — отказ, хост жив", frame(b"{oops")["ok"], False)
    check("после отказа хост отвечает", ask("key", subject="t.me/durov/42")["data"]["key"], "tg:durov/42")

    host.stdin.close()
    check("хост закончил, когда браузер закрыл канал", host.wait(timeout=10), 0)

    listed = subprocess.run([bc, "--data-dir", data, "list", "--leaf", str(BROWSER_LEAF)],
                            capture_output=True, text=True).stdout
    check("заметки легли в ветку браузера", listed.count("[Note]"), 3)
    return failed


if __name__ == "__main__":
    sys.exit(main())

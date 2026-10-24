#!/usr/bin/env python3
"""Расширение bc в настоящем Firefox (extension.md §6): то же окно, что в Chromium.

bc.xpi ставится так, как его ставит участник (extension.md §5): насовсем и без
подписи — в Firefox ESR это разрешает xpinstall.signatures.required = false.
Дальше окно на странице ролика и товара: лайк, комментарий, отзыв. И слова окна,
когда с хостом что-то не так: у Firefox они не такие, как у Chromium.

Firefox ведёт geckodriver по протоколу WebDriver — запросами urllib, без
библиотек: Playwright расширения в Firefox не грузит.

Запуск (обычно из host-e2e.sh): firefox_test.py BC.XPI ДОМ BC КАТАЛОГ_ДАННЫХ

ДОМ — HOME для Firefox: манифест хоста он ищет только в ДОМ/.mozilla/native-messaging-hosts,
куда его кладёт HOME=ДОМ bc native-install --browser firefox.
"""
import base64
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ADDON_ID = "bc@free-labour.org"                    # browser_specific_settings в manifest.json
UUID = "0b5c0e1a-6f4d-4c2b-9e7a-bc0000000001"      # moz-extension://UUID — задаём сами
ELEMENT = "element-6066-11e4-a52e-4f735466cecf"    # ключ ссылки на элемент в ответах WebDriver
BROWSER_LEAF = "2147483648"
VIDEO = "https://youtu.be/jNQXAC9IVRw"
SHOP = "https://www.wildberries.ru/catalog/777777/detail.aspx"
COMMENT = "из Firefox <b>не жирный</b>"

failed = 0


def check(what, fact, want):
    global failed
    if fact == want:
        print(f"   ✓ {what}")
    else:
        print(f"   ✗ {what}: получили «{fact}», ждали «{want}»")
        failed = 1


class Firefox:
    """Firefox под geckodriver: сеанс WebDriver и то немногое, что нужно окну."""

    def __init__(self, home):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        # Без --allow-system-access geckodriver не открывает страницы расширений.
        # Дом — у самого geckodriver: env в настройках сеанса до поиска манифеста не доходит.
        self.driver = subprocess.Popen(["geckodriver", "--port", str(port), "--allow-system-access"],
                                       env={**os.environ, "HOME": home},
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.root = f"http://127.0.0.1:{port}"
        for _ in range(100):
            try:
                self.call("GET", "/status")
                break
            except OSError:
                time.sleep(0.1)
        options = {
            "args": ["-headless"],
            "prefs": {"extensions.webextensions.uuids": json.dumps({ADDON_ID: UUID}),
                      "xpinstall.signatures.required": False},   # как в about:config у участника
        }
        caps = {"browserName": "firefox", "moz:firefoxOptions": options}
        self.session = "/session/" + self.call("POST", "/session", {"capabilities": {"alwaysMatch": caps}})["sessionId"]

    def call(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.root + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=60) as reply:
                return json.load(reply)["value"]
        except urllib.error.HTTPError as e:
            raise RuntimeError(json.load(e)["value"]["message"]) from None

    def ask(self, method, path, body=None):
        return self.call(method, self.session + path, body)

    def install(self, xpi):
        return self.ask("POST", "/moz/addon/install", {"path": os.path.abspath(xpi), "temporary": False})

    def popup(self, url):
        self.ask("POST", "/url", {"url": f"moz-extension://{UUID}/popup.html?url={urllib.parse.quote(url, safe='')}"})

    def open(self, url, section):
        """Окно для страницы url; ждём раздел section, а вместо него — ошибку окна."""
        self.popup(url)
        shown = self.find(f"{section}:not([hidden]), #error:not([hidden])")
        if self.ask("GET", f"/element/{shown}/property/id") == "error":
            raise RuntimeError("окно: " + self.text("#error"))

    def find(self, css, timeout=20):
        end = time.time() + timeout
        while True:
            found = self.ask("POST", "/elements", {"using": "css selector", "value": css})
            if found:
                return found[0][ELEMENT]
            if time.time() > end:
                raise RuntimeError(f"не дождались {css}")
            time.sleep(0.1)

    def text(self, css):
        return self.ask("GET", f"/element/{self.find(css)}/property/textContent")

    def count(self, css):
        return len(self.ask("POST", "/elements", {"using": "css selector", "value": css}))

    def click(self, css):
        self.ask("POST", f"/element/{self.find(css)}/click", {})

    def type(self, css, text):
        self.ask("POST", f"/element/{self.find(css)}/value", {"text": text})

    def until(self, css, predicate, timeout=20):
        end = time.time() + timeout
        while True:
            text = self.text(css)
            if predicate(text) or time.time() > end:
                return text
            time.sleep(0.1)

    def shot(self, path):
        with open(path, "wb") as f:
            f.write(base64.b64decode(self.ask("GET", "/screenshot")))

    def quit(self):
        try:
            self.ask("DELETE", "")
        finally:
            self.driver.terminate()
            self.driver.wait(timeout=10)


def host_errors(ff, manifest, home):
    """Слова окна, когда с хостом что-то не так (extension.md §5)."""
    with open(manifest, encoding="utf-8") as f:
        original = f.read()
    dead = os.path.join(home, "dead-host.sh")
    with open(dead, "w", encoding="utf-8") as f:
        f.write("#!/bin/sh\necho 'Error response from daemon: container dev-1 is not running' >&2\nexit 1\n")
    os.chmod(dead, 0o755)

    def error_with(changes):
        if changes is None:
            os.remove(manifest)
        else:
            with open(manifest, "w", encoding="utf-8") as f:
                json.dump({**json.loads(original), **changes}, f)
        ff.popup("https://example.org/")
        ff.find("#error:not([hidden])")
        return ff.text("#error")

    try:
        check("нет манифеста — «не установлен»",
              error_with(None).startswith("хост bc не установлен или не пускает это расширение"), True)
        check("манифест для чужого расширения — то же",
              error_with({"allowed_extensions": ["other@example.org"]}).startswith("хост bc не установлен"), True)
        check("bc вышел, не ответив, — «завершился»",
              error_with({"path": dead}).startswith("bc завершился, не ответив"), True)
        check("нет скрипта запуска — «не запускается»",
              error_with({"path": os.path.join(home, "missing.sh")}).startswith("хост bc не запускается"), True)
    finally:
        with open(manifest, "w", encoding="utf-8") as f:
            f.write(original)


def main():
    xpi, home, bc, data = sys.argv[1:5]
    shot = os.environ.get("SHOT")
    ff = Firefox(home)
    try:
        check("bc.xpi без подписи ставится насовсем", ff.install(xpi), ADDON_ID)

        ff.open(VIDEO, "#social")
        check("ключ страницы ролика", ff.text("#key"), "yt:jNQXAC9IVRw")
        check("подпись — ветка браузера", ff.text("#who").endswith("подпись — ветка браузера"), True)

        ff.click("#like")
        check("лайк в картине круга", ff.until("#reactions", lambda t: "+1" in t).strip(), "реакции круга: +1")

        ff.type("#comment-text", COMMENT)
        ff.click("#comment-form button[type=submit]")
        ff.find("#comments li[data-id]")
        check("комментарий — текстом, а не разметкой", ff.text("#comments li[data-id] div:nth-child(2)"), COMMENT)
        check("разметка из чужого текста не вставлена", ff.count("#comments b"), 0)
        if shot:
            ff.shot(f"{shot}-firefox-video.png")

        ff.open(SHOP, "#product")
        check("ключ страницы товара", ff.text("#key"), "wb:777777")
        ff.click('#stars button[data-stars="4"]')
        ff.type("#review-text", "лёгкий")
        ff.click("#review-form button[type=submit]")
        ff.find("#reviews li[data-id]")
        check("отзыв в картине товара", ff.text("#summary").startswith("отзывов: 1 · медиана 4★"), True)
        if shot:
            ff.shot(f"{shot}-firefox-shop.png")

        host_errors(ff, os.path.join(home, ".mozilla/native-messaging-hosts/org.free_labour.bc.json"), home)
    finally:
        ff.quit()

    # Заметки легли в ветку браузера — смотрим глазами самого bc.
    listed = subprocess.run([bc, "--data-dir", data, "list", "--leaf", BROWSER_LEAF],
                            capture_output=True, text=True).stdout
    check("заметки окна — в ветке браузера", all(s in listed for s in ("yt:jNQXAC9IVRw", COMMENT, "wb:777777")), True)
    return failed


if __name__ == "__main__":
    sys.exit(main())

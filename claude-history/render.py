#!/usr/bin/env python3
"""Рендер JSONL-транскриптов Claude Code в человекочитаемые Markdown и HTML.

Вход:  raw/*.jsonl (родной формат Claude Code — источник истины)
Выход: md/*.md, html/*.html + html/index.html
"""
import argparse
import html
import json
import re
from datetime import datetime, timezone
from pathlib import Path

TOOL_CAP_HTML = 8000     # обрезка длинного вывода инструмента в HTML
TOOL_CAP_MD = 2000       # то же для Markdown
SERVICE_PREFIXES = (
    "<system-reminder>", "<ide_selection>", "<ide_opened_file>",
    "<local-command-caveat>", "<command-name>", "<command-message>",
    "<command-args>", "Caveat: The messages below",
)


# ─────────────────────────────── разбор ───────────────────────────────

def ts_parse(s):
    if not s:
        return None
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).astimezone()
    except ValueError:
        return None


def fmt_ts(dt, with_date=True):
    if not dt:
        return ""
    return dt.strftime("%Y-%m-%d %H:%M" if with_date else "%H:%M")


def is_service_text(t):
    t = t.lstrip()
    return t.startswith(SERVICE_PREFIXES)


def tool_result_text(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for b in content:
            if not isinstance(b, dict):
                parts.append(str(b))
            elif b.get("type") == "text":
                parts.append(b.get("text", ""))
            elif b.get("type") == "image":
                parts.append("[изображение]")
            else:
                parts.append(f"[{b.get('type')}] " + json.dumps(
                    {k: v for k, v in b.items() if k != "type"}, ensure_ascii=False)[:500])
        return "\n".join(parts)
    return "" if content is None else str(content)


def tool_input_summary(name, inp):
    """Короткая подпись инструмента в заголовке свёртки."""
    if not isinstance(inp, dict):
        return ""
    for k in ("description", "command", "file_path", "pattern", "path", "query", "prompt", "url"):
        v = inp.get(k)
        if isinstance(v, str) and v.strip():
            v = " ".join(v.split())
            return v[:110] + ("…" if len(v) > 110 else "")
    return ""


def tool_input_body(inp):
    if isinstance(inp, dict):
        if set(inp) == {"command", "description"} or (
                "command" in inp and isinstance(inp["command"], str) and len(inp) <= 3):
            body = inp["command"]
            rest = {k: v for k, v in inp.items() if k not in ("command", "description")}
            if rest:
                body += "\n\n" + json.dumps(rest, ensure_ascii=False, indent=2)
            return body
        return json.dumps(inp, ensure_ascii=False, indent=2)
    return str(inp)


def parse_session(path):
    """→ dict с метаданными и списком событий."""
    records = []
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue

    meta = {
        "id": path.stem,
        "title": None,
        "start": None,
        "end": None,
        "branch": None,
        "cwd": None,
        "version": None,
        "models": {},
        "n_user": 0,
        "n_assistant": 0,
        "n_tools": 0,
        "bytes": path.stat().st_size,
    }
    results = {}       # tool_use_id → текст результата
    for r in records:
        if r.get("type") == "ai-title" and r.get("aiTitle"):
            meta["title"] = r["aiTitle"]
        msg = r.get("message") or {}
        c = msg.get("content")
        if isinstance(c, list):
            for b in c:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    results[b.get("tool_use_id")] = (
                        tool_result_text(b.get("content")), bool(b.get("is_error")))

    events = []
    for r in records:
        t = r.get("type")
        dt = ts_parse(r.get("timestamp"))
        if dt:
            meta["start"] = min(meta["start"], dt) if meta["start"] else dt
            meta["end"] = max(meta["end"], dt) if meta["end"] else dt
        meta["branch"] = r.get("gitBranch") or meta["branch"]
        meta["cwd"] = r.get("cwd") or meta["cwd"]
        meta["version"] = r.get("version") or meta["version"]

        if t == "system" and r.get("subtype") == "api_error":
            err = r.get("error") or {}
            events.append({"kind": "error", "ts": dt,
                           "text": str(err.get("message", ""))[:400]})
            continue
        if t not in ("user", "assistant"):
            continue

        msg = r.get("message") or {}
        content = msg.get("content")
        if isinstance(content, str):
            content = [{"type": "text", "text": content}]
        if not isinstance(content, list):
            continue

        if t == "assistant":
            meta["models"][msg.get("model")] = meta["models"].get(msg.get("model"), 0) + 1

        said = False
        for b in content:
            if not isinstance(b, dict):
                continue
            bt = b.get("type")
            if bt == "text":
                txt = b.get("text", "")
                if not txt.strip():
                    continue
                if t == "user" and is_service_text(txt):
                    events.append({"kind": "service", "ts": dt, "text": txt})
                else:
                    events.append({"kind": "user" if t == "user" else "assistant",
                                   "ts": dt, "text": txt,
                                   "model": msg.get("model")})
                    said = True
            elif bt == "thinking":
                th = b.get("thinking", "")
                if th.strip():
                    events.append({"kind": "thinking", "ts": dt, "text": th})
            elif bt == "tool_use":
                res, err = results.get(b.get("id"), ("", False))
                events.append({"kind": "tool", "ts": dt, "name": b.get("name", "?"),
                               "input": b.get("input"), "result": res, "error": err})
                meta["n_tools"] += 1
            # tool_result рендерится внутри своего tool_use
        if said:
            meta["n_user" if t == "user" else "n_assistant"] += 1

    meta["events"] = events
    return meta


# ─────────────────────────────── Markdown ───────────────────────────────

def esc_summary(s):
    """<summary> в Markdown парсится как HTML — экранируем угловые скобки."""
    return (s or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def cut(s, n):
    s = s or ""
    if len(s) <= n:
        return s, False
    return s[:n], True


def fence(s):
    """Забор из кавычек длиннее любого внутри текста."""
    longest = max((len(m) for m in re.findall(r"`+", s or "")), default=0)
    return "`" * max(3, longest + 1)


def render_md(meta):
    L = []
    L.append(f"# {meta['title'] or 'Без названия'}\n")
    L.append(f"- **Сессия:** `{meta['id']}`")
    L.append(f"- **Период:** {fmt_ts(meta['start'])} — {fmt_ts(meta['end'])}")
    L.append(f"- **Реплик:** пользователь {meta['n_user']}, Claude {meta['n_assistant']}, "
             f"вызовов инструментов {meta['n_tools']}")
    if meta["branch"]:
        L.append(f"- **Ветка git:** `{meta['branch']}`")
    if meta["models"]:
        models = ", ".join(f"{m} ×{n}" for m, n in
                           sorted(meta["models"].items(), key=lambda kv: -kv[1]) if m)
        L.append(f"- **Модели:** {models}")
    L.append(f"- **Исходник:** [`raw/{meta['id']}.jsonl`](../raw/{meta['id']}.jsonl)")
    L.append("\n---\n")

    for e in meta["events"]:
        k = e["kind"]
        if k == "user":
            L.append(f"\n## 👤 Пользователь · {fmt_ts(e['ts'])}\n")
            L.append(e["text"].rstrip())
        elif k == "assistant":
            L.append(f"\n## 🤖 Claude · {fmt_ts(e['ts'])}\n")
            L.append(e["text"].rstrip())
        elif k == "thinking":
            body, trunc = cut(e["text"], 4000)
            L.append(f"\n<details><summary>💭 Размышления ({len(e['text'])} симв.)</summary>\n")
            L.append(body + ("\n\n…обрезано, полностью — в raw/*.jsonl" if trunc else ""))
            L.append("\n</details>\n")
        elif k == "tool":
            sub = esc_summary(tool_input_summary(e["name"], e["input"]))
            head = f"🔧 {esc_summary(e['name'])}" + (f" — {sub}" if sub else "")
            if e["error"]:
                head = "⚠️ " + head
            L.append(f"\n<details><summary>{head}</summary>\n")
            inp, _ = cut(tool_input_body(e["input"]), TOOL_CAP_MD)
            f1 = fence(inp)
            L.append(f"{f1}\n{inp}\n{f1}\n")
            if e["result"]:
                res, trunc = cut(e["result"], TOOL_CAP_MD)
                f2 = fence(res)
                L.append(f"**Результат:**\n\n{f2}\n{res}\n" +
                         ("\n…обрезано, полностью — в raw/*.jsonl\n" if trunc else "") + f"{f2}\n")
            L.append("</details>\n")
        elif k == "service":
            first = esc_summary(" ".join(e["text"].split())[:80])
            L.append(f"\n<details><summary>⚙️ служебное сообщение — {first}…</summary>\n")
            body, trunc = cut(e["text"], 1500)
            f3 = fence(body)
            L.append(f"{f3}\n{body}\n{f3}\n</details>\n")
        elif k == "error":
            L.append(f"\n> ⚠️ **Ошибка API:** {e['text']}\n")
    return "\n".join(L) + "\n"


# ─────────────────────────────── HTML ───────────────────────────────

CSS = """
:root{--bg:#fbfaf8;--fg:#1f1d1b;--muted:#6f6a63;--line:#e3ded6;--card:#fff;
--user:#eef3fb;--userline:#4a7ec2;--ai:#fff;--aiiline:#c9a227;--tool:#f5f3ef;--think:#f7f2fb}
@media (prefers-color-scheme:dark){:root{--bg:#17161a;--fg:#e9e6e1;--muted:#9b958c;
--line:#312e33;--card:#1e1d21;--user:#1c2431;--userline:#5b8fd0;--ai:#1e1d21;--aiiline:#c9a227;
--tool:#232227;--think:#241f2b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.6 -apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif}
.wrap{max-width:900px;margin:0 auto;padding:24px 18px 80px}
a{color:inherit}
header.top{position:sticky;top:0;background:var(--bg);border-bottom:1px solid var(--line);
padding:10px 0;margin-bottom:18px;z-index:5}
h1{font-size:1.5rem;margin:.2em 0 .3em;line-height:1.25}
.meta{color:var(--muted);font-size:.85rem;margin-bottom:1.4em}
.meta code{background:var(--tool);padding:1px 5px;border-radius:4px}
.msg{border:1px solid var(--line);border-left:4px solid var(--line);border-radius:8px;
padding:12px 16px;margin:14px 0;background:var(--card);overflow-wrap:anywhere}
.msg.user{background:var(--user);border-left-color:var(--userline)}
.msg.ai{background:var(--ai);border-left-color:var(--aiiline)}
.who{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin-bottom:6px}
.msg p{margin:.6em 0}
pre{background:var(--tool);border:1px solid var(--line);border-radius:6px;padding:10px 12px;
overflow-x:auto;font-size:.82rem;line-height:1.45;margin:.6em 0}
code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.88em}
details{border:1px solid var(--line);border-radius:8px;margin:10px 0;background:var(--tool)}
details.think{background:var(--think)}
summary{cursor:pointer;padding:8px 12px;font-size:.85rem;color:var(--muted);
white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
details[open]>summary{border-bottom:1px solid var(--line);margin-bottom:6px}
details .body{padding:0 12px 10px}
.err{border-left:4px solid #c0392b;padding:8px 14px;background:var(--tool);border-radius:6px;font-size:.85rem}
table{border-collapse:collapse;width:100%;font-size:.9rem}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600;font-size:.8rem;text-transform:uppercase;letter-spacing:.05em}
tr:hover td{background:var(--tool)}
td.n{text-align:right;color:var(--muted);white-space:nowrap;font-variant-numeric:tabular-nums}
#q{width:100%;padding:10px 12px;font-size:1rem;border:1px solid var(--line);border-radius:8px;
background:var(--card);color:var(--fg);margin:6px 0 16px}
.tot{color:var(--muted);font-size:.85rem;margin-bottom:10px}
.back{font-size:.85rem;color:var(--muted);text-decoration:none}
.tbl-scroll{overflow-x:auto}
"""


def md_lite_to_html(text):
    """Минимальный markdown: блоки кода, inline-код, жирный, заголовки, списки."""
    out = []
    for i, chunk in enumerate(re.split(r"```", text)):
        if i % 2:  # внутри блока кода
            lines = chunk.split("\n")
            if lines and re.fullmatch(r"[a-zA-Z0-9_+-]*", lines[0].strip()):
                lines = lines[1:]
            out.append("<pre><code>" + html.escape("\n".join(lines).strip("\n")) + "</code></pre>")
        else:
            e = html.escape(chunk)
            e = re.sub(r"`([^`\n]+)`", r"<code>\1</code>", e)
            e = re.sub(r"\*\*([^*\n]+)\*\*", r"<strong>\1</strong>", e)
            e = re.sub(r"^#{1,6}\s+(.+)$", r"<strong>\1</strong>", e, flags=re.M)
            paras = [p.strip() for p in re.split(r"\n\s*\n", e) if p.strip()]
            out.append("".join("<p>" + p.replace("\n", "<br>") + "</p>" for p in paras))
    return "".join(out)


def render_html(meta, prev_link=None, next_link=None):
    t = html.escape(meta["title"] or "Без названия")
    P = []
    P.append(f"<!doctype html><html lang=ru><head><meta charset=utf-8>"
             f"<meta name=viewport content='width=device-width,initial-scale=1'>"
             f"<title>{t}</title><style>{CSS}</style></head><body><div class=wrap>")
    P.append("<header class=top><a class=back href='index.html'>← ко всем чатам</a></header>")
    P.append(f"<h1>{t}</h1>")
    models = ", ".join(html.escape(str(m)) for m, _ in
                       sorted(meta["models"].items(), key=lambda kv: -kv[1]) if m)
    P.append("<div class=meta>"
             f"{fmt_ts(meta['start'])} — {fmt_ts(meta['end'])} · "
             f"реплик: 👤 {meta['n_user']} / 🤖 {meta['n_assistant']} · "
             f"инструментов: {meta['n_tools']}"
             + (f" · ветка <code>{html.escape(meta['branch'])}</code>" if meta["branch"] else "")
             + (f" · {models}" if models else "")
             + f" · <a href='../raw/{meta['id']}.jsonl'>raw jsonl</a>"
             f"<br><code>{meta['id']}</code></div>")

    for e in meta["events"]:
        k = e["kind"]
        if k in ("user", "assistant"):
            cls = "user" if k == "user" else "ai"
            who = "👤 Пользователь" if k == "user" else "🤖 Claude"
            P.append(f"<div class='msg {cls}'><div class=who>{who} · {fmt_ts(e['ts'])}</div>"
                     + md_lite_to_html(e["text"]) + "</div>")
        elif k == "thinking":
            P.append("<details class=think><summary>💭 Размышления "
                     f"({len(e['text'])} симв.)</summary><div class=body>"
                     + md_lite_to_html(e["text"]) + "</div></details>")
        elif k == "tool":
            sub = html.escape(tool_input_summary(e["name"], e["input"]))
            mark = "⚠️ " if e["error"] else "🔧 "
            P.append(f"<details><summary>{mark}<b>{html.escape(e['name'])}</b>"
                     + (f" — {sub}" if sub else "") + "</summary><div class=body>")
            inp, tr1 = cut(tool_input_body(e["input"]), TOOL_CAP_HTML)
            P.append("<pre><code>" + html.escape(inp) +
                     ("\n…обрезано" if tr1 else "") + "</code></pre>")
            if e["result"]:
                res, tr2 = cut(e["result"], TOOL_CAP_HTML)
                P.append("<pre><code>" + html.escape(res) +
                         ("\n…обрезано, полностью — в raw/*.jsonl" if tr2 else "") + "</code></pre>")
            P.append("</div></details>")
        elif k == "service":
            first = html.escape(" ".join(e["text"].split())[:90])
            body, _ = cut(e["text"], 4000)
            P.append(f"<details><summary>⚙️ служебное — {first}…</summary><div class=body>"
                     "<pre><code>" + html.escape(body) + "</code></pre></div></details>")
        elif k == "error":
            P.append("<div class=err>⚠️ Ошибка API: " + html.escape(e["text"]) + "</div>")
    P.append("</div></body></html>")
    return "".join(P)


def render_index(items):
    rows = []
    for it in items:
        rows.append(
            f"<tr data-s=\"{html.escape((it['title'] or '').lower())} {it['id']} {it['date']}\">"
            f"<td class=n>{it['date']}</td>"
            f"<td><a href='{it['html']}'>{html.escape(it['title'] or 'Без названия')}</a>"
            f"<br><span style='color:var(--muted);font-size:.78rem'>{it['dur']} · "
            f"<a href='../md/{it['md']}'>md</a> · <a href='../raw/{it['id']}.jsonl'>jsonl</a></span></td>"
            f"<td class=n>{it['n_user']}</td><td class=n>{it['n_assistant']}</td>"
            f"<td class=n>{it['n_tools']}</td><td class=n>{it['size']}</td></tr>")
    tot_u = sum(i["n_user"] for i in items)
    tot_a = sum(i["n_assistant"] for i in items)
    tot_t = sum(i["n_tools"] for i in items)
    tot_b = sum(i["bytes"] for i in items)
    return (
        "<!doctype html><html lang=ru><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        f"<title>Архив чатов Claude Code</title><style>{CSS}</style></head><body><div class=wrap>"
        "<h1>Архив чатов Claude Code</h1>"
        f"<div class=tot>Сессий: {len(items)} · реплик пользователя: {tot_u} · "
        f"ответов Claude: {tot_a} · вызовов инструментов: {tot_t} · "
        f"объём raw: {tot_b/1048576:.1f} МБ<br>"
        f"обновлено {datetime.now().strftime('%Y-%m-%d %H:%M')}</div>"
        "<input id=q placeholder='фильтр по названию или дате…' autocomplete=off>"
        "<div class=tbl-scroll><table><thead><tr><th>Дата</th><th>Чат</th><th>👤</th><th>🤖</th>"
        "<th>🔧</th><th>Размер</th></tr></thead><tbody id=tb>"
        + "".join(rows) +
        "</tbody></table></div>"
        "<script>const q=document.getElementById('q'),tb=document.getElementById('tb');"
        "q.addEventListener('input',()=>{const v=q.value.toLowerCase().trim();"
        "for(const tr of tb.rows){tr.style.display=!v||tr.dataset.s.includes(v)?'':'none';}});"
        "</script></div></body></html>")


def human_size(n):
    for u in ("Б", "КБ", "МБ", "ГБ"):
        if n < 1024:
            return f"{n:.0f} {u}" if u == "Б" else f"{n:.1f} {u}"
        n /= 1024
    return f"{n:.1f} ТБ"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, help="каталог архива (содержит raw/)")
    args = ap.parse_args()
    root = Path(args.root)
    raw, mdd, htd = root / "raw", root / "md", root / "html"
    mdd.mkdir(exist_ok=True)
    htd.mkdir(exist_ok=True)

    metas = []
    for p in sorted(raw.glob("*.jsonl")):
        m = parse_session(p)
        if not m["events"]:
            continue
        metas.append(m)
    metas.sort(key=lambda m: m["start"] or datetime(1970, 1, 1, tzinfo=timezone.utc))

    items = []
    for m in metas:
        stamp = (m["start"] or datetime.now()).strftime("%Y-%m-%d_%H%M")
        base = f"{stamp}_{m['id'][:8]}"
        (mdd / f"{base}.md").write_text(render_md(m), encoding="utf-8")
        (htd / f"{base}.html").write_text(render_html(m), encoding="utf-8")
        dur = ""
        if m["start"] and m["end"]:
            mins = int((m["end"] - m["start"]).total_seconds() // 60)
            dur = f"{mins//60} ч {mins%60} мин" if mins >= 60 else f"{mins} мин"
        items.append({"id": m["id"], "title": m["title"], "html": f"{base}.html",
                      "md": f"{base}.md", "date": (m["start"] or datetime.now()).strftime("%Y-%m-%d"),
                      "n_user": m["n_user"], "n_assistant": m["n_assistant"],
                      "n_tools": m["n_tools"], "bytes": m["bytes"],
                      "size": human_size(m["bytes"]), "dur": dur})
    items.sort(key=lambda i: i["date"], reverse=True)
    (htd / "index.html").write_text(render_index(items), encoding="utf-8")

    idx_md = ["# Архив чатов Claude Code\n",
              f"Сессий: {len(items)}. Обновлено: {datetime.now().strftime('%Y-%m-%d %H:%M')}.\n",
              "Открыть как чат в браузере: [`html/index.html`](html/index.html)\n",
              "| Дата | Чат | 👤 | 🤖 | 🔧 |", "|---|---|--:|--:|--:|"]
    for i in items:
        title = (i["title"] or "Без названия").replace("|", "\\|")
        idx_md.append(f"| {i['date']} | [{title}](md/{i['md']}) | {i['n_user']} | "
                      f"{i['n_assistant']} | {i['n_tools']} |")
    (root / "INDEX.md").write_text("\n".join(idx_md) + "\n", encoding="utf-8")
    print(f"Отрендерено сессий: {len(items)} → md/, html/, INDEX.md")


if __name__ == "__main__":
    main()

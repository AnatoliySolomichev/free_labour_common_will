// Окно расширения bc (extension.md §4): картина вашего круга по странице и
// действия с ней. Подписывает не расширение, а bc — ключом ветки браузера
// (extension.md §2); сюда ключ не попадает. Чужие тексты — только textContent:
// комментарии и отзывы приходят из чужих цепей.
'use strict';

const HOST = 'org.free_labour.bc';
const api = globalThis.browser ?? globalThis.chrome;
const PRODUCT = /^(gtin|model|ozon|wb|ymarket|amazon|ali):/;   // ключи товара (social.md §3.6)

let seq = 0;
let subject = '';   // ключ страницы
let me = '';        // своя цепь
let stars = 0;      // выбранные звёзды отзыва

const $ = (id) => document.getElementById(id);

// Запрос к bc через native messaging; отказ bc — исключение с его словами.
async function ask(cmd, fields = {}) {
  const reply = await api.runtime.sendNativeMessage(HOST, { id: ++seq, cmd, ...fields });
  if (!reply) throw new Error('bc не ответил');
  if (!reply.ok) throw new Error(reply.error || reply.text || 'bc отказал');
  return reply;
}

// Адрес страницы: из ?url= (проверка и отладка) или активной вкладки.
async function currentUrl() {
  const fromQuery = new URLSearchParams(location.search).get('url');
  if (fromQuery) return fromQuery;
  const [tab] = await api.tabs.query({ active: true, currentWindow: true });
  return tab?.url ?? '';
}

function el(tag, text, cls) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (cls) node.className = cls;
  return node;
}

const tally = (t) => [t.plus ? `+${t.plus}` : '', t.minus ? `−${t.minus}` : ''].filter(Boolean).join(' ') || '—';
const who = (chain) => (chain === me ? 'вы' : `${chain.slice(0, 8)}…`);
const when = (at) => new Date(at * 1000).toISOString().slice(0, 16).replace('T', ' ');
const minutes = (sec) => (sec < 3600 ? `${Math.round(sec / 60)} мин` : `${(sec / 3600).toFixed(1)} ч`);

function say(text, isError = false) {
  $('status').textContent = isError ? '' : text;
  $('error').hidden = !isError;
  $('error').textContent = isError ? text : '';
}

function renderSocial(p) {
  const r = p.reactions;
  const circle = { plus: r.me.plus + r.step1.plus + r.step2.plus, minus: r.me.minus + r.step1.minus + r.step2.minus };
  $('reactions').textContent = `реакции круга: ${tally(circle)}` +
    (r.outside.plus || r.outside.minus ? ` · вне круга ${tally(r.outside)} от ${r.outside_chains} цеп.` : '');
  const v = p.views;
  const viewers = v.me + v.step1 + v.step2;
  $('views').textContent = viewers
    ? `смотрели в круге: ${viewers} цеп.` + (v.known ? `, внимание ${minutes(v.attention_sec)}` : '')
    : v.outside_chains ? `смотрели вне круга: ${v.outside_chains} цеп.` : '';
  const list = $('comments');
  list.replaceChildren();
  const shown = new Set(p.comments.map((c) => c.id));
  for (const c of p.comments) {
    const item = el('li', undefined, c.parent && shown.has(c.parent) ? 'reply' : '');
    item.dataset.id = c.id;
    item.append(el('div', `${who(c.chain)} · ${when(c.at)}` + (c.plus || c.minus ? ` · ${tally(c)}` : ''), 'meta'));
    item.append(el('div', c.retracted ? 'снят автором' : c.text, c.retracted ? 'muted' : ''));
    list.append(item);
  }
  if (!p.comments.length) list.append(el('li', 'пока никто из круга не писал', 'muted'));
  $('hidden-comments').textContent = p.hidden_comments ? `вне круга: ${p.hidden_comments} коммент.` : '';
  $('social').hidden = false;
}

function renderProduct(p) {
  $('names').textContent = p.names.length ? `он же: ${p.names.join(', ')}` : '';
  $('summary').textContent = p.reviews.length
    ? `отзывов: ${p.reviews.length} · медиана ${p.median}★`
    : 'отзывов круга пока нет';
  $('spread').textContent = p.reviews.length > 1
    ? [5, 4, 3, 2, 1].map((s) => `${s}★ ${p.spread[s]}`).join(' · ')
    : '';
  const list = $('reviews');
  list.replaceChildren();
  for (const r of p.reviews) {
    const item = el('li');
    item.dataset.id = r.id;
    const purchase = { confirmed: ' · покупка подтверждена', unconfirmed: ' · покупка не подтверждена',
                       none: ' · без сделки покупки' }[r.purchase] ?? '';
    item.append(el('div', `${'★'.repeat(r.stars)}${'☆'.repeat(5 - r.stars)} · ${who(r.chain)}${purchase}` +
                          (r.plus || r.minus ? ` · полезно ${tally(r)}` : ''), 'meta'));
    if (r.text) item.append(el('div', r.text));
    list.append(item);
  }
  $('product').hidden = false;
}

async function refresh() {
  const picture = await ask('show', { subject });
  renderSocial(picture.data);
  if (PRODUCT.test(subject)) renderProduct((await ask('reviews', { subject })).data);
}

// Действие — запись в вашу цепь, потом свежая картина.
async function act(cmd, fields, done) {
  try {
    await ask(cmd, { subject, ...fields });
    say(done);
    await refresh();
  } catch (e) {
    say(e.message, true);
  }
}

function buildStars() {
  const box = $('stars');
  for (let s = 1; s <= 5; ++s) {
    const b = el('button', '★');
    b.type = 'button';
    b.dataset.stars = String(s);
    b.setAttribute('role', 'radio');
    b.addEventListener('click', () => {
      stars = s;
      for (const x of box.children) x.setAttribute('aria-checked', String(Number(x.dataset.stars) <= s));
    });
    box.append(b);
  }
}

async function main() {
  buildStars();
  try {
    const url = await currentUrl();
    subject = (await ask('key', { subject: url })).data.key;
    $('key').textContent = subject;
    const whoami = (await ask('whoami')).data;
    me = whoami.chain;
    $('who').textContent = `вы: ${me.slice(0, 8)}… · подпись — ветка браузера`;
    await refresh();
  } catch (e) {
    say(e.message, true);
    return;
  }
  $('like').addEventListener('click', () => act('like', {}, '👍 записано в вашу цепь'));
  $('dislike').addEventListener('click', () => act('dislike', {}, '👎 записано в вашу цепь'));
  $('unlike').addEventListener('click', () => act('unlike', {}, 'реакция снята'));
  $('watched').addEventListener('click', () => act('watch', {}, 'в журнале — уйдёт пакетом'));
  $('publish').addEventListener('click', () => act('views.publish', {}, 'журнал опубликован'));
  $('comment-form').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = $('comment-text').value.trim();
    if (!text) return;
    act('comment', { text }, 'комментарий записан').then(() => { $('comment-text').value = ''; });
  });
  $('review-form').addEventListener('submit', (e) => {
    e.preventDefault();
    if (!stars) return say('выберите звёзды', true);
    act('review', { stars, text: $('review-text').value.trim() }, 'отзыв записан').then(() => {
      $('review-text').value = '';
      stars = 0;
      for (const x of $('stars').children) x.setAttribute('aria-checked', 'false');
    });
  });
}

main();

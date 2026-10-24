// Расширение bc в настоящем Chromium (extension.md §6): окно на странице ролика
// и товара — лайк, комментарий, отзыв — через хост native messaging в bc; и
// слова окна, когда с хостом что-то не так.
//
// Запуск (обычно из host-e2e.sh):
//   node browser_test.cjs КАТАЛОГ_РАСШИРЕНИЯ ПРОФИЛЬ BC КАТАЛОГ_ДАННЫХ URL_АГРЕГАТОРА
'use strict';

const { chromium } = require('playwright');
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const [extDir, profile, bc, data] = process.argv.slice(2);
const EXT_ID = 'dmmadjknblhkhcfhjblkaikanclebljf';   // из key в manifest.json
const BROWSER_LEAF = '2147483648';
const VIDEO = 'https://www.youtube.com/watch?v=M7lc1UVf-VE&t=42s';
const SHOP = 'https://www.wildberries.ru/catalog/654321/detail.aspx?targetUrl=GP';
const COMMENT = 'из настоящего браузера <b>не жирный</b>';

// Ждать текста — опросом отсюда: страницы расширения запрещают eval (CSP), а
// waitForFunction исполняет условие внутри страницы именно через него.
async function until(page, selector, predicate, timeout = 20000) {
  const end = Date.now() + timeout;
  for (;;) {
    const text = await page.textContent(selector).catch(() => null);
    if (text !== null && predicate(text)) return text;
    if (Date.now() > end) throw new Error(`не дождались ${selector}: «${text}»`);
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
}

let failed = 0;
function check(what, fact, want) {
  const ok = JSON.stringify(fact) === JSON.stringify(want);
  console.log(`   ${ok ? '✓' : '✗'} ${what}` +
              (ok ? '' : `: получили «${JSON.stringify(fact)}», ждали «${JSON.stringify(want)}»`));
  if (!ok) failed = 1;
}

// Слова окна, когда с хостом что-то не так (extension.md §5). Манифест хоста на
// время подменяем: браузер читает его при каждом подключении.
async function hostErrors(page, popup) {
  const manifest = path.join(profile, 'NativeMessagingHosts', 'org.free_labour.bc.json');
  const original = fs.readFileSync(manifest, 'utf8');
  const dead = path.join(profile, 'dead-host.sh');
  fs.writeFileSync(dead, "#!/bin/sh\necho 'Error response from daemon: container dev-1 is not running' >&2\nexit 1\n");
  fs.chmodSync(dead, 0o755);
  const errorWith = async (changes) => {
    if (changes === null) fs.rmSync(manifest);
    else fs.writeFileSync(manifest, JSON.stringify({ ...JSON.parse(original), ...changes }));
    await page.goto(popup('https://example.org/'));
    await page.waitForSelector('#error:not([hidden])', { timeout: 20000 });
    return page.textContent('#error');
  };
  try {
    check('нет манифеста — «не установлен»', (await errorWith(null)).startsWith('хост bc не установлен:'), true);
    check('манифест для чужого расширения — «не пускает»',
          (await errorWith({ allowed_origins: ['chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/'] }))
            .startsWith('хост bc не пускает это расширение'), true);
    check('bc вышел, не ответив, — «завершился»',
          (await errorWith({ path: dead })).startsWith('bc завершился, не ответив'), true);
  } finally {
    fs.writeFileSync(manifest, original);
  }
}

(async () => {
  const ext = path.resolve(extDir);
  const context = await chromium.launchPersistentContext(profile, {
    headless: false,   // расширения — только в окне; под xvfb-run окно виртуальное
    viewport: { width: 420, height: 640 },
    args: [`--disable-extensions-except=${ext}`, `--load-extension=${ext}`],
  });
  try {
    const page = await context.newPage();
    const popup = (url) => `chrome-extension://${EXT_ID}/popup.html?url=${encodeURIComponent(url)}`;

    await page.goto(popup(VIDEO));
    await page.waitForSelector('#social:not([hidden])', { timeout: 20000 });
    check('ключ страницы ролика', await page.textContent('#key'), 'yt:M7lc1UVf-VE');
    check('подпись — ветка браузера', (await page.textContent('#who')).endsWith('подпись — ветка браузера'), true);

    await page.click('#like');
    await until(page, '#reactions', (t) => t.includes('+1'));
    check('лайк в картине круга', (await page.textContent('#reactions')).trim(), 'реакции круга: +1');

    await page.fill('#comment-text', COMMENT);
    await page.click('#comment-form button[type=submit]');
    await page.waitForSelector('#comments li[data-id]');
    check('комментарий — текстом, а не разметкой',
          await page.textContent('#comments li[data-id] div:nth-child(2)'), COMMENT);
    check('разметка из чужого текста не вставлена', await page.locator('#comments b').count(), 0);
    if (process.env.SHOT) await page.screenshot({ path: `${process.env.SHOT}-video.png`, fullPage: true });

    await page.goto(popup(SHOP));
    await page.waitForSelector('#product:not([hidden])', { timeout: 20000 });
    check('ключ страницы товара', await page.textContent('#key'), 'wb:654321');
    await page.click('#stars button[data-stars="4"]');
    await page.fill('#review-text', 'лёгкий');
    await page.click('#review-form button[type=submit]');
    await page.waitForSelector('#reviews li[data-id]');
    check('отзыв в картине товара', (await page.textContent('#summary')).startsWith('отзывов: 1 · медиана 4★'),
          true);
    if (process.env.SHOT) await page.screenshot({ path: `${process.env.SHOT}-shop.png`, fullPage: true });

    await hostErrors(page, popup);
  } finally {
    await context.close();
  }

  // Заметки легли в ветку браузера — смотрим глазами самого bc.
  const listed = execFileSync(bc, ['--data-dir', data, 'list', '--leaf', BROWSER_LEAF]).toString();
  check('заметки окна — в ветке браузера',
        ['yt:M7lc1UVf-VE', COMMENT, 'wb:654321'].every((s) => listed.includes(s)), true);
  process.exit(failed);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});

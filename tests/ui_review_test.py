import asyncio, sys, json, os
from playwright.async_api import async_playwright
EXE = sys.argv[1]; ENGID = sys.argv[2] if len(sys.argv) > 2 else 'sara'
ROOT = os.path.dirname(os.path.abspath(__file__)) + '/../app/src/main/assets'
BOOK = open(ROOT + '/book/openings.txt', encoding='utf8').read()
async def main():
    proc = await asyncio.create_subprocess_exec(EXE, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL, cwd=os.path.dirname(EXE))
    async with async_playwright() as p:
        b = await p.chromium.launch(); page = await (await b.new_context(viewport={'width':390,'height':844})).new_page()
        errs=[]; page.on('pageerror', lambda e: errs.append(str(e)))
        async def send(l): proc.stdin.write((l+'\n').encode())
        await page.expose_function('__send', send)
        await page.add_init_script("window.Android={engines(){return JSON.stringify([{id:'%s',name:'Sara'}])},startEngine(){return 'ok'},engineSend(l){window.__send(l)},getBook(){return %s},bookPath(){return ''},cores(){return 4},copy(){},keepAwake(){},diag(){return 'x'}};" % (ENGID, json.dumps(BOOK)))
        async def reader():
            while True:
                l = await proc.stdout.readline()
                if not l: break
                try: await page.evaluate("l=>window.onEngineLine&&window.onEngineLine(l)", l.decode().strip())
                except Exception: pass
        asyncio.create_task(reader())
        await page.goto('file://' + os.path.abspath(ROOT) + '/index.html')
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك')>=0", timeout=60000)
        await page.click('.bot[data-bot="king"]')
        # fool's-mate style quick game: play e4, then g4/f3 moves; just make 2 human moves then resign
        await page.click('#pBoard .sq[data-i="52"]'); await page.click('#pBoard .sq[data-i="36"]')
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك')>=0 && document.getElementById('pMoves').textContent.indexOf('e4 ')>=0", timeout=30000)
        await page.click('#pResign')
        await page.wait_for_function("!document.getElementById('review').hidden", timeout=5000)
        print('review modal shown:', (await page.inner_text('#reviewTxt')))
        await page.click('#reviewNo'); print('hidden after No:', await page.evaluate("document.getElementById('review').hidden"))
        await page.click('#pResign')  # no-op (over)
        await page.evaluate("document.getElementById('review').hidden=true")
        # yes path: new game, play, resign, press yes
        await page.click('#pNew')
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك')>=0", timeout=10000)
        await page.click('#pBoard .sq[data-i="52"]'); await page.click('#pBoard .sq[data-i="36"]')
        await page.wait_for_function("document.getElementById('pMoves').textContent.split(' ').filter(x=>x).length>=3", timeout=30000)
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك')>=0", timeout=30000)
        await page.click('#pResign'); await page.wait_for_function("!document.getElementById('review').hidden", timeout=5000)
        await page.click('#reviewYes')
        await page.wait_for_function("!document.getElementById('aResult').hidden", timeout=60000)
        print('analysis opened after Yes; tab analyze visible:', await page.evaluate("!document.getElementById('tab-analyze').hidden"))
        print('errors:', errs); proc.stdin.write(b'quit\n'); await b.close()
asyncio.run(main())

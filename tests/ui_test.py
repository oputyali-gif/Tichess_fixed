# اختبار الواجهة في Chromium مع محرك حقيقي: python3 tests/ui_test.py <engine-exe> <outdir>
import asyncio, sys, json, os
from playwright.async_api import async_playwright
EXE = sys.argv[1]; OUT = sys.argv[2]; ENGID = sys.argv[3] if len(sys.argv) > 3 else 'sara'; os.makedirs(OUT, exist_ok=True)
ROOT = os.path.dirname(os.path.abspath(__file__)) + '/../app/src/main/assets'
BOOK = open(ROOT + '/book/openings.txt', encoding='utf8').read()
PGN = '1. e4 e5 2. Nf3 d6 3. d4 Bg4 4. dxe5 Bxf3 5. Qxf3 dxe5 6. Bc4 Nf6 7. Qb3 Qe7 8. Nc3 c6 9. Bg5 b5 10. Nxb5 cxb5 11. Bxb5+ Nbd7 12. O-O-O Rd8 13. Rxd7 Rxd7 14. Rd1 Qe6 15. Bxd7+ Nxd7 16. Qb8+ Nxb8 17. Rd8#'

async def main():
    proc = await asyncio.create_subprocess_exec(EXE, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL, cwd=os.path.dirname(EXE))
    async with async_playwright() as p:
        b = await p.chromium.launch()
        ctx = await b.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2)
        page = await ctx.new_page()
        errors = []
        page.on('pageerror', lambda e: errors.append(str(e)))
        page.on('console', lambda m: errors.append('console:' + m.text) if m.type == 'error' else None)
        async def send(line):
            proc.stdin.write((line + '\n').encode())
        await page.expose_function('__send', send)
        await page.add_init_script("window.__osc=0; if (window.AudioContext){ const oc=AudioContext.prototype.createOscillator; AudioContext.prototype.createOscillator=function(){window.__osc++; return oc.call(this)} }")
        await page.add_init_script("""
          window.Android = {
            engines(){ return JSON.stringify([{id:'%s',name:'%s'}]); },
            startEngine(id){ return 'ok'; },
            engineSend(l){ window.__send(l); },
            getBook(){ return %s; },
            bookPath(){ return '%s/book/openings.txt'; },
            cores(){ return 4; },
            copy(t){ window.__copied = t; },
            keepAwake(b){}
          };""" % (ENGID, os.path.basename(EXE), json.dumps(BOOK), ROOT))
        async def reader():
            while True:
                line = await proc.stdout.readline()
                if not line: break
                try: await page.evaluate("l => window.onEngineLine && window.onEngineLine(l)", line.decode().strip())
                except Exception: pass
        rt = asyncio.create_task(reader())
        await page.goto('file://' + os.path.abspath(ROOT) + '/index.html')
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك') >= 0", timeout=60000)
        print('status:', await page.inner_text('#pStatus'))
        await page.click('.bot[data-bot="king"]')
                # play e2-e4 (indices 52 -> 36)
        await page.click('#pBoard .sq[data-i="52"]'); await page.screenshot(path=OUT + '/01_selected.png')
        await page.click('#pBoard .sq[data-i="36"]')
        await page.wait_for_function("document.querySelectorAll('#pMoves').length && document.getElementById('pMoves').textContent.split(' ').filter(x=>x).length >= 3", timeout=30000)
        await page.wait_for_function("document.getElementById('pStatus').textContent.indexOf('دورك') >= 0", timeout=30000)
        print('moves:', await page.inner_text('#pMoves'))
        await page.screenshot(path=OUT + '/02_play.png')
        await page.select_option('#pieceSel', 'set2'); await page.screenshot(path=OUT + '/02b_set2.png'); await page.select_option('#pieceSel', 'set1')
        # undo, then analysis
        await page.click('#pUndo'); print('after undo:', await page.inner_text('#pMoves'))
        await page.click('.tabs button[data-tab="analyze"]')
        await page.fill('#aText', PGN); await page.select_option('#aTime', '150')
        await page.click('#aGo')
        await page.screenshot(path=OUT + '/03_progress.png')
        await page.wait_for_function("!document.getElementById('aResult').hidden", timeout=180000)
        print('oscillators after play moves:', await page.evaluate('window.__osc'))
        await page.click('.movelist .mv[data-ply="13"]')
        await page.screenshot(path=OUT + '/04_analysis_ply13.png')
        await page.click('.movelist .mv[data-ply="18"]')
        await page.screenshot(path=OUT + '/05_analysis_ply18.png')
        o0 = await page.evaluate('window.__osc'); await page.click('.movelist .mv[data-ply="31"]'); await page.wait_for_timeout(600); o1 = await page.evaluate('window.__osc'); print('brilliant sound oscillators:', o1 - o0)
        await page.screenshot(path=OUT + '/05b_brilliant.png')
        await page.click('.movelist .mv[data-ply="33"]'); await page.screenshot(path=OUT + '/05c_mate.png')
        await page.evaluate("document.getElementById('aSummary').scrollIntoView()")
        await page.screenshot(path=OUT + '/06_summary.png')
        print('info:', (await page.inner_text('#aInfo')).replace('\n', ' | '))
        print('JS errors:', errors)
        proc.stdin.write(b'quit\n'); await b.close()
asyncio.run(main())

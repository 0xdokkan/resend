"""Puts the palettes and the sprites into the page, after sprites.py: python3 build.py [DEFAULT_PALETTE_INDEX]

Writes resend-motion-prototype.html, the page as published, and test.html, the same page
with a document around it and a self-test that runs when opened with #selftest.
"""
import json
import re
import sys
from pathlib import Path

here = Path(__file__).parent
pals = json.loads((here / "palettes.json").read_text())
default = int(sys.argv[1]) if len(sys.argv) > 1 else 0


def expand(p):
    c = {"out": p["out"], "shade": p["shade"], "bg": p["bg"], "canvas": p["canvas"], "grid": p["grid"],
         "panel": p["panel"], "edge": p["edge"], "btn": p["btn"], "btn-edge": p["btnEdge"], "well": p["well"],
         "line": p["line"], "ink": p["ink"], "muted": p["muted"], "screen": p["screen"],
         "metal-dd": p["metalDD"], "metal-d": p["metalD"], "metal": p["metal"], "metal-l": p["metalL"]}
    for f in ("acc", "ok", "link", "ask", "route"):
        t = p[f]
        c[f + "-dd"], c[f + "-d"], c[f], c[f + "-l"] = t
        c[f + "-text"] = t[2] if p["dark"] else t[0]
        c[f + "-line"] = t[2] if p["dark"] else t[1]
        c[f + "-chip"] = t[0] if p["dark"] else t[3]
        c[f + "-chip-ink"] = t[3] if p["dark"] else t[0]
    return c


tokens = "\n".join(f"  --{k}:{v};" for k, v in expand(pals[default]).items())
tokens += f"\n  color-scheme:{'dark' if pals[default]['dark'] else 'light'};"
html = (here / "template.html").read_text()
for mark, value in (("/*ROOT_TOKENS*/", tokens), ("/*SPRITES*/null", (here / "sprites.json").read_text()),
                    ("/*PALETTES*/null", json.dumps(pals, separators=(",", ":"))), ("/*DEFAULT*/0", str(default))):
    assert mark in html, mark
    html = html.replace(mark, value)
(here / "resend-motion-prototype.html").write_text(html)
skeleton = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            '<style>:root{color-scheme:light}body{margin:0;font:14px system-ui}img{max-width:100%}[hidden]{display:none!important}</style>'
            '</head><body>')
selftest = r"""
if (location.hash === '#selftest') {
  let out;
  try {
    const log = [];
    state.sound = false;
    restart(); cancelAnimationFrame(raf);
    let now = performance.now();
    for (let k = 0; k < 800 && state.playing; k++) { now += 20; frame(now); cancelAnimationFrame(raf); }
    log.push('ran to T=' + state.T + ', goal ' + el.count.textContent + ', status ' + el.status.textContent + ', cues fired ' + (fired + 1));
    seek(65); cancelAnimationFrame(raf); raf = 0;
    step(); cancelAnimationFrame(raf);
    for (let k = 0; k < 200 && state.playing; k++) { now += 20; frame(now); cancelAnimationFrame(raf); }
    log.push('step from 65 stops at T=' + state.T.toFixed(2));
    for (const id in DEV) { select(id); log.push(id + ': ' + el.body.querySelectorAll('tr').length + ' rows'); }
    for (const v of ['reduced', 'off', 'full']) { setFx(v); for (let T = 0; T <= 180; T += 0.7) { state.T = T; draw(now); sync(false); } }
    PALETTES.forEach((p, i) => { applyPalette(i, false); draw(now); });
    state.sound = true; EV.forEach(playCue); click();
    log.push('audio ' + (ac ? ac.state : 'none'));
    toggleRun(); toggleRun(); cancelAnimationFrame(raf);
    state.sound = false; state.tab = 'term';
    select('S1'); command('S1', 'bridge fdb show'); command('S1', 'ip addr');
    select('R'); seek(65); cancelAnimationFrame(raf); raf = 0; command('R', 'ip neigh'); command('R', 'ip route');
    select('A'); command('A', 'ip  addr'); command('A', 'ping -c 1 192.168.20.10'); cancelAnimationFrame(raf);
    for (let k = 0; k < 800 && state.playing; k++) { now += 20; frame(now); cancelAnimationFrame(raf); }
    command('A', 'ip neigh'); renderTerm();
    log.push('terminal of PC-A: ' + TERM.A.length + ' commands, ' + $('#term-out').textContent.length + ' characters shown');
    log.push('router said: ' + TERM.R[0].out.replace(/\\n/g, ' / '));
    const fam = document.querySelectorAll('.menu-wrap > .btn');
    fam[1].click();
    const opened = !fam[1].nextElementSibling.hidden;
    fam[1].nextElementSibling.children[2].click();
    log.push('menu ip: opened ' + opened + ', closed after a choice ' + fam[1].nextElementSibling.hidden + ', it ran: ' + TERM.A[TERM.A.length - 1].cmd + ', history ' + HIST.A.length);
    seek(65); cancelAnimationFrame(raf); raf = 0; applyPalette(0, false); draw(now); sync(true);
    document.querySelectorAll('.menu-wrap > .btn')[1].click();
    out = 'SELFTEST OK\n' + log.join('\n');
  } catch (err) { out = 'SELFTEST FAIL\n' + (err && err.stack || err); }
  const pre = document.createElement('pre');
  pre.style.cssText = 'margin:0;padding:12px;font:15px/1.4 monospace;background:#fff;color:#000;white-space:pre-wrap';
  pre.textContent = out;
  document.body.prepend(pre);
}
"""
tail = "})();\n</script>"
assert html.count(tail) == 1
(here / "test.html").write_text(skeleton + html.replace(tail, selftest + tail) + "</body></html>")
(here / "script.js").write_text(re.search(r"<script>(.*)</script>", html, re.S).group(1))
print(len(html), "bytes")

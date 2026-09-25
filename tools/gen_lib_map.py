#!/usr/bin/env python3
"""Writes the interactive library dependency map as one self-contained HTML fragment (title, styles, an inline SVG
and a little script), read from the `use` lines under lib/ like tools/gen_lib_deps.py. Not part of `make
corelib-reference`: it is for the Ziman website, when there is one, or to open in a browser.
Usage: python3 tools/gen_lib_map.py [output.html]   (default: docs/lib/library-map.html)
"""
import re, os, glob, collections, json, sys
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
mods={}
for path in glob.glob(ROOT+'/lib/*/*.zm'):
    mods[os.path.basename(path)[:-3]]=os.path.basename(os.path.dirname(path))
deps=collections.defaultdict(set)
for n in mods:
    for line in open(ROOT+'/lib/%s/%s.zm'%(mods[n],n)):
        m=re.match(r'use ([a-z_]+)',line)
        if m and m.group(1) in mods and m.group(1)!=n: deps[n].add(m.group(1))
level={}
def depth(n):
    if n not in level: level[n]=0 if not deps[n] else 1+max(depth(d) for d in deps[n])
    return level[n]
for n in mods: depth(n)
maxl=max(level.values())
rows=[sorted(n for n in mods if level[n]==l) for l in range(maxl,-1,-1)]  # top row = highest layer
# barycenter ordering sweeps
pos={}
def setpos():
    for r in rows:
        for i,n in enumerate(r): pos[n]=i/(max(1,len(r)-1))
setpos()
users=collections.defaultdict(set)
for n,ds in deps.items():
    for d in ds: users[d].add(n)
for sweep in range(6):
    for ri in range(1,len(rows)):
        rows[ri].sort(key=lambda n:(sum(pos[u] for u in users[n])/len(users[n]) if users[n] else 0.5, n)); setpos()
    for ri in range(len(rows)-2,-1,-1):
        rows[ri].sort(key=lambda n:(sum(pos[d] for d in deps[n])/len(deps[n]) if deps[n] else 0.5, n)); setpos()
NW,NH,GAPX,GAPY=120,32,10,120
full=lambda n: n if mods[n]=='core' or n==mods[n] else '%s.%s'%(mods[n],n)
W=max(len(r) for r in rows)*(NW+GAPX)+GAPX+40
X0=20
xy={}
for ri,r in enumerate(rows):
    total=len(r)*(NW+GAPX)-GAPX
    start=(W-total)/2
    for i,n in enumerate(r):
        xy[n]=(start+i*(NW+GAPX), 46+ri*(NH+GAPY))
H=46+len(rows)*(NH+GAPY)-GAPY+30
layer_label=lambda l:{0:'Foundations',1:'Building blocks',2:'Drivers and servers',3:'Front doors'}[l]
svg=[]
svg.append('<svg id="graph" viewBox="0 0 %d %d" role="img" aria-label="Module dependency diagram" xmlns="http://www.w3.org/2000/svg">'%(W,H))
svg.append('<defs><marker id="head" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L8,4 L0,8 z" class="arrow"/></marker><marker id="head-use" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L8,4 L0,8 z" class="arrow-use"/></marker><marker id="head-by" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L8,4 L0,8 z" class="arrow-by"/></marker></defs>')
# layer bands
for ri,r in enumerate(rows):
    l=maxl-ri
    y=xy[r[0]][1]
    svg.append('<text class="layer-label" x="8" y="%d">LAYER %d · %s</text>'%(y-10,l,layer_label(l).upper()))
for ri,r in enumerate(rows):
    y=46+ri*(NH+GAPY)
svg.append('<g id="edges">')
for n in sorted(mods):
    for d in sorted(deps[n]):
        x1=xy[n][0]+NW/2; y1=xy[n][1]+NH; x2=xy[d][0]+NW/2; y2=xy[d][1]
        ym=(y1+y2)/2
        # spread endpoints a little by edge index to reduce overlap
        svg.append('<path class="edge" data-from="%s" data-to="%s" d="M%.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,%.1f" marker-end="url(#head)"/>'%(full(n),full(d),x1,y1,x1,ym,x2,ym,x2,y2-1))
svg.append('</g><g id="nodes">')
for n in sorted(mods):
    x,y=xy[n]
    svg.append('<g class="node %s" data-name="%s" tabindex="0" role="button" aria-label="%s"><rect x="%.1f" y="%.1f" width="%d" height="%d" rx="4"/><text x="%.1f" y="%.1f" text-anchor="middle">%s</text></g>'%(mods[n],full(n),full(n),x,y,NW,NH,x+NW/2,y+NH/2+4.5,full(n)))
svg.append('</g></svg>')
counts={n:(len(deps[n]),len(users[n])) for n in mods}
data=json.dumps({full(n):{'uses':sorted(full(d) for d in deps[n]),'usedBy':sorted(full(u) for u in users[n]),'folder':mods[n]} for n in sorted(mods)})
html='''<title>Ziman Library Map</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;600&display=swap">
<style>
:root{--bg:#f4f5f7;--panel:#ffffff;--ink:#1c2230;--muted:#5d6678;--line:#c9cfdb;--edge:#8791a6;
--core:#3d5a99;--core-soft:#e3e9f7;--db:#1a7f78;--db-soft:#dcf1ee;--http:#b5651d;--http-soft:#f8e8d5;--time:#7a4fa3;--time-soft:#eee4f7;
--use:#d1461f;--usedby:#1c7ed6;--focus:#1c2230}
:root:not([data-theme="light"]){}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#12151c;--panel:#1a1f29;--ink:#e6e9f0;--muted:#98a2b6;--line:#2f3748;--edge:#66718a;
--core:#8fa8e6;--core-soft:#232d4a;--db:#57c7bd;--db-soft:#173a38;--http:#e8a463;--http-soft:#42301c;--time:#bf9be6;--time-soft:#35284a;--use:#ff8c66;--usedby:#6fb4f5;--focus:#e6e9f0}}
:root[data-theme="dark"]{--bg:#12151c;--panel:#1a1f29;--ink:#e6e9f0;--muted:#98a2b6;--line:#2f3748;--edge:#66718a;
--core:#8fa8e6;--core-soft:#232d4a;--db:#57c7bd;--db-soft:#173a38;--http:#e8a463;--http-soft:#42301c;--time:#bf9be6;--time-soft:#35284a;--use:#ff8c66;--usedby:#6fb4f5;--focus:#e6e9f0}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--ink);font:14px/1.5 "IBM Plex Sans",system-ui,sans-serif;padding-inline:16px;padding-block:24px 40px}
main{max-width:1680px;margin:0 auto;display:flex;flex-direction:column;gap:18px}
h1{font-size:26px;line-height:1.2;margin:0;font-weight:600;text-wrap:balance}
.lede{margin:4px 0 0;color:var(--muted);max-width:68ch}
header{display:flex;flex-wrap:wrap;gap:12px 32px;align-items:flex-end;justify-content:space-between}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;font-size:12.5px;color:var(--muted)}
.legend span{display:inline-flex;align-items:center;gap:6px}
.sw{width:12px;height:12px;border-radius:3px;border:1.5px solid}
.sw.core{border-color:var(--core);background:var(--core-soft)}.sw.db{border-color:var(--db);background:var(--db-soft)}
.sw.http{border-color:var(--http);background:var(--http-soft)}.sw.time{border-color:var(--time);background:var(--time-soft)}
.ln{width:22px;height:0;border-top:2.5px solid}.ln.use{border-color:var(--use)}.ln.usedby{border-color:var(--usedby)}
.stage{background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow-x:auto}
#graph{display:block;min-width:1500px;width:100%;height:auto}
.edge{fill:none;stroke:var(--edge);stroke-width:1.1;opacity:.42;transition:opacity .15s,stroke .15s}
.arrow{fill:var(--edge)}.arrow-use{fill:var(--use)}.arrow-by{fill:var(--usedby)}
.node rect{stroke-width:1.5;transition:opacity .15s}
.node text{font:500 11.5px "IBM Plex Mono",ui-monospace,monospace;fill:var(--ink);pointer-events:none}
.node{cursor:pointer;outline:none}
.node.core rect{fill:var(--core-soft);stroke:var(--core)}.node.db rect{fill:var(--db-soft);stroke:var(--db)}
.node.http rect{fill:var(--http-soft);stroke:var(--http)}.node.time rect{fill:var(--time-soft);stroke:var(--time)}
.node:focus-visible rect{stroke:var(--focus);stroke-width:3}
.layer-label{font:500 10.5px "IBM Plex Mono",ui-monospace,monospace;fill:var(--muted);letter-spacing:.08em}
#graph.focus .edge{opacity:.07}
#graph.focus .node{opacity:.28}
#graph.focus .node.on{opacity:1}
#graph.focus .edge.use{stroke:var(--use);opacity:1;stroke-width:2.2}
#graph.focus .edge.usedby{stroke:var(--usedby);opacity:1;stroke-width:2.2}
#graph.focus .edge.use{marker-end:url(#head-use)}
#graph.focus .edge.usedby{marker-end:url(#head-by)}
.detail{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px 28px;background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.detail h2{font:500 15px "IBM Plex Mono",ui-monospace,monospace;margin:0 0 2px}
.detail h3{margin:0 0 4px;font-size:11.5px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);font-weight:600}
.detail p{margin:0;font:400 13px/1.6 "IBM Plex Mono",ui-monospace,monospace}
code{font:12px "IBM Plex Mono",ui-monospace,monospace}
.hint{color:var(--muted);font-size:12.5px;margin:0}
.notes{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px 28px;color:var(--muted);font-size:13px}
.notes b{color:var(--ink);font-weight:600}
</style>
<main>
<header>
<div><h1>Ziman library map</h1>
<p class="lede">Which bundled module uses which, named by module, with the folder under lib/ in front when it is not core (<code>http.server</code>, <code>db.pg</code>); a program writes just the module, as in <code>use server</code>. Each arrow runs from a module to one it uses; every module in a layer depends only on layers below it. Select a module to trace it.</p></div>
<div class="legend"><span><i class="sw core"></i>core</span><span><i class="sw db"></i>databases</span><span><i class="sw http"></i>HTTP</span><span><i class="sw time"></i>time</span>
<span><i class="ln use"></i>it uses</span><span><i class="ln usedby"></i>uses it</span></div>
</header>
<div class="stage">'''+"".join(svg)+'''</div>
<section class="detail" id="detail" aria-live="polite">
<div><h2 id="d-name">db</h2><p class="hint" id="d-hint">The front door for databases: picks SQLite, Postgres or MySQL from the URL.</p></div>
<div><h3>Uses</h3><p id="d-uses"></p></div>
<div><h3>Used by</h3><p id="d-by"></p></div>
</section>
<section class="notes">
<div><b>Why sql sits at the bottom.</b> It holds SqlValue, Row and DbError so pg, mysql, sqlite, url, time and uuid can share them without using db, which uses them.</div>
<div><b>Why http is separate.</b> Request, Response, Header and HttpMethod live in http so server and client share them and neither needs the other.</div>
<div><b>Not drawn.</b> The prelude (strings, Option, builders) is in every program, and natives such as net have no module dependencies.</div>
</section>
</main>
<script>
var DATA=%s;
var HINTS={db:"The front door for databases: picks SQLite, Postgres or MySQL from the URL.",client:"HTTP and HTTPS client with cookies, proxies and streaming.",server:"HTTP server toolkit: parse, route, respond.",time:"Dates, times, durations, instants and named time zones.",zone:"Reads the system IANA time zone files.",sql:"SqlValue, Row and DbError, shared by every database module.",http:"Request, Response and the types server and client share.",json:"The Json tree, a native parser and helpers.",number:"Formatting, hex and binary encodings of numbers.",net:"Blocking TCP and TLS sockets.",fs:"Files, directories, lazy lines, chunks and walks."};
var g=document.getElementById("graph"),edges=[].slice.call(g.querySelectorAll(".edge")),nodes=[].slice.call(g.querySelectorAll(".node"));
var pinned=null;
function fmt(a){return a.length?a.join("  "):"nothing (only the prelude and its own natives)"}
function fmtBy(a){return a.length?a.join("  "):"no other module"}
function show(name){
  var d=DATA[name];g.classList.add("focus");
  nodes.forEach(function(n){var m=n.dataset.name;n.classList.toggle("on",m===name||d.uses.indexOf(m)>=0||d.usedBy.indexOf(m)>=0)});
  edges.forEach(function(e){e.classList.toggle("use",e.dataset.from===name);e.classList.toggle("usedby",e.dataset.to===name)});
  document.getElementById("d-name").textContent=name;
  document.getElementById("d-hint").textContent=HINTS[name.split('.').pop()]||("In lib/"+d.folder+".");
  document.getElementById("d-uses").textContent=fmt(d.uses);
  document.getElementById("d-by").textContent=fmtBy(d.usedBy);
}
function clear(){g.classList.remove("focus");nodes.forEach(function(n){n.classList.remove("on")});edges.forEach(function(e){e.classList.remove("use","usedby")});
  if(pinned)show(pinned)}
nodes.forEach(function(n){
  n.addEventListener("mouseenter",function(){show(n.dataset.name)});
  n.addEventListener("mouseleave",function(){if(pinned)show(pinned);else{g.classList.remove("focus");nodes.forEach(function(x){x.classList.remove("on")});edges.forEach(function(e){e.classList.remove("use","usedby")})}});
  n.addEventListener("focus",function(){show(n.dataset.name)});
  n.addEventListener("click",function(){pinned=(pinned===n.dataset.name)?null:n.dataset.name;pinned?show(pinned):clear()});
  n.addEventListener("keydown",function(ev){if(ev.key==="Enter"||ev.key===" "){ev.preventDefault();n.dispatchEvent(new Event("click"))}});
});
show("db");pinned="db";
</script>
''' % data
out=sys.argv[1] if len(sys.argv)>1 else os.path.join(ROOT,'docs','lib','library-map.html')
open(out,'w').write(html)
print(W,H,[len(r) for r in rows])
